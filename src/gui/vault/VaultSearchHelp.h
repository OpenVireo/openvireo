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

#ifndef KEEPASSXC_VAULTSEARCHHELP_H
#define KEEPASSXC_VAULTSEARCHHELP_H

#include <components/dialogs_flyouts/Flyout.h>

class QGridLayout;
class QVBoxLayout;

/**
 * Search help in the Vault Index layout: a flyout below the search box that
 * lists the fields, modifiers and wildcards, with examples to try.
 *
 * Clicking a field inserts it into the search, clicking an example runs it.
 */
class VaultSearchHelp : public fluent::dialogs_flyouts::Flyout
{
    Q_OBJECT

public:
    explicit VaultSearchHelp(QWidget* parent = nullptr);

signals:
    void insertRequested(const QString& text);
    void searchRequested(const QString& text);

private:
    void addSection(QVBoxLayout* layout, const QString& title);
    void addSyntaxRow(QGridLayout* grid, int row, int column, const QString& token, const QString& meaning);
};

#endif // KEEPASSXC_VAULTSEARCHHELP_H
