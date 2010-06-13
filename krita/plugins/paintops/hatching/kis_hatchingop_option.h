/*
 *  Copyright (c) 2008 Lukas Tvrdy <lukast.dev@gmail.com>
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
#ifndef KIS_HATCHINGOP_OPTION_H
#define KIS_HATCHINGOP_OPTION_H

#include <kis_paintop_option.h>
#include <krita_export.h>

const QString HATCHING_ANGLE = "Hatching/angle";
const QString HATCHING_WIDTH = "Hatching/width";
const QString HATCHING_HEIGHT = "Hatching/height";
const QString HATCHING_SEPARATION = "Hatching/separation";
const QString HATCHING_THICKNESS = "Hatching/thickness";

class KisHatchingOpOptionsWidget;

class KisHatchingOpOption : public KisPaintOpOption
{
public:
    KisHatchingOpOption();
    ~KisHatchingOpOption();

   
    void writeOptionSetting(KisPropertiesConfiguration* setting) const;
    void readOptionSetting(const KisPropertiesConfiguration* setting);


private:

    KisHatchingOpOptionsWidget * m_options;

};

#endif
