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

#ifndef KEEPASSXC_ACCOUNTLOCKWIDGET_H
#define KEEPASSXC_ACCOUNTLOCKWIDGET_H

#include <QPointer>
#include <QWidget>

class AccountController;
class MessageWidget;
class QAction;
class QFrame;
class QLabel;
class VaultButton;
class VaultLineEdit;

/**
 * The lock screen of a signed-in account, shown in the welcome screen's place while the account is
 * locked: who is signed in and where, Quick Unlock when this session already unlocked once, and the
 * master password. Wrong passwords and an unreachable server show on the card itself. Without a
 * connection the account unlocks from this device's offline cache (SPEC §20).
 *
 * Quick Unlock never prompts by itself: the screen also shows after an idle lock, and a Touch ID
 * prompt appearing on its own then would be surprising.
 */
class AccountLockWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AccountLockWidget(QWidget* parent = nullptr);
    ~AccountLockWidget() override;

    void setController(AccountController* controller);
    // Fill in the account, clear the form, and focus Quick Unlock or the password
    void reset();
    // "Go to open databases" instead of "Open a local database" while other databases are open
    void setDatabasesOpen(bool open);

signals:
    // Leave the lock screen for the welcome screen or the open databases; the account stays locked
    void dismissRequested();
    void switchAccountRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void unlock();
    void quickUnlock();
    void updateState();
    void handleError(const QString& message);
    void signOut();

private:
    void applyTheme();
    void updateLinks();
    void setBusy(bool busy);
    bool needsSignIn() const;

    QPointer<AccountController> m_controller;
    QFrame* m_card;
    QLabel* m_tile;
    QLabel* m_headline;
    QLabel* m_accountLabel;
    QLabel* m_serverLabel;
    VaultButton* m_quickUnlockButton;
    QLabel* m_orLabel;
    QLabel* m_passwordLabel;
    VaultLineEdit* m_passwordEdit;
    QAction* m_revealAction;
    MessageWidget* m_messageWidget;
    QLabel* m_busyLabel;
    VaultButton* m_unlockButton;
    QLabel* m_recoverLink;
    QLabel* m_dismissLink;
    QLabel* m_switchLink;
    QLabel* m_signOutLink;
    bool m_databasesOpen = false;
};

#endif // KEEPASSXC_ACCOUNTLOCKWIDGET_H
