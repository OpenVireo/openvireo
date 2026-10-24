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

#include "SignUpWizard.h"
#include "ui_SignUpWizard.h"

#include "AccountController.h"
#include "EmergencyKit.h"

#include "account/AccountManager.h"
#include "gui/FileDialog.h"
#include "gui/MessageBox.h"

#include <QDir>
#include <QFontDatabase>
#include <QPushButton>
#include <QStandardPaths>

SignUpWizard::SignUpWizard(AccountController* controller, QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::SignUpWizard())
    , m_controller(controller)
{
    m_ui->setupUi(this);
    setObjectName("signUpWizard");
    m_ui->messageWidget->setHidden(true);
    m_ui->busyLabel->setVisible(false);
    m_ui->serverValue->setText(AccountController::hostedServerUrl());
    m_ui->pages->setCurrentIndex(0);

    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(font.pointSize() + 6);
    font.setBold(true);
    m_ui->secretKeyLabel->setFont(font);

    m_nextButton = m_ui->buttonBox->addButton(tr("Create account"), QDialogButtonBox::AcceptRole);
    m_nextButton->setObjectName("signUpNext");
    m_nextButton->setDefault(true);
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        if (m_ui->pages->currentIndex() == 0) {
            createAccount();
        } else if (m_ui->savedCheckBox->isChecked()) {
            forgetSecretKey();
            accept();
        }
    });
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &SignUpWizard::reject);
    connect(m_ui->saveKitButton, &QPushButton::clicked, this, &SignUpWizard::saveKit);
    connect(m_ui->savedCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_ui->pages->currentIndex() == 1) {
            m_nextButton->setEnabled(checked);
        }
    });

    if (m_controller) {
        auto* account = m_controller->account();
        connect(account, &AccountManager::registered, this, &SignUpWizard::handleRegistered);
        connect(account, &AccountManager::error, this, &SignUpWizard::handleError);
    }
    m_ui->emailEdit->setFocus();
}

SignUpWizard::~SignUpWizard()
{
    forgetSecretKey();
}

QString SignUpWizard::secretKey() const
{
    return m_secretKey;
}

void SignUpWizard::setBusy(const QString& message)
{
    m_ui->busyLabel->setText(message);
    m_ui->busyLabel->setVisible(!message.isEmpty());
    m_ui->pages->setEnabled(message.isEmpty());
    m_nextButton->setEnabled(message.isEmpty());
}

void SignUpWizard::createAccount()
{
    if (!m_controller) {
        return;
    }
    const auto email = m_ui->emailEdit->text().trimmed();
    const auto password = m_ui->passwordEdit->text();
    if (!email.contains('@')) {
        m_ui->messageWidget->showMessage(tr("Enter your email address."), MessageWidget::Error);
        return;
    }
    if (password.size() < MinPasswordLength) {
        m_ui->messageWidget->showMessage(
            tr("Choose a master password of at least %1 characters.").arg(MinPasswordLength), MessageWidget::Error);
        return;
    }
    if (password != m_ui->confirmEdit->text()) {
        m_ui->messageWidget->showMessage(tr("The passwords don't match."), MessageWidget::Error);
        return;
    }
    m_ui->messageWidget->hideMessage();
    setBusy(tr("Creating your account…"));
    m_registering = true;
    QPointer<SignUpWizard> guard(this);
    m_controller->ensureServer(AccountController::hostedServerUrl(),
                               [guard, email, password](bool ok, const QString& error) {
                                   if (!guard || !guard->m_controller) {
                                       return;
                                   }
                                   if (!ok) {
                                       guard->handleError(error);
                                       return;
                                   }
                                   auto* account = guard->m_controller->account();
                                   if (account->state() != AccountManager::State::SignedOut) {
                                       guard->m_controller->lockAccount();
                                       account->signOut();
                                   }
                                   account->registerAccount(email, password);
                               });
}

void SignUpWizard::handleError(const QString& message)
{
    if (!m_registering) {
        return;
    }
    m_registering = false;
    setBusy({});
    m_ui->pages->setCurrentIndex(0);
    m_ui->messageWidget->showMessage(message, MessageWidget::Error, MessageWidget::DisableAutoHide);
}

void SignUpWizard::handleRegistered(const QString& secretKey)
{
    if (!m_registering) {
        return;
    }
    m_registering = false;
    setBusy({});
    m_ui->passwordEdit->clear();
    m_ui->confirmEdit->clear();
    m_secretKey = secretKey;
    m_ui->secretKeyLabel->setText(secretKey);
    m_ui->pages->setCurrentIndex(1);
    m_nextButton->setText(tr("Finish"));
    m_nextButton->setEnabled(false);
    m_ui->buttonBox->button(QDialogButtonBox::Cancel)->setVisible(false);
    if (m_controller) {
        m_controller->setMode(AccountController::Mode::Account);
    }
}

void SignUpWizard::saveKit()
{
    const auto defaultPath = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                 .filePath("OpenVireo Emergency Kit.pdf");
    const auto path =
        fileDialog()->getSaveFileName(this, tr("Save Emergency Kit"), defaultPath, tr("PDF files (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    const auto email = m_controller ? m_controller->account()->email() : m_ui->emailEdit->text();
    QString error;
    if (!EmergencyKit(email, AccountController::hostedServerUrl(), m_secretKey).writePdf(path, &error)) {
        m_ui->messageWidget->showMessage(error, MessageWidget::Error);
        return;
    }
    m_kitSaved = true;
    m_ui->savedCheckBox->setChecked(true);
    m_ui->messageWidget->showMessage(tr("Emergency Kit saved to %1.").arg(QDir::toNativeSeparators(path)),
                                     MessageWidget::Positive);
}

void SignUpWizard::reject()
{
    if (m_ui->pages->currentIndex() == 1 && !m_ui->savedCheckBox->isChecked()) {
        // The account exists and is signed in; only the kit can still be lost
        auto answer = MessageBox::question(this,
                                           tr("Close without the Emergency Kit?"),
                                           tr("Your Secret Key won't be shown again. Without it you can't sign in on "
                                              "another device. Close anyway?"),
                                           MessageBox::Close | MessageBox::Cancel,
                                           MessageBox::Cancel);
        if (answer != MessageBox::Close) {
            return;
        }
    }
    if (m_registering) {
        return;
    }
    forgetSecretKey();
    // Once registered, the account is signed in either way
    if (m_ui->pages->currentIndex() == 1) {
        accept();
        return;
    }
    QDialog::reject();
}

void SignUpWizard::forgetSecretKey()
{
    m_secretKey.fill(QChar(0));
    m_secretKey.clear();
    m_ui->secretKeyLabel->clear();
}
