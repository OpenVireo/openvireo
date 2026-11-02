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

#include "AccountLockWidget.h"

#include "AccountController.h"
#include "account/AccountManager.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/MessageWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <QAction>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    // The width of the card and of the tile at its top, as on a database's unlock card
    constexpr int CardWidth = 440;
    constexpr int TileSize = 56;

    QColor outlineColor(const FluentTheme::Palette& p)
    {
        auto stroke = p.strokeStrong;
        stroke.setAlphaF(stroke.alphaF() * 0.3);
        return stroke;
    }

    QString linkHtml(const QString& text, const QColor& color)
    {
        return QStringLiteral("<a href=\"#\" style=\"color: %1; text-decoration: none\">%2</a>")
            .arg(color.name(), text.toHtmlEscaped());
    }

    QString quickUnlockText()
    {
#if defined(Q_OS_MACOS)
        return QObject::tr("Unlock with Touch ID");
#elif defined(Q_OS_WIN)
        return QObject::tr("Unlock with Windows Hello");
#else
        return QObject::tr("Quick Unlock");
#endif
    }
} // namespace

AccountLockWidget::AccountLockWidget(QWidget* parent)
    : QWidget(parent)
    , m_card(new QFrame(this))
    , m_tile(new QLabel(m_card))
    , m_headline(new QLabel(tr("Your account is locked"), m_card))
    , m_accountLabel(new QLabel(m_card))
    , m_serverLabel(new QLabel(m_card))
    , m_quickUnlockButton(new VaultButton(quickUnlockText(), m_card))
    , m_orLabel(new QLabel(tr("or use your master password"), m_card))
    , m_passwordLabel(new QLabel(tr("Master password"), m_card))
    , m_passwordEdit(new VaultLineEdit(m_card))
    , m_messageWidget(new MessageWidget(m_card))
    , m_busyLabel(new QLabel(tr("Unlocking…"), m_card))
    , m_unlockButton(new VaultButton(tr("Unlock"), m_card))
    , m_recoverLink(new QLabel(m_card))
    , m_dismissLink(new QLabel(this))
    , m_signOutLink(new QLabel(this))
{
    Vault::paintCanvas(this);

    m_card->setObjectName("accountLockCard");
    m_card->setFixedWidth(CardWidth);
    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(32, 32, 32, 28);
    cardLayout->setSpacing(0);

    m_tile->setFixedSize(TileSize, TileSize);
    m_tile->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(m_tile, 0, Qt::AlignHCenter);
    cardLayout->addSpacing(16);
    m_headline->setObjectName("accountLockHeadline");
    m_headline->setAlignment(Qt::AlignCenter);
    m_headline->setWordWrap(true);
    cardLayout->addWidget(m_headline);
    cardLayout->addSpacing(4);
    for (auto* label : {m_accountLabel, m_serverLabel}) {
        label->setAlignment(Qt::AlignCenter);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(label);
    }
    m_accountLabel->setObjectName("accountLockEmail");
    m_serverLabel->setObjectName("accountLockServer");
    cardLayout->addSpacing(24);

    m_quickUnlockButton->setObjectName("accountLockQuickUnlock");
    m_quickUnlockButton->setMinimumHeight(48);
    m_quickUnlockButton->setToolTip(tr("Unlock with Touch ID, Windows Hello or your system password"));
    cardLayout->addWidget(m_quickUnlockButton);
    m_orLabel->setAlignment(Qt::AlignCenter);
    m_orLabel->setContentsMargins(0, 12, 0, 12);
    cardLayout->addWidget(m_orLabel);

    m_passwordLabel->setContentsMargins(0, 0, 0, 6);
    m_passwordLabel->setBuddy(m_passwordEdit);
    cardLayout->addWidget(m_passwordLabel);
    m_passwordEdit->setObjectName("accountLockPassword");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setContentMargins(VaultLineEdit::LargePadding);
    m_passwordEdit->setAccessibleName(tr("Master password"));
    m_revealAction = m_passwordEdit->addAction(icons()->onOffIcon("password-show", false), QLineEdit::TrailingPosition);
    m_revealAction->setCheckable(true);
    m_revealAction->setToolTip(tr("Show password"));
    connect(m_revealAction, &QAction::toggled, this, [this](bool show) {
        m_passwordEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        m_revealAction->setIcon(icons()->onOffIcon("password-show", show));
        m_revealAction->setToolTip(show ? tr("Hide password") : tr("Show password"));
    });
    cardLayout->addWidget(m_passwordEdit);

    m_messageWidget->setObjectName("accountLockMessage");
    m_messageWidget->setHidden(true);
    // The next attempt clears it; a close button would take a row of its own once the text wraps
    m_messageWidget->setCloseButtonVisible(false);
    m_messageWidget->setWordWrap(true);
    cardLayout->addSpacing(8);
    cardLayout->addWidget(m_messageWidget);
    m_busyLabel->setContentsMargins(0, 8, 0, 0);
    m_busyLabel->setVisible(false);
    cardLayout->addWidget(m_busyLabel);

    m_unlockButton->setObjectName("accountLockUnlock");
    m_unlockButton->setDefault(true);
    m_unlockButton->setMinimumHeight(36);
    cardLayout->addSpacing(16);
    cardLayout->addWidget(m_unlockButton);
    cardLayout->addSpacing(16);
    m_recoverLink->setObjectName("accountLockRecover");
    m_recoverLink->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(m_recoverLink);

    // Ways out beneath the card: the account stays locked
    auto* links = new QHBoxLayout();
    links->setSpacing(24);
    links->addStretch();
    m_dismissLink->setObjectName("accountLockDismiss");
    m_signOutLink->setObjectName("accountLockSignOut");
    links->addWidget(m_dismissLink);
    links->addWidget(m_signOutLink);
    links->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(0);
    layout->addStretch(2);
    layout->addWidget(m_card, 0, Qt::AlignHCenter);
    layout->addSpacing(16);
    layout->addLayout(links);
    layout->addStretch(3);

    Vault::followTheme(this, [this] { applyTheme(); });

    connect(m_unlockButton, &QPushButton::clicked, this, &AccountLockWidget::unlock);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &AccountLockWidget::unlock);
    connect(m_quickUnlockButton, &QPushButton::clicked, this, &AccountLockWidget::quickUnlock);
    // Lost password or Secret Key: the request form on the account server's website (SPEC §18)
    connect(m_recoverLink, &QLabel::linkActivated, this, [this] {
        const auto issuer = m_controller ? m_controller->account()->server().issuer : QString();
        QDesktopServices::openUrl(AccountController::recoveryRequestUrl(issuer));
    });
    connect(m_dismissLink, &QLabel::linkActivated, this, &AccountLockWidget::dismissRequested);
    connect(m_signOutLink, &QLabel::linkActivated, this, &AccountLockWidget::signOut);
}

AccountLockWidget::~AccountLockWidget() = default;

void AccountLockWidget::setController(AccountController* controller)
{
    if (m_controller) {
        m_controller->account()->disconnect(this);
    }
    m_controller = controller;
    if (!controller) {
        return;
    }
    auto* account = controller->account();
    connect(account, &AccountManager::stateChanged, this, &AccountLockWidget::updateState);
    connect(account, &AccountManager::emailChanged, this, &AccountLockWidget::reset);
    connect(account, &AccountManager::error, this, &AccountLockWidget::handleError);
    reset();
}

void AccountLockWidget::reset()
{
    m_passwordEdit->clear();
    m_revealAction->setChecked(false);
    m_messageWidget->hideMessage();
    if (m_controller) {
        auto* account = m_controller->account();
        m_accountLabel->setText(account->email());
        const auto server = account->server();
        m_serverLabel->setText(QUrl(server.issuer.isEmpty() ? server.api : server.issuer).host());
    }
    updateState();
}

void AccountLockWidget::setDatabasesOpen(bool open)
{
    m_databasesOpen = open;
    updateLinks();
}

void AccountLockWidget::showEvent(QShowEvent* event)
{
    // The application style sheet resets the labels' fonts when they're first polished, which is on show
    applyTheme();
    reset();
    QWidget::showEvent(event);
}

void AccountLockWidget::hideEvent(QHideEvent* event)
{
    // Nothing typed stays behind once the screen is gone
    m_passwordEdit->clear();
    m_revealAction->setChecked(false);
    QWidget::hideEvent(event);
}

void AccountLockWidget::applyTheme()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_headline, Typography::FontRole::Subtitle, p.text);
    Vault::styleLabel(m_accountLabel, Typography::FontRole::Body, p.text);
    Vault::styleLabel(m_serverLabel, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_orLabel, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_passwordLabel, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_busyLabel, Typography::FontRole::Body, p.textSecondary);
    m_quickUnlockButton->setFont(Vault::font(Typography::FontRole::BodyStrong));
    m_quickUnlockButton->setIcon(icons()->icon("fingerprint", true, p.text));
    m_quickUnlockButton->setIconSize({24, 24});

    m_tile->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-radius: 12px; color: %2; }")
                              .arg(p.accentSubtle.name(QColor::HexArgb), p.accentText.name(QColor::HexArgb)));
    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(24);
    m_tile->setFont(glyphFont);
    m_tile->setText(Typography::Icons::Lock);

    m_card->setStyleSheet(QStringLiteral("QFrame#accountLockCard { background-color: %1; border: 1px solid %2;"
                                         " border-radius: 8px; }")
                              .arg(p.layer.name(QColor::HexArgb), outlineColor(p).name(QColor::HexArgb)));
    updateLinks();
}

void AccountLockWidget::updateLinks()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_recoverLink, Typography::FontRole::Caption, p.accentText);
    m_recoverLink->setText(linkHtml(tr("Forgot your password?"), p.accentText));
    Vault::styleLabel(m_dismissLink, Typography::FontRole::Caption, p.accentText);
    m_dismissLink->setText(
        linkHtml(m_databasesOpen ? tr("Go to open databases") : tr("Open a local database"), p.accentText));
    Vault::styleLabel(m_signOutLink, Typography::FontRole::Caption, p.accentText);
    m_signOutLink->setText(linkHtml(tr("Sign out"), p.accentText));
}

bool AccountLockWidget::needsSignIn() const
{
    if (!m_controller) {
        return false;
    }
    auto* account = m_controller->account();
    return AccountManager::storedSecretKey(account->server().serverId(), account->email()).isEmpty();
}

void AccountLockWidget::updateState()
{
    if (!m_controller) {
        return;
    }
    auto* account = m_controller->account();
    const auto state = account->state();
    setBusy(state == AccountManager::State::SigningIn);
    const bool quick = account->canQuickUnlock();
    m_quickUnlockButton->setVisible(quick);
    m_orLabel->setVisible(quick);

    // Without the Secret Key the password alone can't unlock: sign in again, which asks for both
    const bool signIn = needsSignIn();
    m_passwordLabel->setVisible(!signIn);
    m_passwordEdit->setVisible(!signIn);
    m_unlockButton->setText(signIn ? tr("Sign in again") : tr("Unlock"));

    if (state == AccountManager::State::Locked && isVisible()) {
        if (quick) {
            m_quickUnlockButton->setFocus();
        } else if (!signIn) {
            m_passwordEdit->setFocus();
        }
    }
}

void AccountLockWidget::setBusy(bool busy)
{
    m_busyLabel->setVisible(busy);
    m_quickUnlockButton->setEnabled(!busy);
    m_passwordEdit->setEnabled(!busy);
    m_unlockButton->setEnabled(!busy);
    if (busy) {
        m_messageWidget->hideMessage();
    }
}

void AccountLockWidget::unlock()
{
    if (!m_controller || m_controller->account()->state() != AccountManager::State::Locked) {
        return;
    }
    if (needsSignIn()) {
        // Signing out keeps the email for the start screen's sign-in form
        m_controller->signOut();
        return;
    }
    const auto password = m_passwordEdit->text();
    if (password.isEmpty()) {
        m_passwordEdit->setFocus();
        return;
    }
    m_passwordEdit->clear();
    m_controller->unlockAccount(password);
}

void AccountLockWidget::quickUnlock()
{
    if (m_controller) {
        m_controller->quickUnlockAccount();
    }
}

void AccountLockWidget::handleError(const QString& message)
{
    if (!isVisible() || !m_controller || m_controller->account()->state() == AccountManager::State::Unlocked) {
        return;
    }
    setBusy(false);
    m_passwordEdit->clear();
    m_messageWidget->showMessage(message, MessageWidget::Error, MessageWidget::DisableAutoHide);
    updateState();
}

void AccountLockWidget::signOut()
{
    if (!m_controller) {
        return;
    }
    const auto answer = MessageBox::question(
        this,
        tr("Sign out of %1?").arg(m_controller->account()->email()),
        tr("This device forgets the account's session and offline copy. Your vaults stay safe on the server; "
           "signing in again downloads them."),
        MessageBox::Yes | MessageBox::Cancel,
        MessageBox::Cancel);
    if (answer == MessageBox::Yes) {
        m_controller->signOut();
    }
}
