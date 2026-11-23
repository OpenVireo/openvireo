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

#ifndef KEEPASSXC_VAULTMETRICS_H
#define KEEPASSXC_VAULTMETRICS_H

// Corner radii of the vault UI, matching the web app (dev guide 06-reference/app-design-tokens.md).
// Controls (6) and dialogs and flyouts (12) come from Fluent-Qt's themeRadius(); FluentTheme sets them.
namespace VaultMetrics
{
    // The main surface: view title, entry list and details
    constexpr int SurfaceRadius = 20;
    // Cards on the surface: the entry list, the details card, settings cards, modals
    constexpr int CardRadius = 18;
    // Panels inside a card: the field list in the details card, info bars
    constexpr int PanelRadius = 16;
    // List and navigation rows
    constexpr int RowRadius = 12;
    // Pills: badges, the search box, labelled buttons
    constexpr int PillRadius = 999;
} // namespace VaultMetrics

#endif // KEEPASSXC_VAULTMETRICS_H
