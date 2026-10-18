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

#ifndef KEEPASSXC_SIGNUPWIZARD_H
#define KEEPASSXC_SIGNUPWIZARD_H

#include <QDialog>
#include <QPointer>
#include <QScopedPointer>

class AccountController;
class QPushButton;

namespace Ui
{
    class SignUpWizard;
}

/**
 * Creates a KeePassXC account in two steps: the email and master password, then the Secret Key, shown
 * once, with the Emergency Kit to save. The account is signed in and unlocked when it closes.
 */
class SignUpWizard : public QDialog
{
    Q_OBJECT

public:
    static constexpr int MinPasswordLength = 10;

    explicit SignUpWizard(AccountController* controller, QWidget* parent = nullptr);
    ~SignUpWizard() override;

    // The Secret Key while the kit page shows, empty before and after
    QString secretKey() const;

public slots:
    void reject() override;

private slots:
    void createAccount();
    void saveKit();
    void handleRegistered(const QString& secretKey);
    void handleError(const QString& message);

private:
    void setBusy(const QString& message);
    void forgetSecretKey();

    const QScopedPointer<Ui::SignUpWizard> m_ui;
    QPointer<AccountController> m_controller;
    QPushButton* m_nextButton;
    QString m_secretKey;
    bool m_registering = false;
    bool m_kitSaved = false;
};

#endif // KEEPASSXC_SIGNUPWIZARD_H
