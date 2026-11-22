/*
 *  Copyright (C) 2026 Bongani Mbigi
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OPENVIREO_HIGHCONTRASTSTYLE_H
#define OPENVIREO_HIGHCONTRASTSTYLE_H

#include "gui/styles/FluentTheme.h"
#include "gui/styles/base/BaseStyle.h"

/**
 * Paints the stock Qt widgets that aren't Fluent-Qt components in the
 * high-contrast theme: a QPalette made from the scheme's base colours and
 * solid, text-coloured outlines on every control.
 */
class HighContrastStyle : public BaseStyle
{
    Q_OBJECT

public:
    explicit HighContrastStyle(const FluentTheme::HighContrastColors& colors);
    QPalette standardPalette() const override;

    using BaseStyle::polish;
    void polish(QWidget* widget) override;

    static QPalette paletteFor(const FluentTheme::HighContrastColors& colors);

protected:
    QString getAppStyleSheet() const override;

private:
    FluentTheme::HighContrastColors m_colors;
};

#endif // OPENVIREO_HIGHCONTRASTSTYLE_H
