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

#ifndef KEEPASSXC_AUTOFILLSETTINGSWIDGET_H
#define KEEPASSXC_AUTOFILLSETTINGSWIDGET_H

#include <QWidget>

class MessageWidget;
class QAbstractButton;
class QLabel;
class QListWidget;
class VaultButton;
class VaultCheckBox;

class AutofillSettingsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AutofillSettingsWidget(QWidget* parent = nullptr);
    ~AutofillSettingsWidget() override = default;

    void loadSettings();
    void saveSettings();

private slots:
    void updateEnabledState();
    void updateProviderStatus();

private:
    void buildVaultPage();

    MessageWidget* m_statusMessage;
    VaultCheckBox* m_enableCheckBox;
    VaultCheckBox* m_keepIdentitiesCheckBox;
    VaultButton* m_openSettingsButton;
    QLabel* m_databasesLabel;
    QListWidget* m_databasesList;
};

#endif // KEEPASSXC_AUTOFILLSETTINGSWIDGET_H
