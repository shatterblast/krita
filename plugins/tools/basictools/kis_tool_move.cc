/*
 *  Copyright (c) 1999 Matthias Elter  <me@kde.org>
 *                1999 Michael Koch    <koch@kde.org>
 *                2002 Patrick Julien <freak@codepimps.org>
 *                2004 Boudewijn Rempt <boud@valdyas.org>
 *                2016 Michael Abrahams <miabraha@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_tool_move.h"

#include <QPoint>


#include "kis_cursor.h"
#include "kis_selection.h"
#include "kis_canvas2.h"
#include "kis_image.h"

#include "kis_tool_utils.h"
#include "kis_paint_layer.h"
#include "strokes/move_stroke_strategy.h"
#include "kis_tool_movetooloptionswidget.h"
#include "strokes/move_selection_stroke_strategy.h"
#include "kis_resources_snapshot.h"
#include "kis_action_registry.h"
#include "krita_utils.h"

#include <KisViewManager.h>
#include <KisDocument.h>

#include "kis_node_manager.h"
#include "kis_selection_manager.h"
#include "kis_signals_blocker.h"
#include <boost/operators.hpp>
#include "KisMoveBoundsCalculationJob.h"


struct KisToolMoveState : KisToolChangesTrackerData, boost::equality_comparable<KisToolMoveState>
{
    KisToolMoveState(QPoint _accumulatedOffset) : accumulatedOffset(_accumulatedOffset) {}
    KisToolChangesTrackerData* clone() const override { return new KisToolMoveState(*this); }

    bool operator ==(const KisToolMoveState &rhs) {
        return accumulatedOffset == rhs.accumulatedOffset;
    }

    QPoint accumulatedOffset;
};


KisToolMove::KisToolMove(KoCanvasBase *canvas)
    : KisTool(canvas, KisCursor::moveCursor())
    , m_updateCursorCompressor(100, KisSignalCompressor::FIRST_ACTIVE)
{
    setObjectName("tool_move");

    m_showCoordinatesAction = action("movetool-show-coordinates");
    m_showCoordinatesAction = action("movetool-show-coordinates");
    connect(&m_updateCursorCompressor, SIGNAL(timeout()), this, SLOT(resetCursorStyle()));

    m_optionsWidget = new MoveToolOptionsWidget(0, currentImage()->xRes(), toolId());

    // See https://bugs.kde.org/show_bug.cgi?id=316896
    QWidget *specialSpacer = new QWidget(m_optionsWidget);
    specialSpacer->setObjectName("SpecialSpacer");
    specialSpacer->setFixedSize(0, 0);
    m_optionsWidget->layout()->addWidget(specialSpacer);

    m_optionsWidget->setFixedHeight(m_optionsWidget->sizeHint().height());

    m_showCoordinatesAction->setChecked(m_optionsWidget->showCoordinates());

    m_optionsWidget->slotSetTranslate(m_handlesRect.topLeft() + currentOffset());

    connect(m_optionsWidget, SIGNAL(sigSetTranslateX(int)), SLOT(moveBySpinX(int)), Qt::UniqueConnection);
    connect(m_optionsWidget, SIGNAL(sigSetTranslateY(int)), SLOT(moveBySpinY(int)), Qt::UniqueConnection);
    connect(m_optionsWidget, SIGNAL(sigRequestCommitOffsetChanges()), this, SLOT(commitChanges()), Qt::UniqueConnection);

    connect(this, SIGNAL(moveInNewPosition(QPoint)), m_optionsWidget, SLOT(slotSetTranslate(QPoint)), Qt::UniqueConnection);

    connect(qobject_cast<KisCanvas2*>(canvas)->viewManager()->nodeManager(), SIGNAL(sigUiNeedChangeSelectedNodes(KisNodeList)), this, SLOT(slotNodeChanged(KisNodeList)), Qt::UniqueConnection);
    connect(qobject_cast<KisCanvas2*>(canvas)->viewManager()->selectionManager(), SIGNAL(currentSelectionChanged()), this, SLOT(slotSelectionChanged()), Qt::UniqueConnection);
}

KisToolMove::~KisToolMove()
{
    endStroke();
}

void KisToolMove::resetCursorStyle()
{
    KisTool::resetCursorStyle();

    if (!isActive()) return;
    KisImageSP image = this->image();
    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image, currentNode(), canvas()->resourceManager());
    KisSelectionSP selection = resources->activeSelection();
    KisNodeList nodes = fetchSelectedNodes(moveToolMode(), &m_lastCursorPos, selection);

    if (nodes.isEmpty()) {
        canvas()->setCursor(Qt::ForbiddenCursor);
    }
}

KisNodeList KisToolMove::fetchSelectedNodes(MoveToolMode mode, const QPoint *pixelPoint, KisSelectionSP selection)
{
    KisNodeList nodes;

    KisImageSP image = this->image();
    if (mode != MoveSelectedLayer && pixelPoint) {
        const bool wholeGroup = !selection &&  mode == MoveGroup;
        KisNodeSP node = KisToolUtils::findNode(image->root(), *pixelPoint, wholeGroup);
        if (node) {
            nodes = {node};
        }
    }

    if (nodes.isEmpty()) {
        nodes = this->selectedNodes();

        KritaUtils::filterContainer<KisNodeList>(nodes,
                                                 [](KisNodeSP node) {
                                                     return node->isEditable();
                                                 });
    }

    return nodes;
}

bool KisToolMove::startStrokeImpl(MoveToolMode mode, const QPoint *pos)
{
    KisNodeSP node;
    KisImageSP image = this->image();

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image, currentNode(), canvas()->resourceManager());
    KisSelectionSP selection = resources->activeSelection();

    KisNodeList nodes = fetchSelectedNodes(mode, pos, selection);

    if (nodes.size() == 1) {
        node = nodes.first();
    }

    if (nodes.isEmpty()) {
        return false;
    }

    /**
     * If the target node has changed, the stroke should be
     * restarted. Otherwise just continue processing current node.
     */
    if (m_strokeId && !tryEndPreviousStroke(nodes)) {
        return true;
    }

    KisStrokeStrategy *strategy;

    KisPaintLayerSP paintLayer = node ?
        dynamic_cast<KisPaintLayer*>(node.data()) : 0;

    bool isMoveSelection = false;

    if (paintLayer && selection &&
        (!selection->selectedRect().isEmpty() &&
         !selection->selectedExactRect().isEmpty())) {

        MoveSelectionStrokeStrategy *moveStrategy =
            new MoveSelectionStrokeStrategy(paintLayer,
                                            selection,
                                            image.data(),
                                            image.data());

        connect(moveStrategy,
                SIGNAL(sigHandlesRectCalculated(const QRect&)),
                SLOT(slotHandlesRectCalculated(const QRect&)));

        strategy = moveStrategy;
        isMoveSelection = true;

    } else {

        MoveStrokeStrategy *moveStrategy =
            new MoveStrokeStrategy(nodes, image.data(), image.data());
        connect(moveStrategy,
                SIGNAL(sigHandlesRectCalculated(const QRect&)),
                SLOT(slotHandlesRectCalculated(const QRect&)));

        strategy = moveStrategy;
    }

    // disable outline feedback until the stroke calcualtes
    // correct bounding rect
    m_handlesRect = QRect();
    m_strokeId = image->startStroke(strategy);
    m_currentlyProcessingNodes = nodes;
    m_accumulatedOffset = QPoint();

    if (!isMoveSelection) {
        m_asyncUpdateHelper.startUpdateStream(image.data(), m_strokeId);
    }

    KIS_SAFE_ASSERT_RECOVER(m_changesTracker.isEmpty()) {
        m_changesTracker.reset();
    }
    commitChanges();

    return true;
}

QPoint KisToolMove::currentOffset() const
{
    return m_accumulatedOffset + m_dragPos - m_dragStart;
}

void KisToolMove::notifyGuiAfterMove(bool showFloatingMessage)
{
    if (!m_optionsWidget) return;
    if (m_handlesRect.isEmpty()) return;

    const QPoint currentTopLeft = m_handlesRect.topLeft() + currentOffset();

    KisSignalsBlocker b(m_optionsWidget);
    emit moveInNewPosition(currentTopLeft);

    // TODO: fetch this info not from options widget, but from config
    const bool showCoordinates = m_optionsWidget->showCoordinates();

    if (showCoordinates && showFloatingMessage) {
        KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2*>(canvas());
        kisCanvas->viewManager()->
            showFloatingMessage(
                i18nc("floating message in move tool",
                      "X: %1 px, Y: %2 px",
                      QLocale().toString(currentTopLeft.x()),
                      QLocale().toString(currentTopLeft.y())),
                QIcon(), 1000, KisFloatingMessage::High);
    }
}

bool KisToolMove::tryEndPreviousStroke(const KisNodeList &nodes)
{
    if (!m_strokeId) return false;

    bool strokeEnded = false;

    if (!KritaUtils::compareListsUnordered(nodes, m_currentlyProcessingNodes)) {
        endStroke();
        strokeEnded = true;
    }

    return strokeEnded;
}

void KisToolMove::commitChanges()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_strokeId);

    QSharedPointer<KisToolMoveState> newState(new KisToolMoveState(m_accumulatedOffset));
    KisToolMoveState *lastState = dynamic_cast<KisToolMoveState*>(m_changesTracker.lastState().data());
    if (lastState && *lastState == *newState) return;

    m_changesTracker.commitConfig(newState);
}

void KisToolMove::slotHandlesRectCalculated(const QRect &handlesRect)
{
    m_handlesRect = handlesRect;
    notifyGuiAfterMove(false);
}

void KisToolMove::moveDiscrete(MoveDirection direction, bool big)
{
    if (mode() == KisTool::PAINT_MODE) return;  // Don't interact with dragging
    if (!currentNode()) return;
    if (!image()) return;
    if (!currentNode()->isEditable()) return; // Don't move invisible nodes

    if (startStrokeImpl(MoveSelectedLayer, 0)) {
        setMode(KisTool::PAINT_MODE);
    }

    // Larger movement if "shift" key is pressed.
    qreal scale = big ? m_optionsWidget->moveScale() : 1.0;
    qreal moveStep = m_optionsWidget->moveStep() * scale;

    const QPoint offset =
        direction == Up   ? QPoint( 0, -moveStep) :
        direction == Down ? QPoint( 0,  moveStep) :
        direction == Left ? QPoint(-moveStep,  0) :
        QPoint( moveStep,  0) ;

    m_accumulatedOffset += offset;
    image()->addJob(m_strokeId, new MoveStrokeStrategy::Data(m_accumulatedOffset));

    notifyGuiAfterMove();
    commitChanges();
    setMode(KisTool::HOVER_MODE);
}

void KisToolMove::activate(ToolActivation toolActivation, const QSet<KoShape*> &shapes)
{
    KisTool::activate(toolActivation, shapes);

    m_actionConnections.addConnection(action("movetool-move-up"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteUp()));
    m_actionConnections.addConnection(action("movetool-move-down"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteDown()));
    m_actionConnections.addConnection(action("movetool-move-left"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteLeft()));
    m_actionConnections.addConnection(action("movetool-move-right"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteRight()));

    m_actionConnections.addConnection(action("movetool-move-up-more"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteUpMore()));
    m_actionConnections.addConnection(action("movetool-move-down-more"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteDownMore()));
    m_actionConnections.addConnection(action("movetool-move-left-more"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteLeftMore()));
    m_actionConnections.addConnection(action("movetool-move-right-more"), SIGNAL(triggered(bool)),
                                      this, SLOT(slotMoveDiscreteRightMore()));

    connect(m_showCoordinatesAction, SIGNAL(triggered(bool)), m_optionsWidget, SLOT(setShowCoordinates(bool)), Qt::UniqueConnection);
    connect(m_optionsWidget, SIGNAL(showCoordinatesChanged(bool)), m_showCoordinatesAction, SLOT(setChecked(bool)), Qt::UniqueConnection);

    connect(&m_changesTracker,
            SIGNAL(sigConfigChanged(KisToolChangesTrackerDataSP)),
            SLOT(slotTrackerChangedConfig(KisToolChangesTrackerDataSP)));


    slotNodeChanged(this->selectedNodes());
}



void KisToolMove::paint(QPainter& gc, const KoViewConverter &converter)
{
    Q_UNUSED(converter);

    if (m_strokeId && !m_handlesRect.isEmpty()) {
        QPainterPath handles;
        handles.addRect(m_handlesRect.translated(currentOffset()));

        QPainterPath path = pixelToView(handles);
        paintToolOutline(&gc, path);
    }
}

void KisToolMove::deactivate()
{
    m_actionConnections.clear();

    disconnect(m_showCoordinatesAction, 0, this, 0);
    disconnect(m_optionsWidget, 0, this, 0);

    endStroke();
    KisTool::deactivate();
}

void KisToolMove::requestStrokeEnd()
{
    endStroke();
}

void KisToolMove::requestStrokeCancellation()
{
    cancelStroke();
}

void KisToolMove::requestUndoDuringStroke()
{
    if (!m_strokeId) return;

    if (m_changesTracker.isEmpty()) {
        cancelStroke();
    } else {
        m_changesTracker.requestUndo();
    }
}

void KisToolMove::beginPrimaryAction(KoPointerEvent *event)
{
    startAction(event, moveToolMode());
}

void KisToolMove::continuePrimaryAction(KoPointerEvent *event)
{
    continueAction(event);
}

void KisToolMove::endPrimaryAction(KoPointerEvent *event)
{
    endAction(event);
}

void KisToolMove::beginAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    // Ctrl+Right click toggles between moving current layer and moving layer w/ content
    if (action == PickFgNode || action == PickBgImage) {
        MoveToolMode mode = moveToolMode();

        if (mode == MoveSelectedLayer) {
            mode = MoveFirstLayer;
        } else if (mode == MoveFirstLayer) {
            mode = MoveSelectedLayer;
        }

        startAction(event, mode);
    } else {
        startAction(event, MoveGroup);
    }
}

void KisToolMove::continueAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    Q_UNUSED(action)
    continueAction(event);
}

void KisToolMove::endAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    Q_UNUSED(action)
    endAction(event);
}

void KisToolMove::mouseMoveEvent(KoPointerEvent *event)
{
    m_lastCursorPos = convertToPixelCoord(event).toPoint();
    KisTool::mouseMoveEvent(event);

    if (moveToolMode() == MoveFirstLayer) {
        m_updateCursorCompressor.start();
    }
}

void KisToolMove::startAction(KoPointerEvent *event, MoveToolMode mode)
{
    QPoint pos = convertToPixelCoordAndSnap(event).toPoint();
    m_dragStart = pos;
    m_dragPos = pos;

    if (startStrokeImpl(mode, &pos)) {
        setMode(KisTool::PAINT_MODE);
    } else {
        event->ignore();
        m_dragPos = QPoint();
        m_dragStart = QPoint();
    }
    qobject_cast<KisCanvas2*>(canvas())->updateCanvas();
}

void KisToolMove::continueAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    if (!m_strokeId) return;

    QPoint pos = convertToPixelCoordAndSnap(event).toPoint();
    pos = applyModifiers(event->modifiers(), pos);
    m_dragPos = pos;

    drag(pos);
    notifyGuiAfterMove();

    qobject_cast<KisCanvas2*>(canvas())->updateCanvas();
}

void KisToolMove::endAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    setMode(KisTool::HOVER_MODE);
    if (!m_strokeId) return;

    QPoint pos = convertToPixelCoordAndSnap(event).toPoint();
    pos = applyModifiers(event->modifiers(), pos);
    drag(pos);

    m_accumulatedOffset += pos - m_dragStart;
    m_dragStart = QPoint();
    m_dragPos = QPoint();
    commitChanges();

    notifyGuiAfterMove();

    qobject_cast<KisCanvas2*>(canvas())->updateCanvas();
}

void KisToolMove::drag(const QPoint& newPos)
{
    KisImageWSP image = currentImage();

    QPoint offset = m_accumulatedOffset + newPos - m_dragStart;

    image->addJob(m_strokeId,
                  new MoveStrokeStrategy::Data(offset));
}

void KisToolMove::endStroke()
{
    if (!m_strokeId) return;

    if (m_asyncUpdateHelper.isActive()) {
        m_asyncUpdateHelper.endUpdateStream();
    }

    KisImageSP image = currentImage();
    image->endStroke(m_strokeId);
    m_strokeId.clear();
    m_changesTracker.reset();
    m_currentlyProcessingNodes.clear();
    m_accumulatedOffset = QPoint();
    qobject_cast<KisCanvas2*>(canvas())->updateCanvas();
}

void KisToolMove::slotTrackerChangedConfig(KisToolChangesTrackerDataSP state)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_strokeId);

    KisToolMoveState *newState = dynamic_cast<KisToolMoveState*>(state.data());
    KIS_SAFE_ASSERT_RECOVER_RETURN(newState);

    if (mode() == KisTool::PAINT_MODE) return;  // Don't interact with dragging
    m_accumulatedOffset = newState->accumulatedOffset;
    image()->addJob(m_strokeId, new MoveStrokeStrategy::Data(m_accumulatedOffset));
    notifyGuiAfterMove();
}

void KisToolMove::slotMoveDiscreteLeft()
{
    moveDiscrete(MoveDirection::Left, false);
}

void KisToolMove::slotMoveDiscreteRight()
{
    moveDiscrete(MoveDirection::Right, false);
}

void KisToolMove::slotMoveDiscreteUp()
{
    moveDiscrete(MoveDirection::Up, false);
}

void KisToolMove::slotMoveDiscreteDown()
{
    moveDiscrete(MoveDirection::Down, false);
}

void KisToolMove::slotMoveDiscreteLeftMore()
{
    moveDiscrete(MoveDirection::Left, true);
}

void KisToolMove::slotMoveDiscreteRightMore()
{
    moveDiscrete(MoveDirection::Right, true);
}

void KisToolMove::slotMoveDiscreteUpMore()
{
    moveDiscrete(MoveDirection::Up, true);
}

void KisToolMove::slotMoveDiscreteDownMore()
{
    moveDiscrete(MoveDirection::Down, true);
}

void KisToolMove::cancelStroke()
{
    if (!m_strokeId) return;

    if (m_asyncUpdateHelper.isActive()) {
        m_asyncUpdateHelper.cancelUpdateStream();
    }

    KisImageSP image = currentImage();
    image->cancelStroke(m_strokeId);
    m_strokeId.clear();
    m_changesTracker.reset();
    m_currentlyProcessingNodes.clear();
    m_accumulatedOffset = QPoint();
    notifyGuiAfterMove();
    qobject_cast<KisCanvas2*>(canvas())->updateCanvas();
}

QWidget* KisToolMove::createOptionWidget()
{
    return m_optionsWidget;
}

KisToolMove::MoveToolMode KisToolMove::moveToolMode() const
{
    if (m_optionsWidget)
        return m_optionsWidget->mode();
    return MoveSelectedLayer;
}

QPoint KisToolMove::applyModifiers(Qt::KeyboardModifiers modifiers, QPoint pos)
{
    QPoint move = pos - m_dragStart;

    // Snap to axis
    if (modifiers & Qt::ShiftModifier) {
        move = snapToClosestAxis(move);
    }

    // "Precision mode" - scale down movement by 1/5
    if (modifiers & Qt::AltModifier) {
        const qreal SCALE_FACTOR = .2;
        move = SCALE_FACTOR * move;
    }

    return m_dragStart + move;
}

void KisToolMove::moveBySpinX(int newX)
{
    if (mode() == KisTool::PAINT_MODE) return;  // Don't interact with dragging
    if (!currentNode()->isEditable()) return; // Don't move invisible nodes

    if (startStrokeImpl(MoveSelectedLayer, 0)) {
        setMode(KisTool::PAINT_MODE);
    }

    m_accumulatedOffset.rx() =  newX - m_handlesRect.x();

    image()->addJob(m_strokeId, new MoveStrokeStrategy::Data(m_accumulatedOffset));

    notifyGuiAfterMove(false);
    setMode(KisTool::HOVER_MODE);
}

void KisToolMove::moveBySpinY(int newY)
{
    if (mode() == KisTool::PAINT_MODE) return;  // Don't interact with dragging
    if (!currentNode()->isEditable()) return; // Don't move invisible nodes

    if (startStrokeImpl(MoveSelectedLayer, 0)) {
        setMode(KisTool::PAINT_MODE);
    }

    m_accumulatedOffset.ry() =  newY - m_handlesRect.y();

    image()->addJob(m_strokeId, new MoveStrokeStrategy::Data(m_accumulatedOffset));

    notifyGuiAfterMove(false);
    setMode(KisTool::HOVER_MODE);
}

void KisToolMove::requestHandlesRectUpdate()
{
    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image(), currentNode(), canvas()->resourceManager());
    KisSelectionSP selection = resources->activeSelection();

    KisMoveBoundsCalculationJob *job = new KisMoveBoundsCalculationJob(this->selectedNodes(),
                                                                       selection, this);
    connect(job,
            SIGNAL(sigCalcualtionFinished(const QRect&)),
            SLOT(slotHandlesRectCalculated(const QRect &)));

    KisImageSP image = this->image();
    image->addSpontaneousJob(job);

    notifyGuiAfterMove(false);
}

void KisToolMove::slotNodeChanged(const KisNodeList &nodes)
{
    if (m_strokeId && !tryEndPreviousStroke(nodes)) {
        return;
    }
    requestHandlesRectUpdate();
}

void KisToolMove::slotSelectionChanged()
{
    if (m_strokeId) return;
    requestHandlesRectUpdate();
}

QList<QAction *> KisToolMoveFactory::createActionsImpl()
{
    KisActionRegistry *actionRegistry = KisActionRegistry::instance();
    QList<QAction *> actions = KisToolPaintFactoryBase::createActionsImpl();

    actions << actionRegistry->makeQAction("movetool-move-up");
    actions << actionRegistry->makeQAction("movetool-move-down");
    actions << actionRegistry->makeQAction("movetool-move-left");
    actions << actionRegistry->makeQAction("movetool-move-right");
    actions << actionRegistry->makeQAction("movetool-move-up-more");
    actions << actionRegistry->makeQAction("movetool-move-down-more");
    actions << actionRegistry->makeQAction("movetool-move-left-more");
    actions << actionRegistry->makeQAction("movetool-move-right-more");
    actions << actionRegistry->makeQAction("movetool-show-coordinates");

    return actions;

}
