/*
 *  Copyright (c) 2011 Lukáš Tvrdý <lukast.dev@gmail.com>
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

#include "kis_linewidth_option.h"
#include <klocale.h>

KisLineWidthOption::KisLineWidthOption()
    : KisCurveOption(i18n("Line width"), "Line width", KisPaintOpOption::commonCategory(), false)
{
    setMinimumLabel(i18n("0%"));
    setMaximumLabel(i18n("100%"));
}


double KisLineWidthOption::apply(const KisPaintInformation & info, double lineWidth) const
{
    if (!isChecked()) {
        return lineWidth;
    }
    return computeValue(info) * lineWidth;
}
