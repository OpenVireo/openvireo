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

#ifndef KEEPASSXC_EMERGENCYKITDIALOG_H
#define KEEPASSXC_EMERGENCYKITDIALOG_H

#include <QDialog>

class AccountManager;
class MessageWidget;
class QLabel;
class QPushButton;

// Account › Emergency Kit: the Secret Key this device keeps, shown again with Copy and Save, as on the
// web app's Account panel and iOS Settings › Account › Emergency Kit
class EmergencyKitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EmergencyKitDialog(AccountManager* account, QWidget* parent = nullptr);
    ~EmergencyKitDialog() override;

private slots:
    void copyKey();
    void saveKit();

private:
    QString m_email;
    QString m_server;
    QString m_secretKey;
    MessageWidget* m_message;
    QLabel* m_secretKeyLabel;
    QPushButton* m_copyButton;
    QPushButton* m_saveButton;
};

#endif // KEEPASSXC_EMERGENCYKITDIALOG_H
