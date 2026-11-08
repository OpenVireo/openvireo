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

#ifndef KEEPASSXC_DELETEACCOUNTDIALOG_H
#define KEEPASSXC_DELETEACCOUNTDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class AccountManager;
class MessageWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// Account › Delete Account… (SPEC §23): what happens to each vault, organization, the plan and share
// links; organizations you are the only owner of to hand over or delete first, and shared vaults to give
// away; then the typed email and master password. The account is locked at once and erased after the
// grace period.
class DeleteAccountDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeleteAccountDialog(AccountManager* account, QWidget* parent = nullptr);

    // One line per thing that changes, for the summary
    static QStringList summary(const QJsonObject& preview);

private slots:
    void reload();
    void deleteAccount();

private:
    void fill();
    void setBusy(bool busy);
    void updateDeleteButton();

    QPointer<AccountManager> m_account;
    QJsonObject m_preview;
    MessageWidget* m_message;
    QLabel* m_intro;
    QLabel* m_summary;
    QWidget* m_actions;
    QVBoxLayout* m_actionsLayout;
    QLineEdit* m_confirm;
    QLineEdit* m_password;
    QPushButton* m_delete;
    bool m_busy = false;
};

// Signed in to an account scheduled for deletion: Restore Account (with the master password) or Sign Out.
class PendingDeletionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PendingDeletionDialog(AccountManager* account, QWidget* parent = nullptr);

private:
    QPointer<AccountManager> m_account;
    MessageWidget* m_message;
    QLineEdit* m_password;
    QPushButton* m_restore;
};

#endif // KEEPASSXC_DELETEACCOUNTDIALOG_H
