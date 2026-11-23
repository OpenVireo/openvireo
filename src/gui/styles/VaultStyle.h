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

#ifndef OPENVIREO_VAULTSTYLE_H
#define OPENVIREO_VAULTSTYLE_H

#include <QProxyStyle>

/**
 * The application style: Qt's Fusion for the stock widgets that aren't Fluent-Qt components, coloured by
 * FluentTheme::qtPalette(), with a few Fluent touches on top: overlay scroll bars, a small chevron as the
 * header sort indicator and outlined tooltips. In high contrast every control gets a solid outline in
 * the text colour, since Fusion derives its outlines from the window colour and they vanish on black.
 */
class VaultStyle : public QProxyStyle
{
    Q_OBJECT

public:
    VaultStyle();

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element,
                     const QStyleOption* option,
                     QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex* option,
                            QPainter* painter,
                            const QWidget* widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const override;
    int styleHint(StyleHint hint,
                  const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
};

#endif // OPENVIREO_VAULTSTYLE_H
