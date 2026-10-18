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

#include "AccountStartWidget.h"
#include "ui_AccountStartWidget.h"

#include "AccountController.h"

#include "account/AccountManager.h"
#include "core/Config.h"

#include <QUrl>

AccountStartWidget::AccountStartWidget(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::AccountStartWidget())
{
    m_ui->setupUi(this);
    auto titleFont = m_ui->titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    m_ui->titleLabel->setFont(titleFont);
    m_ui->messageWidget->setHidden(true);
    m_ui->messageWidget->setCloseButtonVisible(true);
    m_ui->busyLabel->setVisible(false);

    connect(m_ui->signInButton, &QPushButton::clicked, this, &AccountStartWidget::signIn);
    connect(m_ui->passwordEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::signIn);
    connect(m_ui->secretKeyEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::signIn);
    connect(m_ui->emailEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSecretKeyField);
    connect(m_ui->serverEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSecretKeyField);
    connect(m_ui->createAccountButton, &QPushButton::clicked, this, &AccountStartWidget::createAccount);
    connect(m_ui->selfHostedButton, &QPushButton::clicked, this, &AccountStartWidget::showSelfHosted);
    connect(m_ui->localButton, &QPushButton::clicked, this, &AccountStartWidget::useWithoutAccount);
    connect(m_ui->mfaVerifyButton, &QPushButton::clicked, this, &AccountStartWidget::submitMfa);
    connect(m_ui->mfaCodeEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::submitMfa);
    connect(m_ui->mfaCancelButton, &QPushButton::clicked, this, [this] {
        // The server drops the MFA token after five minutes anyway
        if (m_controller) {
            m_controller->account()->signOut();
        }
        m_signingIn = false;
        reset();
    });
    connect(m_ui->pairButton, &QPushButton::clicked, this, &AccountStartWidget::pair);
    connect(m_ui->pairingCodeEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::pair);
    connect(m_ui->selfHostedBackButton, &QPushButton::clicked, this, [this] { showPage(Page::SignIn); });
}

AccountStartWidget::~AccountStartWidget() = default;

void AccountStartWidget::setController(AccountController* controller)
{
    if (m_controller) {
        m_controller->account()->disconnect(this);
    }
    m_controller = controller;
    if (!controller) {
        return;
    }
    auto* account = controller->account();
    connect(account, &AccountManager::error, this, &AccountStartWidget::handleAccountError);
    connect(account, &AccountManager::mfaRequired, this, &AccountStartWidget::handleMfaRequired);
    connect(account, &AccountManager::signedIn, this, &AccountStartWidget::handleSignedIn);
    reset();
}

void AccountStartWidget::reset()
{
    m_ui->passwordEdit->clear();
    m_ui->secretKeyEdit->clear();
    m_ui->mfaCodeEdit->clear();
    m_ui->pairingCodeEdit->clear();
    m_ui->serverEdit->setText(AccountController::hostedServerUrl());
    if (m_controller && m_ui->emailEdit->text().isEmpty()) {
        m_ui->emailEdit->setText(m_controller->account()->email());
    }
    const auto selfHosted = config()->get(Config::Account_SelfHostedServerUrl).toString();
    if (!selfHosted.isEmpty()) {
        m_ui->selfHostedUrlEdit->setText(selfHosted);
    }
    setBusy({});
    updateSecretKeyField();
    showPage(Page::SignIn);
}

AccountStartWidget::Page AccountStartWidget::currentPage() const
{
    return static_cast<Page>(m_ui->pages->currentIndex());
}

void AccountStartWidget::showPage(Page page)
{
    m_ui->pages->setCurrentIndex(static_cast<int>(page));
    switch (page) {
    case Page::SignIn:
        if (m_ui->emailEdit->text().isEmpty()) {
            m_ui->emailEdit->setFocus();
        } else {
            m_ui->passwordEdit->setFocus();
        }
        break;
    case Page::Mfa:
        m_ui->mfaCodeEdit->clear();
        m_ui->mfaCodeEdit->setFocus();
        break;
    case Page::SelfHosted:
        if (m_ui->selfHostedUrlEdit->text().isEmpty()) {
            m_ui->selfHostedUrlEdit->setFocus();
        } else {
            m_ui->pairingCodeEdit->setFocus();
        }
        break;
    }
}

void AccountStartWidget::setBusy(const QString& message)
{
    m_ui->busyLabel->setText(message);
    m_ui->busyLabel->setVisible(!message.isEmpty());
    m_ui->pages->setEnabled(message.isEmpty());
    if (!message.isEmpty()) {
        m_ui->messageWidget->hideMessage();
    }
}

void AccountStartWidget::showError(const QString& message)
{
    setBusy({});
    m_ui->messageWidget->showMessage(message, MessageWidget::Error, MessageWidget::DisableAutoHide);
}

void AccountStartWidget::updateSecretKeyField()
{
    // Only a device that hasn't been trusted with the account yet asks for the Secret Key
    bool needed = true;
    const auto email = m_ui->emailEdit->text().trimmed();
    if (m_controller && !email.isEmpty()) {
        const auto& server = m_controller->account()->server();
        auto serverId = QUrl(m_ui->serverEdit->text().trimmed()).host();
        if (server.isValid() && QUrl(server.issuer).host() == serverId) {
            serverId = server.serverId();
        }
        needed = AccountManager::storedSecretKey(serverId, email).isEmpty();
    }
    m_ui->secretKeyLabel->setVisible(needed);
    m_ui->secretKeyEdit->setVisible(needed);
    m_ui->secretKeyHint->setVisible(needed && !email.isEmpty());
}

void AccountStartWidget::signIn()
{
    if (!m_controller) {
        return;
    }
    const auto email = m_ui->emailEdit->text().trimmed();
    const auto password = m_ui->passwordEdit->text();
    if (email.isEmpty() || password.isEmpty()) {
        showError(tr("Enter your email address and master password."));
        return;
    }
    const auto secretKey = !m_ui->secretKeyEdit->isHidden() ? m_ui->secretKeyEdit->text() : QString();
    setBusy(tr("Signing in…"));
    m_signingIn = true;
    QPointer<AccountStartWidget> guard(this);
    m_controller->ensureServer(m_ui->serverEdit->text(),
                               [guard, email, password, secretKey](bool ok, const QString& error) {
                                   if (!guard || !guard->m_controller) {
                                       return;
                                   }
                                   if (!ok) {
                                       guard->m_signingIn = false;
                                       guard->showError(error);
                                       return;
                                   }
                                   // A new sign-in replaces whatever session this device had
                                   auto* account = guard->m_controller->account();
                                   if (account->state() != AccountManager::State::SignedOut) {
                                       guard->m_controller->lockAccount();
                                       account->signOut();
                                   }
                                   account->signIn(email, password, secretKey);
                               });
}

void AccountStartWidget::handleMfaRequired()
{
    if (!m_signingIn) {
        return;
    }
    setBusy({});
    m_ui->messageWidget->hideMessage();
    showPage(Page::Mfa);
}

void AccountStartWidget::submitMfa()
{
    if (!m_controller) {
        return;
    }
    const auto code = m_ui->mfaCodeEdit->text().trimmed();
    if (code.size() != 6) {
        showError(tr("Enter the 6 digits shown by your authenticator app."));
        return;
    }
    setBusy(tr("Checking the code…"));
    m_controller->account()->submitMfa(code);
}

void AccountStartWidget::handleAccountError(const QString& message)
{
    if (!m_signingIn || !m_controller) {
        return;
    }
    if (m_controller->account()->state() == AccountManager::State::NeedsMfa) {
        // A wrong code (invalid_mfa): the server keeps the challenge, so ask again
        showPage(Page::Mfa);
        showError(message);
        return;
    }
    // Anything else ends this sign-in attempt
    m_signingIn = false;
    m_ui->passwordEdit->clear();
    showPage(Page::SignIn);
    showError(message);
}

void AccountStartWidget::handleSignedIn()
{
    if (!m_signingIn) {
        return;
    }
    m_signingIn = false;
    m_ui->passwordEdit->clear();
    m_ui->secretKeyEdit->clear();
    setBusy({});
    if (m_controller) {
        m_controller->setMode(AccountController::Mode::Account);
    }
    emit finished();
}

void AccountStartWidget::showSelfHosted()
{
    m_ui->messageWidget->hideMessage();
    showPage(Page::SelfHosted);
}

void AccountStartWidget::pair()
{
    if (!m_controller) {
        return;
    }
    setBusy(tr("Connecting to the sync server…"));
    QPointer<AccountStartWidget> guard(this);
    m_controller->pairSelfHosted(
        m_ui->selfHostedUrlEdit->text(), m_ui->pairingCodeEdit->text(), [guard](bool ok, const QString& error) {
            if (!guard) {
                return;
            }
            if (!ok) {
                guard->showError(error);
                return;
            }
            guard->setBusy({});
            guard->m_ui->pairingCodeEdit->clear();
            emit guard->finished();
        });
}

void AccountStartWidget::useWithoutAccount()
{
    if (m_controller) {
        m_controller->setMode(AccountController::Mode::Local);
    }
    emit finished();
}

void AccountStartWidget::createAccount()
{
    if (!m_controller) {
        return;
    }
    // The wizard signs up on the server entered here
    AccountController::setHostedServerUrl(m_ui->serverEdit->text());
    emit createAccountRequested();
}
