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
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultInfoBar.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultWidgets.h"

#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace
{
    // The card's width over the main window, as in the mockup
    constexpr int CardWidth = 520;
    // WinUI's content dialog padding, for the dialog when it opens as a window of its own
    constexpr int WindowPadding = 24;
    constexpr QChar MaskChar(u'\u2022');

    // The key with every group but its version prefix ("K1") hidden, keeping the dashes so it reads as a key
    QString maskedKey(const QString& key)
    {
        auto groups = key.split('-');
        for (int i = 1; i < groups.size(); ++i) {
            groups[i] = QString(groups[i].size(), MaskChar);
        }
        return groups.join('-');
    }
} // namespace

EmergencyKitDialog::EmergencyKitDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_message(new VaultInfoBar(this))
    , m_intro(new QLabel(this))
    , m_keyCaption(nullptr)
    , m_keyChip(nullptr)
    , m_secretKeyLabel(new QLabel(this))
    , m_note(new QLabel(this))
    , m_buttonRow(new QWidget(this))
    , m_revealButton(nullptr)
{
    setObjectName("emergencyKitDialog");
    setWindowTitle(tr("Emergency Kit"));

    if (account) {
        const auto server = account->server();
        m_email = account->email();
        m_server = server.issuer.isEmpty() ? AccountController::hostedServerUrl() : server.issuer;
        m_secretKey = AccountManager::storedSecretKey(server.serverId(), m_email);
    }
    const bool hasKey = !m_secretKey.isEmpty();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    layout->setSpacing(16);

    m_intro->setText(tr("Your Secret Key and master password sign you in on a new device."));
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    m_message->setObjectName("messageWidget");
    m_message->setHidden(true);
    connect(m_message, &VaultInfoBar::linkActivated, this, [](const QString& link) {
        QDesktopServices::openUrl(QUrl(link));
    });
    layout->addWidget(m_message);

    // The key, on an inset panel: a caption with where it's kept, then the key and Show
    auto* keyPanel = new VaultPanel(VaultPanel::Inset, this);
    keyPanel->setObjectName("secretKeyPanel");
    auto* keyLayout = new QVBoxLayout(keyPanel);
    keyLayout->setContentsMargins(20, 14, 14, 16);
    keyLayout->setSpacing(8);
    auto* keyHeader = new QHBoxLayout();
    keyHeader->setSpacing(8);
    m_keyCaption = new QLabel(tr("SECRET KEY"), keyPanel);
    m_keyChip = new VaultPill(tr("Kept on this device"), keyPanel);
    m_keyChip->setObjectName("secretKeyChip");
    keyHeader->addWidget(m_keyCaption);
    keyHeader->addStretch();
    keyHeader->addWidget(m_keyChip);
    keyLayout->addLayout(keyHeader);

    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(12);
    m_secretKeyLabel->setParent(keyPanel);
    m_secretKeyLabel->setObjectName("secretKeyLabel");
    m_secretKeyLabel->setAccessibleName(tr("Secret Key"));
    m_secretKeyLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_revealButton = new VaultButton(keyPanel);
    m_revealButton->setObjectName("revealKeyButton");
    Vault::makePill(m_revealButton);
    connect(m_revealButton, &QPushButton::clicked, this, &EmergencyKitDialog::toggleKey);
    keyRow->addWidget(m_secretKeyLabel, 1);
    keyRow->addWidget(m_revealButton);
    keyLayout->addLayout(keyRow);
    layout->addWidget(keyPanel);

    // The email and server the key goes with, as rows on a panel like the entry details card's
    auto* facts = new VaultPanel(VaultPanel::Layer, this);
    facts->setObjectName("emergencyKitFacts");
    auto* factsLayout = new QVBoxLayout(facts);
    factsLayout->setContentsMargins(16, 0, 16, 0);
    factsLayout->setSpacing(0);
    const QList<QPair<QString, QString>> rows = {{tr("Email"), m_email}, {tr("Sign-in address"), m_server}};
    for (int i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            factsLayout->addWidget(new VaultDivider(facts));
        }
        auto* row = new QWidget(facts);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 11, 0, 11);
        rowLayout->setSpacing(12);
        auto* caption = new QLabel(rows[i].first, row);
        caption->setFixedWidth(120);
        auto* value = new QLabel(rows[i].second, row);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rowLayout->addWidget(caption);
        rowLayout->addWidget(value, 1);
        factsLayout->addWidget(row);
        m_factCaptions << caption;
        m_factValues << value;
    }
    layout->addWidget(facts);

    m_note->setText(tr("Anyone with this key and your master password can open your vault. Keep the kit somewhere "
                       "offline."));
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    // The buttons: Copy and Save, or only Close when there's no key to show
    auto* buttons = new QHBoxLayout(m_buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    buttons->addStretch();
    auto* close = new VaultButton(tr("Close"), m_buttonRow);
    close->setObjectName("emergencyKitClose");
    m_closeButton = close;
    auto* copy = new VaultButton(tr("Copy Secret Key"), m_buttonRow);
    copy->setObjectName("copyKeyButton");
    copy->setIconGlyph(Typography::Icons::Copy);
    m_copyButton = copy;
    auto* save = new VaultButton(tr("Save Emergency Kit…"), m_buttonRow);
    save->setObjectName("saveKitButton");
    save->setIconGlyph(Typography::Icons::Download);
    save->setFluentStyle(fluent::basicinput::Button::Accent);
    m_saveButton = save;
    for (auto* button : {close, copy, save}) {
        Vault::makePill(button);
        buttons->addWidget(button);
    }
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_copyButton, &QPushButton::clicked, this, &EmergencyKitDialog::copyKey);
    connect(m_saveButton, &QPushButton::clicked, this, &EmergencyKitDialog::saveKit);
    layout->addWidget(m_buttonRow);

    if (hasKey) {
        save->setDefault(true);
    } else {
        keyPanel->hide();
        m_note->hide();
        copy->hide();
        save->hide();
        close->setDefault(true);
        const auto recovery = AccountController::recoveryRequestUrl(m_server).toString();
        m_message->showMessage(tr("This device doesn't keep your Secret Key. Use the Emergency Kit you saved when "
                                  "you signed up. Lost it? <a href=\"%1\">Start account recovery</a>.")
                                   .arg(recovery.toHtmlEscaped()),
                               VaultInfoBar::Warning,
                               VaultInfoBar::DisableAutoHide);
    }

    showKey();
    Vault::followTheme(this, [this] { applyColors(); });
    setFixedWidth(CardWidth);
}

EmergencyKitDialog::~EmergencyKitDialog()
{
    m_secretKey.fill(QChar(0));
    m_secretKey.clear();
}

EmergencyKitDialog* EmergencyKitDialog::present(AccountManager* account, QWidget* parent)
{
    auto* dialog = new EmergencyKitDialog(account, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* modal = VaultModal::forDialog(dialog, QStringLiteral("vaultEmergencyKit"), CardWidth);
    if (!modal) {
        dialog->open();
        return dialog;
    }
    // The card brings the title and the close button, and its footer takes the buttons. The content lines up with
    // the card's title.
    dialog->layout()->setContentsMargins(12, 4, 8, 4);
    if (!dialog->m_secretKey.isEmpty()) {
        dialog->m_closeButton->hide();
    }
    modal->addFooterWidget(dialog->m_buttonRow, 1);
    modal->setInitialFocus(dialog->m_secretKey.isEmpty() ? dialog->m_closeButton : dialog->m_revealButton);
    modal->open();
    return dialog;
}

void EmergencyKitDialog::toggleKey()
{
    m_revealed = !m_revealed;
    showKey();
}

void EmergencyKitDialog::showKey()
{
    m_secretKeyLabel->setText(m_revealed ? m_secretKey : maskedKey(m_secretKey));
    // Only the real key can be selected, not the dots standing in for it
    m_secretKeyLabel->setTextInteractionFlags(m_revealed ? Qt::TextSelectableByMouse : Qt::NoTextInteraction);
    m_revealButton->setText(m_revealed ? tr("Hide") : tr("Show"));
    m_revealButton->setIconGlyph(m_revealed ? Typography::Icons::Hide : Typography::Icons::View);
    m_revealButton->setAccessibleName(m_revealed ? tr("Hide Secret Key") : tr("Show Secret Key"));
}

void EmergencyKitDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_note, Typography::FontRole::Caption, p.textSecondary);

    auto caption = Vault::font(Typography::FontRole::Caption);
    caption.setFamily(FluentTheme::monoFontFamily());
    caption.setWeight(QFont::Medium);
    caption.setLetterSpacing(QFont::PercentageSpacing, 110);
    Vault::styleLabel(m_keyCaption, Typography::FontRole::Caption, p.textTertiary);
    m_keyCaption->setFont(caption);

    m_keyChip->setColors(p.accentSubtle, p.accentText);

    QFont key(FluentTheme::monoFontFamily());
    // 17px fits the whole key beside Show at the card's width
    key.setPixelSize(17);
    key.setWeight(QFont::Medium);
    m_secretKeyLabel->setFont(key);
    auto keyPalette = m_secretKeyLabel->palette();
    keyPalette.setColor(QPalette::WindowText, p.text);
    m_secretKeyLabel->setPalette(keyPalette);

    for (auto* label : m_factCaptions) {
        Vault::styleLabel(label, Typography::FontRole::Body, p.textSecondary);
    }
    for (auto* label : m_factValues) {
        Vault::styleLabel(label, Typography::FontRole::Body, p.text);
    }
}

void EmergencyKitDialog::copyKey()
{
    // Cleared from the clipboard after the time set in Settings › Security
    clipboard()->setText(m_secretKey);
    m_message->showMessage(tr("Secret Key copied. Paste it into your Emergency Kit."), VaultInfoBar::Positive);
}

void EmergencyKitDialog::saveKit()
{
    QString path;
    QString error;
    if (!EmergencyKit(m_email, m_server, m_secretKey).saveAs(this, &path, &error)) {
        if (!error.isEmpty()) {
            m_message->showMessage(error, VaultInfoBar::Error);
        }
        return;
    }
    m_message->showMessage(tr("Emergency Kit saved to %1.").arg(QDir::toNativeSeparators(path)),
                           VaultInfoBar::Positive);
}
