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

#ifndef KEEPASSXC_ACCOUNTSTARTWIDGET_H
#define KEEPASSXC_ACCOUNTSTARTWIDGET_H

#include <QPointer>
#include <QScopedPointer>
#include <QWidget>

class AccountController;

namespace Ui
{
    class AccountStartWidget;
}

/**
 * The start screen, shown before the welcome screen until a mode is chosen (and again after signing out):
 *
 * - sign in to the hosted account: email, master password, and the Secret Key on a device that hasn't
 *   signed in to the account before; then the TOTP code when the account has one. Only a wrong code
 *   (invalid_mfa) keeps asking for it, anything else goes back to signing in;
 * - Create account, which opens the sign-up wizard;
 * - Use my own sync server: its URL and a pairing code, then the welcome screen with that server
 *   pre-filled for new Remote Sync settings;
 * - Use without an account: the welcome screen as it always was, with no network requests.
 */
class AccountStartWidget : public QWidget
{
    Q_OBJECT

public:
    enum class Page
    {
        SignIn,
        Mfa,
        SelfHosted
    };

    explicit AccountStartWidget(QWidget* parent = nullptr);
    ~AccountStartWidget() override;

    void setController(AccountController* controller);
    // Back to the sign-in page, with the password cleared
    void reset();
    Page currentPage() const;

signals:
    // Signed in, paired with a sync server or chosen to go on without an account: show the welcome screen
    void finished();
    void createAccountRequested();

private slots:
    void signIn();
    void submitMfa();
    void showSelfHosted();
    void pair();
    void useWithoutAccount();
    void createAccount();
    void updateSecretKeyField();
    void handleAccountError(const QString& message);
    void handleMfaRequired();
    void handleSignedIn();

private:
    void showPage(Page page);
    void setBusy(const QString& message);
    void showError(const QString& message);

    const QScopedPointer<Ui::AccountStartWidget> m_ui;
    QPointer<AccountController> m_controller;
    // A sign-in started here is in progress, so the account's errors are for this screen
    bool m_signingIn = false;
};

#endif // KEEPASSXC_ACCOUNTSTARTWIDGET_H
