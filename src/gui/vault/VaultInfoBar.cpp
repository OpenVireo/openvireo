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

#include "VaultInfoBar.h"
#include "VaultMetrics.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <components/foundation/MotionPolicy.h>
#include <design/IconCatalog.h>
#include <design/Typography.h>

#include <QAction>
#include <QActionEvent>
#include <QDesktopServices>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <utility>

const int VaultInfoBar::DefaultAutoHideTimeout = 6000;
const int VaultInfoBar::LongAutoHideTimeout = 15000;
const int VaultInfoBar::DisableAutoHide = -1;

namespace
{
    constexpr int IconSize = 16;
    constexpr int Radius = VaultMetrics::RowRadius;
    // WinUI's InfoBar animates over the standard "fast" duration
    constexpr int AnimationMs = 167;
} // namespace

// The card: severity fill and stroke behind the icon, text and buttons. The bar clips it while it slides.
class VaultInfoBar::Content : public QWidget
{
public:
    explicit Content(VaultInfoBar* bar)
        : QWidget(bar)
        , m_bar(bar)
    {
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto p = FluentTheme::palette();
        QColor fill = p.infoBg;
        switch (m_bar->messageType()) {
        case Positive:
            fill = p.successBg;
            break;
        case Information:
            break;
        case Warning:
            fill = p.cautionBg;
            break;
        case Error:
            fill = p.criticalBg;
            break;
        }
        QColor stroke = p.stroke;
        if (p.highContrast) {
            stroke = p.text;
        } else if (stroke.alpha() < 15) {
            stroke.setAlpha(15);
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Radius, Radius);
        painter.fillPath(path, fill);
        painter.setPen(QPen(stroke, 1));
        painter.drawPath(path);
    }

private:
    VaultInfoBar* m_bar;
};

// The severity badge or custom icon, painted at full colour even while the window is disabled, as it is while a
// prompt waits for the user
class VaultInfoBar::Icon : public QWidget
{
public:
    Icon(VaultInfoBar* bar, QWidget* parent)
        : QWidget(parent)
        , m_bar(bar)
    {
        setFixedSize(IconSize, IconSize);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const auto icon = m_bar->icon();
        if (icon.isNull()) {
            paintStatusBadge(painter, QRectF(rect()), m_bar->messageType());
        } else {
            icon.paint(&painter, rect());
        }
    }

private:
    VaultInfoBar* m_bar;
};

VaultInfoBar::VaultInfoBar(QWidget* parent)
    : QFrame(parent)
    , m_content(new Content(this))
    , m_autoHideTimer(new QTimer(this))
    , m_animation(new QVariantAnimation(this))
    , m_autoHideTimeout(DefaultAutoHideTimeout)
{
    setFrameShape(QFrame::NoFrame);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    auto* layout = new QHBoxLayout(m_content);
    layout->setContentsMargins(16, 8, 8, 8);
    layout->setSpacing(12);

    // The icon sits on the first line of text, however many lines follow; applyColors() lines that first line up
    // with the middle of the close button
    m_icon = new Icon(this, m_content);
    m_icon->setObjectName("infoBarIcon");
    m_iconColumn = new QVBoxLayout;
    m_iconColumn->setContentsMargins(0, 0, 0, 0);
    m_iconColumn->addWidget(m_icon);
    m_iconColumn->addStretch();
    layout->addLayout(m_iconColumn);

    m_textColumn = new QVBoxLayout;
    m_textColumn->setContentsMargins(0, 0, 0, 0);
    m_textColumn->setSpacing(2);
    m_titleLabel = new QLabel(m_content);
    m_titleLabel->setObjectName("infoBarTitle");
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setVisible(false);
    m_textLabel = new QLabel(m_content);
    m_textLabel->setObjectName("infoBarText");
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_textLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(m_textLabel, &QLabel::linkActivated, this, &VaultInfoBar::linkActivated);
    connect(m_textLabel, &QLabel::linkHovered, this, &VaultInfoBar::linkHovered);
    m_textColumn->addWidget(m_titleLabel);
    m_textColumn->addWidget(m_textLabel);
    m_textColumn->addStretch();
    layout->addLayout(m_textColumn, 1);

    m_buttonLayout = new QHBoxLayout;
    m_buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_buttonLayout->setSpacing(8);
    layout->addLayout(m_buttonLayout);

    auto* close = Vault::iconButton(Typography::Icons::Dismiss, tr("Close message"), m_content);
    close->setObjectName("infoBarCloseButton");
    close->setFluentSize(fluent::basicinput::Button::Small);
    connect(close, &QAbstractButton::clicked, this, &VaultInfoBar::animatedHide);
    m_closeButton = close;
    layout->addWidget(m_closeButton, 0, Qt::AlignTop);

    m_autoHideTimer->setSingleShot(true);
    connect(m_autoHideTimer, &QTimer::timeout, this, &VaultInfoBar::animatedHide);
    connect(this, &VaultInfoBar::hideAnimationFinished, m_autoHideTimer, &QTimer::stop);

    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        setFixedHeight(value.toInt());
    });
    connect(m_animation, &QVariantAnimation::finished, this, &VaultInfoBar::finishAnimation);

    Vault::followTheme(this, [this] { applyColors(); });
}

VaultInfoBar::VaultInfoBar(const QString& text, QWidget* parent)
    : VaultInfoBar(parent)
{
    setText(text);
}

VaultInfoBar::~VaultInfoBar() = default;

QString VaultInfoBar::text() const
{
    return m_textLabel->text();
}

void VaultInfoBar::setText(const QString& text)
{
    m_textLabel->setText(text);
    updateGeometry();
    updateContentGeometry();
}

QString VaultInfoBar::title() const
{
    return m_titleLabel->text();
}

void VaultInfoBar::setTitle(const QString& title)
{
    m_titleLabel->setText(title);
    m_titleLabel->setVisible(!title.isEmpty());
    updateGeometry();
    updateContentGeometry();
}

VaultInfoBar::MessageType VaultInfoBar::messageType() const
{
    return m_type;
}

void VaultInfoBar::setMessageType(VaultInfoBar::MessageType type)
{
    m_type = type;
    m_icon->update();
    m_content->update();
}

bool VaultInfoBar::wordWrap() const
{
    return m_wordWrap;
}

void VaultInfoBar::setWordWrap(bool wordWrap)
{
    m_wordWrap = wordWrap;
    m_textLabel->setWordWrap(wordWrap);
    m_titleLabel->setWordWrap(wordWrap);
    auto policy = sizePolicy();
    policy.setHeightForWidth(wordWrap);
    setSizePolicy(policy);
    updateGeometry();
    updateContentGeometry();
}

bool VaultInfoBar::isCloseButtonVisible() const
{
    return !m_closeButton->isHidden();
}

void VaultInfoBar::setCloseButtonVisible(bool visible)
{
    m_closeButton->setVisible(visible);
    // Without the button the text keeps the same distance from the right edge as the icon from the left
    m_content->layout()->setContentsMargins(16, 8, visible ? 8 : 16, 8);
    updateGeometry();
    updateContentGeometry();
}

QIcon VaultInfoBar::icon() const
{
    return m_customIcon;
}

void VaultInfoBar::setIcon(const QIcon& icon)
{
    m_customIcon = icon;
    m_icon->update();
}

int VaultInfoBar::autoHideTimeout() const
{
    return m_autoHideTimeout;
}

void VaultInfoBar::setAutoHideTimeout(int autoHideTimeout)
{
    m_autoHideTimeout = autoHideTimeout;
    if (autoHideTimeout <= 0) {
        m_autoHideTimer->stop();
    }
}

void VaultInfoBar::setAnimate(bool animate)
{
    m_animate = animate;
}

void VaultInfoBar::showMessage(const QString& text, VaultInfoBar::MessageType type)
{
    showMessage(text, type, m_autoHideTimeout);
}

void VaultInfoBar::showMessage(const QString& text, VaultInfoBar::MessageType type, int autoHideTimeout)
{
    setMessageType(type);
    setText(text);
    emit showAnimationStarted();
    animatedShow();
    if (autoHideTimeout > 0) {
        m_autoHideTimer->start(autoHideTimeout);
    } else {
        m_autoHideTimer->stop();
    }
}

void VaultInfoBar::hideMessage()
{
    emit hideAnimationStarted();
    animatedHide();
    m_autoHideTimer->stop();
}

bool VaultInfoBar::shouldAnimate() const
{
    // Nothing to see while the window or a parent is hidden
    return m_animate && fluent::MotionPolicy::instance().shouldAnimate() && parentWidget()
           && parentWidget()->isVisible();
}

void VaultInfoBar::animatedShow()
{
    if (m_animation->state() == QAbstractAnimation::Running) {
        if (m_showing) {
            return;
        }
    } else if (isVisible()) {
        return;
    }
    if (!shouldAnimate()) {
        m_animation->stop();
        m_showing = true;
        show();
        finishAnimation();
        return;
    }
    show();
    startAnimation(true);
}

void VaultInfoBar::animatedHide()
{
    if (m_animation->state() == QAbstractAnimation::Running) {
        if (!m_showing) {
            return;
        }
    } else if (isHidden()) {
        return;
    }
    if (!shouldAnimate() || !isVisible()) {
        m_animation->stop();
        m_showing = false;
        finishAnimation();
        return;
    }
    startAnimation(false);
}

void VaultInfoBar::startAnimation(bool show)
{
    const int from = m_animation->state() == QAbstractAnimation::Running ? height() : (show ? 0 : height());
    m_animation->stop();
    m_showing = show;
    m_animation->setStartValue(from);
    m_animation->setEndValue(show ? contentHeight(width()) : 0);
    m_animation->setDuration(fluent::MotionPolicy::instance().resolvedDuration(AnimationMs));
    setFixedHeight(from);
    updateContentGeometry();
    m_animation->start();
}

void VaultInfoBar::finishAnimation()
{
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
    if (m_showing) {
        updateGeometry();
        updateContentGeometry();
        emit showAnimationFinished();
    } else {
        hide();
        emit hideAnimationFinished();
    }
}

bool VaultInfoBar::isShowAnimationRunning() const
{
    return m_showing && m_animation->state() == QAbstractAnimation::Running;
}

bool VaultInfoBar::isHideAnimationRunning() const
{
    return !m_showing && m_animation->state() == QAbstractAnimation::Running;
}

int VaultInfoBar::contentHeight(int width) const
{
    const int minimum = m_content->minimumSizeHint().height();
    if (width <= 0 || !m_content->layout()->hasHeightForWidth()) {
        return qMax(minimum, m_content->sizeHint().height());
    }
    return qMax(minimum, m_content->layout()->totalHeightForWidth(width));
}

QSize VaultInfoBar::sizeHint() const
{
    ensurePolished();
    const auto hint = m_content->sizeHint();
    // The layout asks for the height at the width it gave the bar last
    return {hint.width(), contentHeight(width() > 0 && isVisible() ? width() : hint.width())};
}

QSize VaultInfoBar::minimumSizeHint() const
{
    ensurePolished();
    const auto hint = m_content->minimumSizeHint();
    // Wrapped text can shrink to a few words a line
    return {m_wordWrap ? qMin(hint.width(), 120) : hint.width(), hint.height()};
}

bool VaultInfoBar::hasHeightForWidth() const
{
    return m_wordWrap;
}

int VaultInfoBar::heightForWidth(int width) const
{
    return contentHeight(width);
}

void VaultInfoBar::resizeEvent(QResizeEvent* event)
{
    QFrame::resizeEvent(event);
    updateContentGeometry();
    // A new width can mean a different number of lines
    if (m_animation->state() != QAbstractAnimation::Running) {
        const int wanted = contentHeight(width());
        if (wanted != m_lastHintHeight) {
            m_lastHintHeight = wanted;
            updateGeometry();
        }
    }
}

void VaultInfoBar::updateContentGeometry()
{
    // Full height whatever the bar's height: while it slides, the bar shows the top of the card
    m_content->setGeometry(0, 0, width(), contentHeight(width()));
}

void VaultInfoBar::actionEvent(QActionEvent* event)
{
    QFrame::actionEvent(event);
    rebuildButtons();
}

void VaultInfoBar::rebuildButtons()
{
    for (const auto& button : std::as_const(m_actionButtons)) {
        delete button;
    }
    m_actionButtons.clear();
    for (auto* action : actions()) {
        auto* button = new VaultButton(m_content);
        auto sync = [button, action] {
            button->setText(action->text());
            button->setToolTip(action->toolTip() != action->text() ? action->toolTip() : QString());
            button->setEnabled(action->isEnabled());
            button->setVisible(action->isVisible());
        };
        sync();
        connect(action, &QAction::changed, button, sync);
        connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
        m_buttonLayout->addWidget(button, 0, Qt::AlignVCenter);
        m_actionButtons.append(button);
    }
    updateGeometry();
    updateContentGeometry();
}

void VaultInfoBar::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_titleLabel, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_textLabel, Typography::FontRole::Body, p.text);
    // Links in the message take the theme's link colour
    auto palette = m_textLabel->palette();
    palette.setColor(QPalette::Link, p.link);
    palette.setColor(QPalette::LinkVisited, p.link);
    m_textLabel->setPalette(palette);

    // A single line sits in the middle of the bar beside the close button; more lines start there
    const int line = QFontMetrics(m_textLabel->font()).height();
    const int top = qMax(0, (m_closeButton->sizeHint().height() - line) / 2);
    m_textColumn->setContentsMargins(0, top, 0, 0);
    m_iconColumn->setContentsMargins(0, top + qMax(0, (line - IconSize) / 2), 0, 0);
    m_icon->update();
    m_content->update();
    updateGeometry();
}

void VaultInfoBar::paintStatusBadge(QPainter& painter, const QRectF& rect, VaultInfoBar::MessageType type)
{
    const auto p = FluentTheme::palette();
    QColor badge = p.accent;
    QString glyph = Typography::Icons::AsteriskBadge12;
    switch (type) {
    case Positive:
        badge = p.success;
        glyph = Typography::Icons::CheckmarkBadge12;
        break;
    case Information:
        break;
    case Warning:
        badge = p.caution;
        glyph = Typography::Icons::ImportantBadge12;
        break;
    case Error:
        badge = p.critical;
        glyph = Typography::Icons::ErrorBadge12;
        break;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(badge);
    painter.drawEllipse(rect.adjusted(1, 1, -1, -1));
    // Fluent-Qt's compact badge glyphs, as its InfoBar uses: the plain ones draw a second ring inside the badge
    painter.setPen(p.onAccent);
    Typography::Icons::paintGlyph(painter, rect, glyph, qRound(rect.height() * 10 / 16));
    painter.restore();
}

void VaultInfoBar::openHttpUrl(const QString& url)
{
    if (url.startsWith("http://") || url.startsWith("https://")) {
        QDesktopServices::openUrl(QUrl(url));
    }
}
