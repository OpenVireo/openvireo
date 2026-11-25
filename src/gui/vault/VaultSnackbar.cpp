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

#include "VaultSnackbar.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <components/foundation/MotionPolicy.h>
#include <design/Typography.h>

#include <QAbstractButton>
#include <QAccessible>
#include <QAction>
#include <QActionEvent>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVariantAnimation>

const int VaultSnackbar::MaximumWidth = 560;
const int VaultSnackbar::Margin = 24;

namespace
{
    constexpr int Radius = 14;
    constexpr int BadgeSize = 22;
    constexpr int SideMargin = 16;
    // Start of the text: margin, badge and the gap after it
    constexpr int TextStart = SideMargin + BadgeSize + 12;
    constexpr int TimerLine = 3;
    constexpr int SlideDuration = 200;

    // The card is the inverse of the window: dark in the light theme, light in the dark theme
    struct Colors
    {
        QColor fill;
        QColor text;
        QColor action;
        QColor hover;
        QColor glyph;
        QColor stroke;
        QColor success;
        QColor critical;
        QColor caution;
        QColor info;
    };

    Colors colors()
    {
        const auto p = FluentTheme::palette();
        if (p.highContrast) {
            // The scheme's own colors, outlined, so nothing relies on the fill
            return {p.layer, p.text, p.link, p.subtleHover, p.layer, p.text, p.text, p.text, p.text, p.text};
        }
        if (FluentTheme::isDark()) {
            return {QColor("#eef1ed"),
                    QColor("#101511"),
                    QColor("#2c5e17"),
                    QColor(0, 0, 0, 18),
                    QColor("#ffffff"),
                    QColor(0, 0, 0, 0),
                    QColor("#3c7d20"),
                    QColor("#b42318"),
                    QColor("#8a5a00"),
                    QColor("#2459a8")};
        }
        return {QColor("#07100a"),
                QColor("#f2f4f1"),
                QColor("#9fd37f"),
                QColor(255, 255, 255, 22),
                QColor("#07100a"),
                QColor(0, 0, 0, 0),
                QColor("#9fd37f"),
                QColor("#f4a59d"),
                QColor("#f2c46d"),
                QColor("#a9c4f2")};
    }

    QColor badgeColor(const Colors& c, VaultInfoBar::MessageType type)
    {
        switch (type) {
        case VaultInfoBar::Positive:
            return c.success;
        case VaultInfoBar::Warning:
            return c.caution;
        case VaultInfoBar::Error:
            return c.critical;
        case VaultInfoBar::Information:
            break;
        }
        return c.info;
    }

    void paintBadge(QPainter& painter, const QRectF& rect, VaultInfoBar::MessageType type, const Colors& c)
    {
        const bool highContrast = FluentTheme::palette().highContrast;
        painter.save();
        painter.setPen(highContrast ? QPen(c.text, 1.5) : Qt::NoPen);
        painter.setBrush(highContrast ? QColor(Qt::transparent) : badgeColor(c, type));
        painter.drawEllipse(rect.adjusted(0.75, 0.75, -0.75, -0.75));

        const QColor ink = highContrast ? c.text : c.glyph;
        QPen pen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const qreal s = rect.width() / 24.0;
        const QPointF o = rect.topLeft();
        auto at = [&](qreal x, qreal y) { return o + QPointF(x * s, y * s); };
        switch (type) {
        case VaultInfoBar::Positive: {
            QPainterPath check;
            check.moveTo(at(7, 12.5));
            check.lineTo(at(10.5, 16));
            check.lineTo(at(17, 8.5));
            painter.drawPath(check);
            break;
        }
        case VaultInfoBar::Information:
            painter.drawLine(at(12, 11), at(12, 17));
            painter.drawPoint(at(12, 7.5));
            break;
        case VaultInfoBar::Warning:
        case VaultInfoBar::Error:
            painter.drawLine(at(12, 7), at(12, 13));
            painter.drawPoint(at(12, 16.5));
            break;
        }
        painter.restore();
    }

    QString plainText(const QString& text)
    {
        return Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    }
} // namespace

/**
 * A flat button drawn in the card's own colors: an action in bold accent text, or the close cross.
 */
class VaultSnackbar::TextButton : public QAbstractButton
{
public:
    explicit TextButton(QWidget* parent)
        : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
    }

    void setCross(bool cross)
    {
        m_cross = cross;
        updateGeometry();
    }

    QSize sizeHint() const override
    {
        if (m_cross) {
            return {30, 30};
        }
        QFont font = Vault::font(Typography::FontRole::BodyStrong);
        return {QFontMetrics(font).horizontalAdvance(text()) + 24, 30};
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)
        const auto c = colors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (underMouse() || isDown()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(c.hover);
            painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        if (hasFocus()) {
            painter.setPen(QPen(c.text, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        }
        if (m_cross) {
            QColor ink = c.text;
            ink.setAlphaF(0.7);
            painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap));
            const QPointF m = r.center();
            painter.drawLine(m + QPointF(-4.5, -4.5), m + QPointF(4.5, 4.5));
            painter.drawLine(m + QPointF(4.5, -4.5), m + QPointF(-4.5, 4.5));
            return;
        }
        painter.setFont(Vault::font(Typography::FontRole::BodyStrong));
        painter.setPen(c.action);
        painter.drawText(r, Qt::AlignCenter, text());
    }

private:
    bool m_cross = false;
};

VaultSnackbar::VaultSnackbar(QWidget* parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
    , m_textLabel(new QLabel(this))
    , m_buttonLayout(new QHBoxLayout())
    , m_closeButton(new TextButton(this))
    , m_hideTimer(new QTimer(this))
    , m_tick(new QTimer(this))
    , m_animation(new QVariantAnimation(this))
{
    Q_ASSERT(parent);
    setObjectName("vaultSnackbar");
    setAttribute(Qt::WA_Hover);
    setAccessibleDescription(tr("Message"));
    hide();

    m_layout->setContentsMargins(TextStart, 10, 8, 10 + TimerLine / 2);
    m_layout->setSpacing(6);
    m_textLabel->setObjectName("vaultSnackbarText");
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextFormat(Qt::AutoText);
    m_textLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_textLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    connect(m_textLabel, &QLabel::linkActivated, this, &VaultSnackbar::linkActivated);
    connect(m_textLabel, &QLabel::linkActivated, &VaultInfoBar::openHttpUrl);
    m_layout->addWidget(m_textLabel, 1);
    m_buttonLayout->setSpacing(2);
    m_layout->addLayout(m_buttonLayout);
    m_closeButton->setObjectName("vaultSnackbarClose");
    m_closeButton->setCross(true);
    m_closeButton->setToolTip(tr("Dismiss"));
    m_closeButton->setAccessibleName(tr("Dismiss"));
    connect(m_closeButton, &QAbstractButton::clicked, this, &VaultSnackbar::animatedHide);
    m_layout->addWidget(m_closeButton, 0, Qt::AlignVCenter);

    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, &VaultSnackbar::animatedHide);
    // Repaints the countdown line
    m_tick->setInterval(50);
    connect(m_tick, &QTimer::timeout, this, qOverload<>(&QWidget::update));

    m_animation->setDuration(SlideDuration);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_slide = value.toReal();
        reposition();
    });
    connect(m_animation, &QVariantAnimation::finished, this, [this] {
        if (!m_showing) {
            hide();
        }
    });

    parent->installEventFilter(this);
    Vault::followTheme(this, [this] { applyColors(); });
    applyColors();
}

VaultSnackbar::~VaultSnackbar() = default;

QString VaultSnackbar::text() const
{
    return m_textLabel->text();
}

VaultInfoBar::MessageType VaultSnackbar::messageType() const
{
    return m_type;
}

int VaultSnackbar::autoHideTimeout() const
{
    return m_autoHideTimeout;
}

bool VaultSnackbar::isCloseButtonVisible() const
{
    return !m_closeButton->isHidden();
}

void VaultSnackbar::setAnimate(bool animate)
{
    m_animate = animate;
}

bool VaultSnackbar::isAnimating() const
{
    return m_animation->state() == QAbstractAnimation::Running;
}

qreal VaultSnackbar::remainingFraction() const
{
    if (m_autoHideTimeout <= 0) {
        return 1;
    }
    const int left = m_hideTimer->isActive() ? m_hideTimer->remainingTime() : m_remaining;
    return qBound(0.0, qreal(left) / m_autoHideTimeout, 1.0);
}

void VaultSnackbar::setCloseButtonVisible(bool visible)
{
    m_closeButton->setVisible(visible);
    reposition();
}

void VaultSnackbar::showMessage(const QString& text, VaultInfoBar::MessageType type, int autoHideTimeout)
{
    m_type = type;
    m_autoHideTimeout = autoHideTimeout;
    m_textLabel->setText(text);
    setAccessibleName(plainText(text));

    const bool wasShowing = m_showing && isVisible();
    m_showing = true;
    raise();
    if (wasShowing || !shouldAnimate()) {
        // A new message replaces the one on screen in place
        m_animation->stop();
        m_slide = 1;
        show();
        reposition();
    } else {
        m_slide = 0;
        reposition();
        show();
        startAnimation(true);
    }

    m_remaining = autoHideTimeout;
    if (autoHideTimeout > 0) {
        m_hideTimer->start(autoHideTimeout);
        m_tick->start();
        if (underMouse()) {
            pauseTimer();
        }
    } else {
        m_hideTimer->stop();
        m_tick->stop();
    }
    update();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Screen readers hear it without it taking focus; errors interrupt, the rest wait their turn
    QAccessibleAnnouncementEvent announcement(this, plainText(text));
    announcement.setPoliteness(type == VaultInfoBar::Error ? QAccessible::AnnouncementPoliteness::Assertive
                                                           : QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&announcement);
#endif
}

void VaultSnackbar::hideMessage()
{
    m_showing = false;
    m_hideTimer->stop();
    m_tick->stop();
    m_animation->stop();
    hide();
}

void VaultSnackbar::animatedHide()
{
    if (!isVisible() || !shouldAnimate()) {
        hideMessage();
        return;
    }
    m_showing = false;
    m_hideTimer->stop();
    m_tick->stop();
    startAnimation(false);
}

bool VaultSnackbar::shouldAnimate() const
{
    return m_animate && fluent::MotionPolicy::instance().shouldAnimate() && parentWidget()->isVisible();
}

void VaultSnackbar::startAnimation(bool show)
{
    m_animation->stop();
    m_animation->setStartValue(m_slide);
    m_animation->setEndValue(show ? 1.0 : 0.0);
    m_animation->start();
}

int VaultSnackbar::cardWidth() const
{
    const int available = parentWidget()->width() - 2 * SideMargin;
    // As wide as the message needs, up to the maximum, and never wider than the parent
    const int buttons = m_buttonLayout->sizeHint().width() + (isCloseButtonVisible() ? 36 : 0);
    const QFontMetrics metrics(m_textLabel->font());
    int textWidth = 0;
    for (const auto& line : plainText(m_textLabel->text()).split('\n')) {
        textWidth = qMax(textWidth, metrics.horizontalAdvance(line));
    }
    const int wanted = TextStart + textWidth + 8 + buttons + 16;
    return qBound(qMin(320, available), wanted, qMin(MaximumWidth, available));
}

int VaultSnackbar::restingY() const
{
    return parentWidget()->height() - height() - Margin;
}

QSize VaultSnackbar::sizeHint() const
{
    const int width = cardWidth();
    const int height = qMax(52, m_layout->hasHeightForWidth() ? m_layout->totalHeightForWidth(width) : 52);
    return {width, height};
}

void VaultSnackbar::reposition()
{
    if (!parentWidget()) {
        return;
    }
    const QSize size = sizeHint();
    const int x = (parentWidget()->width() - size.width()) / 2;
    const int rest = parentWidget()->height() - size.height() - Margin;
    // Slides up from just below the bottom edge
    const int hidden = parentWidget()->height();
    const int y = qRound(hidden + (rest - hidden) * m_slide);
    setGeometry(x, y, size.width(), size.height());
}

void VaultSnackbar::pauseTimer()
{
    if (m_hideTimer->isActive()) {
        m_remaining = m_hideTimer->remainingTime();
        m_hideTimer->stop();
        m_tick->stop();
        update();
    }
}

void VaultSnackbar::resumeTimer()
{
    if (m_showing && m_autoHideTimeout > 0 && !m_hideTimer->isActive()) {
        // Some time to read the end of it after the pointer leaves
        m_hideTimer->start(qMax(m_remaining, 1500));
        m_tick->start();
    }
}

bool VaultSnackbar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}

void VaultSnackbar::enterEvent(QEnterEvent* event)
{
    pauseTimer();
    QWidget::enterEvent(event);
}

void VaultSnackbar::leaveEvent(QEvent* event)
{
    resumeTimer();
    QWidget::leaveEvent(event);
}

void VaultSnackbar::actionEvent(QActionEvent* event)
{
    QWidget::actionEvent(event);
    rebuildButtons();
}

void VaultSnackbar::rebuildButtons()
{
    for (const auto& button : std::as_const(m_actionButtons)) {
        delete button.data();
    }
    m_actionButtons.clear();
    const auto actionList = actions();
    for (auto* action : actionList) {
        auto* button = new TextButton(this);
        button->setText(action->text());
        button->setAccessibleName(action->text());
        connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
        connect(button, &QAbstractButton::clicked, this, &VaultSnackbar::animatedHide);
        m_buttonLayout->addWidget(button, 0, Qt::AlignVCenter);
        m_actionButtons.append(button);
    }
    reposition();
}

void VaultSnackbar::applyColors()
{
    const auto c = colors();
    Vault::styleLabel(m_textLabel, Typography::FontRole::Body, c.text);
    QPalette palette = m_textLabel->palette();
    palette.setColor(QPalette::Link, c.action);
    palette.setColor(QPalette::LinkVisited, c.action);
    m_textLabel->setPalette(palette);
    update();
}

void VaultSnackbar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto c = colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const bool highContrast = FluentTheme::palette().highContrast;
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(card, Radius, Radius);
    painter.fillPath(path, c.fill);
    if (highContrast) {
        painter.setPen(QPen(c.stroke, 2));
        painter.drawPath(path);
    }

    const QRectF badge(SideMargin, (height() - BadgeSize) / 2.0, BadgeSize, BadgeSize);
    paintBadge(painter, badge, m_type, c);

    // The countdown along the bottom edge
    if (m_autoHideTimeout > 0 && m_showing) {
        painter.save();
        painter.setClipPath(path);
        QColor line = badgeColor(c, m_type);
        painter.fillRect(QRectF(0, height() - TimerLine, width() * remainingFraction(), TimerLine), line);
        painter.restore();
    }
}
