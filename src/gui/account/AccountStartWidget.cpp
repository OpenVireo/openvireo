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
#include "gui/Icons.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QAction>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace
{
    // Start screen layout: the column's widest, the card's padding (less on a narrow window), the option rows'
    // height, and the inset of their glyph and text
    constexpr int ColumnWidth = 488;
    constexpr int CardPadding = 32;
    constexpr int CompactCardPadding = 20;
    constexpr int OptionHeight = 48;
    constexpr int OptionPadding = 20;
    constexpr int OptionGlyphSize = 16;

    // Card and option row outline: between the card stroke and the strong stroke, so they read on the canvas
    QColor outlineColor(const FluentTheme::Palette& p)
    {
        auto stroke = p.strokeStrong;
        stroke.setAlphaF(stroke.alphaF() * 0.3);
        return stroke;
    }

    QString linkHtml(const QString& text, const QColor& color)
    {
        return QStringLiteral("<a href=\"#\" style=\"color: %1; text-decoration: underline\">%2</a>")
            .arg(color.name(), text.toHtmlEscaped());
    }

    // A field label with the required marker after it
    QString requiredHtml(const QString& text, const QColor& color)
    {
        return QStringLiteral("%1 <span style=\"color: %2\">*</span>").arg(text.toHtmlEscaped(), color.name());
    }

    void setGlyph(QLabel* label, const QString& glyph, int pixelSize)
    {
        QFont font(Typography::FontFamily::FluentIcons);
        font.setPixelSize(pixelSize);
        label->setFont(font);
        label->setText(glyph);
    }
} // namespace

QSize AccountStartElidedLabel::minimumSizeHint() const
{
    return {0, QLabel::minimumSizeHint().height()};
}

void AccountStartElidedLabel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::WindowText));
    const auto area = contentsRect();
    painter.drawText(
        area, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideMiddle, area.width()));
}

AccountStartOptionButton::AccountStartOptionButton(QWidget* parent)
    : QPushButton(parent)
{
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    // Only keyboard focus shows the focus ring, so a click doesn't leave it on
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AccountStartOptionButton::setGlyph(const QString& glyph)
{
    m_glyph = glyph;
    update();
}

QSize AccountStartOptionButton::sizeHint() const
{
    const QFontMetrics metrics(Vault::font(Typography::FontRole::Body));
    return {OptionPadding * 2 + OptionGlyphSize + 12 + metrics.horizontalAdvance(text()), OptionHeight};
}

QSize AccountStartOptionButton::minimumSizeHint() const
{
    return {OptionPadding * 2 + OptionGlyphSize, OptionHeight};
}

void AccountStartOptionButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.fillPath(path, p.layer);
    if (isEnabled() && (isDown() || underMouse())) {
        painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
    }
    painter.setPen(hasFocus() ? QPen(p.accent, 2) : QPen(outlineColor(p), 1));
    painter.drawPath(path);

    const auto color = isEnabled() ? p.text : p.textDisabled;
    const QRect glyph(OptionPadding, 0, OptionGlyphSize, height());
    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(OptionGlyphSize);
    painter.setFont(glyphFont);
    painter.setPen(color);
    painter.drawText(glyph, Qt::AlignCenter, m_glyph);

    const auto font = Vault::font(Typography::FontRole::Body);
    const QRect label = rect().adjusted(glyph.right() + 12, 0, -OptionPadding, 0);
    painter.setFont(font);
    painter.drawText(
        label, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(font).elidedText(text(), Qt::ElideRight, label.width()));
}

AccountStartWidget::AccountStartWidget(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::AccountStartWidget())
{
    m_ui->setupUi(this);
    m_ui->messageWidget->setHidden(true);
    m_ui->messageWidget->setCloseButtonVisible(true);
    m_ui->busyLabel->setVisible(false);

    // A centered column on the window canvas: the logo, a sign-in card, and the other ways to start beneath it
    Vault::paintCanvas(this);
    m_ui->scrollArea->viewport()->setAutoFillBackground(false);
    m_ui->scrollContents->setAutoFillBackground(false);
    m_ui->logoLabel->setPixmap(icons()->applicationIcon().pixmap(64));
    m_ui->pages->setFrameShape(QFrame::NoFrame);
    m_ui->selfHostedButton->setGlyph(Typography::Icons::Storage);
    m_ui->localButton->setGlyph(Typography::Icons::Folder);

    // Labels sit just above their fields, and headings just above their intro
    for (auto* label : {m_ui->emailLabel,
                        m_ui->passwordLabel,
                        m_ui->mfaCodeLabel,
                        m_ui->selfHostedUrlLabel,
                        m_ui->pairingCodeLabel}) {
        label->setContentsMargins(0, 0, 0, 6);
    }
    m_ui->secretKeyLabel->setContentsMargins(0, 16, 0, 6);
    m_ui->secretKeyHint->setContentsMargins(0, 6, 0, 0);
    for (auto* heading : {m_ui->signInHeading, m_ui->mfaHeading, m_ui->selfHostedHeading}) {
        heading->setContentsMargins(0, 0, 0, 4);
    }
    m_ui->subtitleLabel->setContentsMargins(0, 4, 0, 0);
    m_ui->busyLabel->setContentsMargins(0, 16, 0, 0);
    m_ui->serverEdit->setVisible(false);
    for (auto* field : {m_ui->emailEdit,
                        m_ui->passwordEdit,
                        m_ui->secretKeyEdit,
                        m_ui->serverEdit,
                        m_ui->mfaCodeEdit,
                        m_ui->selfHostedUrlEdit,
                        m_ui->pairingCodeEdit}) {
        field->setContentMargins(VaultLineEdit::LargePadding);
    }

    m_revealPasswordAction =
        m_ui->passwordEdit->addAction(icons()->onOffIcon("password-show", false), QLineEdit::TrailingPosition);
    m_revealPasswordAction->setCheckable(true);
    m_revealPasswordAction->setToolTip(tr("Show password"));
    connect(m_revealPasswordAction, &QAction::toggled, this, [this](bool show) {
        m_ui->passwordEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        m_revealPasswordAction->setIcon(icons()->onOffIcon("password-show", show));
        m_revealPasswordAction->setToolTip(show ? tr("Hide password") : tr("Show password"));
    });

    Vault::followTheme(this, [this] { applyTheme(); });

    connect(m_ui->signInButton, &QPushButton::clicked, this, &AccountStartWidget::signIn);
    connect(m_ui->passwordEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::signIn);
    connect(m_ui->secretKeyEdit, &QLineEdit::returnPressed, this, &AccountStartWidget::signIn);
    connect(m_ui->emailEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSecretKeyField);
    connect(m_ui->emailEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSignInButton);
    connect(m_ui->passwordEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSignInButton);
    connect(m_ui->serverEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateSecretKeyField);
    connect(m_ui->serverEdit, &QLineEdit::textChanged, this, &AccountStartWidget::updateServerRow);
    connect(m_ui->secretKeyLink, &QLabel::linkActivated, this, [this] {
        m_secretKeyRequested = true;
        updateSecretKeyField();
        m_ui->secretKeyEdit->setFocus();
    });
    connect(m_ui->serverChangeLink, &QLabel::linkActivated, this, [this] {
        m_ui->serverChangeLink->setVisible(false);
        m_ui->serverRow->setContentsMargins(0, 0, 0, 8);
        m_ui->serverEdit->setVisible(true);
        m_ui->serverEdit->setFocus();
        m_ui->serverEdit->selectAll();
    });
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

void AccountStartWidget::showEvent(QShowEvent* event)
{
    // The application style sheet resets the labels' fonts when they're first polished, which is on show
    applyTheme();
    QWidget::showEvent(event);
}

void AccountStartWidget::resizeEvent(QResizeEvent* event)
{
    // A narrow window gives the card's fields more of its width
    const auto margins = m_ui->scrollLayout->contentsMargins();
    const int column = qMin(ColumnWidth, width() - margins.left() - margins.right());
    const int padding = column < 400 ? CompactCardPadding : CardPadding;
    m_ui->cardLayout->setContentsMargins(padding, padding, padding, padding - 4);
    QWidget::resizeEvent(event);
}

void AccountStartWidget::applyTheme()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_ui->titleLabel, Typography::FontRole::Title, p.text);
    Vault::styleLabel(m_ui->subtitleLabel, Typography::FontRole::Body, p.textSecondary);
    for (auto* heading : {m_ui->signInHeading, m_ui->mfaHeading, m_ui->selfHostedHeading}) {
        Vault::styleLabel(heading, Typography::FontRole::Subtitle, p.text);
    }
    for (auto* intro : {m_ui->signInIntro, m_ui->mfaIntro, m_ui->selfHostedIntro}) {
        Vault::styleLabel(intro, Typography::FontRole::Body, p.textSecondary);
    }
    for (auto* label : {m_ui->emailLabel,
                        m_ui->passwordLabel,
                        m_ui->secretKeyLabel,
                        m_ui->mfaCodeLabel,
                        m_ui->selfHostedUrlLabel,
                        m_ui->pairingCodeLabel}) {
        Vault::styleLabel(label, Typography::FontRole::BodyStrong, p.text);
    }
    m_ui->emailLabel->setText(requiredHtml(tr("Email"), p.critical));
    m_ui->passwordLabel->setText(requiredHtml(tr("Master password"), p.critical));
    Vault::styleLabel(m_ui->secretKeyHint, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_ui->localHint, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_ui->busyLabel, Typography::FontRole::Body, p.textSecondary);

    Vault::styleLabel(m_ui->secretKeyLink, Typography::FontRole::Body, p.accentText);
    m_ui->secretKeyLink->setText(linkHtml(tr("First time on this device? Enter your Secret Key"), p.accentText));
    Vault::styleLabel(m_ui->serverLabel, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_ui->serverValueLabel, Typography::FontRole::Caption, p.textSecondary);
    const auto caption = Vault::font(Typography::FontRole::Caption);
    auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (caption.pixelSize() > 0) {
        mono.setPixelSize(caption.pixelSize());
    } else {
        mono.setPointSizeF(caption.pointSizeF());
    }
    m_ui->serverValueLabel->setFont(mono);
    Vault::styleLabel(m_ui->serverChangeLink, Typography::FontRole::Caption, p.accentText);
    m_ui->serverChangeLink->setText(linkHtml(tr("Change"), p.accentText));
    Vault::styleLabel(m_ui->serverIcon, Typography::FontRole::Caption, p.textSecondary);
    setGlyph(m_ui->serverIcon, Typography::Icons::World, 14);
    Vault::styleLabel(m_ui->privacyIcon, Typography::FontRole::Caption, p.textSecondary);
    setGlyph(m_ui->privacyIcon, Typography::Icons::Shield, 14);

    m_ui->card->setStyleSheet(QStringLiteral("QFrame#card { background-color: %1; border: 1px solid %2;"
                                             " border-radius: 8px; }"
                                             "QStackedWidget#pages { background: transparent; border: none; }")
                                  .arg(p.layer.name(QColor::HexArgb), outlineColor(p).name(QColor::HexArgb)));
    m_ui->selfHostedButton->update();
    m_ui->localButton->update();
}

void AccountStartWidget::reset()
{
    m_secretKeyRequested = false;
    m_revealPasswordAction->setChecked(false);
    m_ui->serverEdit->setVisible(false);
    m_ui->serverChangeLink->setVisible(true);
    m_ui->serverRow->setContentsMargins(0, 0, 0, 0);
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
    updateServerRow();
    updateSignInButton();
    showPage(Page::SignIn);
}

AccountStartWidget::Page AccountStartWidget::currentPage() const
{
    return static_cast<Page>(m_ui->pages->currentIndex());
}

void AccountStartWidget::showPage(Page page)
{
    m_ui->pages->setCurrentIndex(static_cast<int>(page));
    // The card fits the page shown rather than the tallest one
    for (int i = 0; i < m_ui->pages->count(); ++i) {
        const auto policy = i == m_ui->pages->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        m_ui->pages->widget(i)->setSizePolicy(QSizePolicy::Preferred, policy);
    }
    m_ui->pages->updateGeometry();
    // The other ways to start sit beneath the sign-in card only
    m_ui->optionsWidget->setVisible(page == Page::SignIn);
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
    m_ui->optionsWidget->setEnabled(message.isEmpty());
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
    // Only a device that hasn't been trusted with the account yet asks for the Secret Key; until the email
    // is entered, the link below the password asks for it instead
    const auto email = m_ui->emailEdit->text().trimmed();
    bool needed = !email.isEmpty();
    if (m_controller && needed) {
        const auto& server = m_controller->account()->server();
        auto serverId = QUrl(m_ui->serverEdit->text().trimmed()).host();
        if (server.isValid() && QUrl(server.issuer).host() == serverId) {
            serverId = server.serverId();
        }
        needed = AccountManager::storedSecretKey(serverId, email).isEmpty();
    }
    const bool shown = needed || m_secretKeyRequested;
    m_ui->secretKeyLabel->setVisible(shown);
    m_ui->secretKeyEdit->setVisible(shown);
    m_ui->secretKeyHint->setVisible(needed);
    m_ui->secretKeyLink->setVisible(!shown);
}

void AccountStartWidget::updateServerRow()
{
    // The server reads as its host (and port), as in the browser's address bar
    const auto text = m_ui->serverEdit->text().trimmed();
    const QUrl url(text);
    auto shown = url.host();
    if (!shown.isEmpty() && url.port() != -1) {
        shown += QStringLiteral(":%1").arg(url.port());
    }
    m_ui->serverValueLabel->setText(shown.isEmpty() ? text : shown);
    m_ui->serverValueLabel->setToolTip(text);
}

void AccountStartWidget::updateSignInButton()
{
    m_ui->signInButton->setEnabled(!m_ui->emailEdit->text().trimmed().isEmpty()
                                   && !m_ui->passwordEdit->text().isEmpty());
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
