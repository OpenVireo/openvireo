/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
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

class QAction;
class QString;

/**
 * Bridge between KeePassXC's theme and Fluent-Qt.
 *
 * All brand colors used by the Vault Index layout live here so the Fluent-Qt
 * components and the KeePassXC widgets drawn next to them stay consistent.
 */
namespace FluentTheme
{
    // True when the Vault Index main window layout is enabled
    bool vaultLayoutEnabled();

    // Point Fluent-Qt at the light or dark palette and the KeePassXC accent
    void apply(bool dark);

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
        QColor accent;
        QColor accentText;
        QColor accentSubtle;
        QColor critical;
        QColor criticalBg;
        QColor caution;
        QColor cautionBg;
        QColor success;
        QColor successBg;
    };
    Palette palette();
    // True while Fluent-Qt uses its dark appearance
    bool isDark();

    // Look up one of the main window actions registered with the ActionCollection
    QAction* action(const QString& objectName);
} // namespace FluentTheme

#endif // KEEPASSXC_FLUENTTHEME_H
