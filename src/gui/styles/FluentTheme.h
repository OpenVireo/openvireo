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

#ifndef KEEPASSXC_FLUENTTHEME_H
#define KEEPASSXC_FLUENTTHEME_H

#include <QColor>
#include <QPalette>

class QAction;
class QPainter;
class QPainterPath;
class QString;

/**
 * Bridge between KeePassXC's theme and Fluent-Qt.
 *
 * All brand colors used by the Vault Index layout live here so the Fluent-Qt
 * components and the KeePassXC widgets drawn next to them stay consistent.
 */
namespace FluentTheme
{
    // Point Fluent-Qt at the light or dark palette and the accent, and install the matching application palette
    // and style (VaultStyle) for the stock widgets
    void apply(bool dark);

    /**
     * Base colours of a high-contrast scheme, named after the Windows system
     * colours they come from (COLOR_WINDOW, COLOR_WINDOWTEXT, ...). Everything
     * the high-contrast theme paints is derived from these.
     */
    struct HighContrastColors
    {
        QColor window;
        QColor windowText;
        QColor highlight;
        QColor highlightText;
        QColor buttonFace;
        QColor buttonText;
        QColor grayText;
        // Hyperlinks
        QColor hotlight;
        // Status text; Windows has no system colours for these
        QColor critical;
        QColor caution;
        QColor success;

        // True for light text on a dark window
        bool isDark() const;
    };
    // The fixed light-on-dark or dark-on-light scheme used when the OS doesn't supply its own colours.
    // Text reaches WCAG AAA (7:1) against the window, control borders and focus rings 3:1.
    HighContrastColors highContrastColors(bool dark);
    // Status colours for a scheme whose base colours came from the OS, checked against its window colour
    void fillStatusColors(HighContrastColors& colors);
    // Switch Fluent-Qt and the semantic palette to a high-contrast scheme: solid colours only, no motion
    void applyHighContrast(const HighContrastColors& colors);
    // True while the high-contrast theme is active
    bool isHighContrast();
    // The scheme applied by applyHighContrast(), or the fixed dark one when it isn't active
    HighContrastColors currentHighContrastColors();

    // WCAG 2 contrast ratio between two opaque colours (1 to 21)
    qreal contrastRatio(const QColor& a, const QColor& b);

    // Semantic colors for KeePassXC widgets painted alongside Fluent-Qt components
    struct Palette
    {
        // Window background behind the navigation pane
        QColor canvas;
        // Content area beside the navigation pane
        QColor surface;
        // Cards
        QColor layer;
        // Recessed areas inside a card, such as field groups and icon tiles
        QColor inset;
        QColor stroke;
        QColor strokeStrong;
        QColor divider;
        // Fills of standard controls, such as buttons and fields
        QColor control;
        QColor controlHover;
        QColor controlPressed;
        QColor subtleHover;
        QColor subtlePressed;
        QColor text;
        QColor textSecondary;
        QColor textTertiary;
        QColor textDisabled;
        QColor accent;
        QColor accentText;
        QColor accentSubtle;
        // Text and glyphs on an accent or status fill
        QColor onAccent;
        QColor critical;
        QColor criticalBg;
        QColor caution;
        QColor cautionBg;
        QColor success;
        QColor successBg;
        QColor infoBg;
        // Hyperlink text
        QColor link;
        // Keyboard focus ring, drawn 2px wide
        QColor focus;
        // Outline around the selected row; only drawn in high contrast, where a fill alone isn't enough
        QColor selectionStroke;
        // High contrast is active: draw borders around state, never rely on a fill or colour alone
        bool highContrast = false;
    };
    Palette palette();
    // The QPalette for the stock Qt widgets the Fusion style draws, derived from palette(). In high contrast it is the
    // scheme itself, Active and Inactive alike. apply() and applyHighContrast() make it the application palette.
    QPalette qtPalette();
    // True while Fluent-Qt uses its dark appearance
    bool isDark();

    // In high contrast, outline a list row so hover and selection don't rely on its fill:
    // 2px selectionStroke when selected, 1px text colour when only hovered. Does nothing otherwise.
    void paintRowStateOutline(QPainter* painter, const QPainterPath& row, bool selected, bool hovered);

    // Look up one of the main window actions registered with the ActionCollection
    QAction* action(const QString& objectName);
} // namespace FluentTheme

#endif // KEEPASSXC_FLUENTTHEME_H
