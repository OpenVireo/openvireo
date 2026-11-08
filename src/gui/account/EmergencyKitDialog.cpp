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

#include "EmergencyKitDialog.h"

#include "AccountController.h"
#include "EmergencyKit.h"

#include "account/AccountManager.h"
#include "gui/Clipboard.h"
#include "gui/MessageWidget.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

EmergencyKitDialog::EmergencyKitDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_message(new MessageWidget(this))
    , m_secretKeyLabel(new QLabel(this))
    , m_copyButton(nullptr)
    , m_saveButton(nullptr)
{
    setObjectName("emergencyKitDialog");
    setWindowTitle(tr("Emergency Kit"));
    resize(520, 0);

    if (account) {
        const auto server = account->server();
        m_email = account->email();
        m_server = server.issuer.isEmpty() ? AccountController::hostedServerUrl() : server.issuer;
        m_secretKey = AccountManager::storedSecretKey(server.serverId(), m_email);
    }

    auto* layout = new QVBoxLayout(this);
    m_message->setObjectName("messageWidget");
    m_message->setHidden(true);
    layout->addWidget(m_message);

    auto* intro = new QLabel(tr("This is your Secret Key, kept on this device. You need it with your master password "
                                "to sign in on a new device. Save the kit again if you lost it."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(font.pointSize() + 6);
    font.setBold(true);
    m_secretKeyLabel->setObjectName("secretKeyLabel");
    m_secretKeyLabel->setFont(font);
    m_secretKeyLabel->setAlignment(Qt::AlignCenter);
    m_secretKeyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_secretKeyLabel->setAccessibleName(tr("Secret Key"));
    m_secretKeyLabel->setContentsMargins(0, 12, 0, 12);
    m_secretKeyLabel->setText(m_secretKey);
    layout->addWidget(m_secretKeyLabel);

    auto* facts = new QFormLayout();
    auto* email = new QLabel(m_email, this);
    email->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* server = new QLabel(m_server, this);
    server->setTextInteractionFlags(Qt::TextSelectableByMouse);
    facts->addRow(tr("Email:"), email);
    facts->addRow(tr("Sign-in address:"), server);
    layout->addLayout(facts);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_copyButton = buttons->addButton(tr("Copy Secret Key"), QDialogButtonBox::ActionRole);
    m_copyButton->setObjectName("copyKeyButton");
    m_saveButton = buttons->addButton(tr("Save Emergency Kit…"), QDialogButtonBox::ActionRole);
    m_saveButton->setObjectName("saveKitButton");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_copyButton, &QPushButton::clicked, this, &EmergencyKitDialog::copyKey);
    connect(m_saveButton, &QPushButton::clicked, this, &EmergencyKitDialog::saveKit);
    layout->addWidget(buttons);

    if (m_secretKey.isEmpty()) {
        m_secretKeyLabel->setVisible(false);
        m_copyButton->setEnabled(false);
        m_saveButton->setEnabled(false);
        m_message->showMessage(tr("This device doesn't keep the Secret Key. Use the Emergency Kit you saved when you "
                                  "signed up."),
                               MessageWidget::Warning,
                               MessageWidget::DisableAutoHide);
    }
}

EmergencyKitDialog::~EmergencyKitDialog()
{
    m_secretKey.fill(QChar(0));
    m_secretKey.clear();
}

void EmergencyKitDialog::copyKey()
{
    // Cleared from the clipboard after the time set in Settings › Security
    clipboard()->setText(m_secretKey);
    m_message->showMessage(tr("Secret Key copied. Paste it into your Emergency Kit."), MessageWidget::Positive);
}

void EmergencyKitDialog::saveKit()
{
    QString path;
    QString error;
    if (!EmergencyKit(m_email, m_server, m_secretKey).saveAs(this, &path, &error)) {
        if (!error.isEmpty()) {
            m_message->showMessage(error, MessageWidget::Error);
        }
        return;
    }
    m_message->showMessage(tr("Emergency Kit saved to %1.").arg(QDir::toNativeSeparators(path)),
                           MessageWidget::Positive);
}
