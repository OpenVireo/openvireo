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

#ifndef KEEPASSXC_VAULTSETTINGSMODAL_H
#define KEEPASSXC_VAULTSETTINGSMODAL_H

#include "VaultModal.h"

class EditWidget;
class QListView;

/**
 * Settings in a modal card over the main window, with a sidebar of categories
 * and Cancel / Save in the footer, after Slack's preferences.
 *
 * The settings widget keeps its own pages and logic; the modal only hides its
 * category list and buttons and drives them from its own controls.
 */
class VaultSettingsModal : public VaultModal
{
    Q_OBJECT

public:
    VaultSettingsModal(QWidget* window, EditWidget* settings);

    QListView* navigation() const;

protected:
    void accept() override;
    void reject() override;
    void prepare() override;

private:
    QPointer<EditWidget> m_settings;
    QListView* m_navigation;
};

#endif // KEEPASSXC_VAULTSETTINGSMODAL_H
