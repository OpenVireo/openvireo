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

#ifndef OPENVIREO_EMERGENCYKITDIALOG_H
#define OPENVIREO_EMERGENCYKITDIALOG_H

#include <QDialog>

class AccountManager;
class VaultButton;
class VaultInfoBar;
class VaultPill;
class QLabel;
class QPushButton;

// Account › Emergency Kit: the Secret Key this device keeps, shown again with Copy and Save, as on the
// web app's Account panel and iOS Settings › Account › Emergency Kit. Over the main window it is a card
// (VaultModal); the key stays masked until Show.
class EmergencyKitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EmergencyKitDialog(AccountManager* account, QWidget* parent = nullptr);
    ~EmergencyKitDialog() override;

    // Opens the dialog as a card over the parent's window, or as a window when it can't be one
    static EmergencyKitDialog* present(AccountManager* account, QWidget* parent);

private slots:
    void copyKey();
    void saveKit();
    void toggleKey();

private:
    void showKey();
    void applyColors();

    QString m_email;
    QString m_server;
    QString m_secretKey;
    bool m_revealed = false;
    VaultInfoBar* m_message;
    QLabel* m_intro;
    QLabel* m_keyCaption;
    VaultPill* m_keyChip;
    QLabel* m_secretKeyLabel;
    QLabel* m_note;
    QList<QLabel*> m_factCaptions;
    QList<QLabel*> m_factValues;
    QWidget* m_buttonRow;
    VaultButton* m_revealButton;
    QPushButton* m_copyButton;
    QPushButton* m_saveButton;
    QPushButton* m_closeButton;
};

#endif // OPENVIREO_EMERGENCYKITDIALOG_H
