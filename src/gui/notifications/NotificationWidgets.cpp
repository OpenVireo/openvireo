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

#include "NotificationWidgets.h"

#include "core/BuildInfo.h"
#include "core/Clock.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVBoxLayout>

#include <components/basicinput/HyperlinkButton.h>
#include <components/status_info/InfoBadge.h>

namespace
{
    // Fluent System Icons in the Fluent-Qt icon font
    const QChar BellGlyph(0xF114);
    const QChar BeakerGlyph(0xF1D6);

    using fluent::status_info::InfoBadge;

    InfoBadge::InfoBadgeStatus badgeStatus(std::optional<MessageWidget::MessageType> type)
    {
        if (type == MessageWidget::Error) {
            return InfoBadge::InfoBadgeStatus::Critical;
        }
        if (type == MessageWidget::Warning) {
            return InfoBadge::InfoBadgeStatus::Caution;
        }
        return InfoBadge::InfoBadgeStatus::Attention;
    }

    QColor badgeColor(std::optional<MessageWidget::MessageType> type)
    {
        const auto p = FluentTheme::palette();
        if (type == MessageWidget::Error) {
            return p.critical;
        }
        if (type == MessageWidget::Warning) {
            return p.caution;
        }
        return p.accent;
    }

    // Message text as the old message bar showed it: rich text when it looks like it, with web links opening
    QLabel* messageLabel(QLabel* label)
    {
        label->setWordWrap(true);
        label->setTextFormat(Qt::AutoText);
        QObject::connect(label, &QLabel::linkActivated, &MessageWidget::openHttpUrl);
        return label;
    }

    QString plainText(const QString& text)
    {
        return Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    }

    QLabel* themedLabel(const QString& text, Typography::FontRole role, bool secondary, QWidget* parent)
    {
        auto* label = new QLabel(text, parent);
        Vault::followTheme(label, [label, role, secondary] {
            const auto p = FluentTheme::palette();
            Vault::styleLabel(label, role, secondary ? p.textSecondary : p.text);
        });
        return label;
    }

    // The round status badge of a message, as at the start of a message bar
    class StatusBadge : public QWidget
    {
    public:
        StatusBadge(MessageWidget::MessageType type, QWidget* parent)
            : QWidget(parent)
            , m_type(type)
        {
            setFixedSize(16, 16);
            Vault::followTheme(this, [this] { update(); });
        }

        void setType(MessageWidget::MessageType type)
        {
            m_type = type;
            update();
        }

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            QPainter painter(this);
            KMessageWidget::paintStatusBadge(painter, rect(), m_type);
        }

    private:
        MessageWidget::MessageType m_type;
    };

    const QString RowName = QStringLiteral("notificationRow");

    // One entry in the notification list. Arrow keys move between entries and Delete dismisses one.
    class NotificationRow : public QFrame
    {
    public:
        NotificationRow(NotificationCenter* center,
                        const NotificationCenter::Notification& notification,
                        QWidget* parent)
            : QFrame(parent)
            , m_center(center)
            , m_id(notification.id)
            , m_pinned(notification.pinned)
            , m_unread(!notification.read && !notification.pinned)
        {
            setObjectName(RowName);
            setFocusPolicy(Qt::StrongFocus);
            setAttribute(Qt::WA_Hover);
            setAccessibleName(notification.title.isEmpty() ? plainText(notification.text)
                                                           : notification.title + ". " + plainText(notification.text));
            Vault::followTheme(this, [this] { update(); });
        }

        quint64 id() const
        {
            return m_id;
        }

        // The rows of a list, top to bottom
        static QList<NotificationRow*> rowsOf(const QWidget* list)
        {
            QList<NotificationRow*> rows;
            const auto children = list->findChildren<QWidget*>(RowName, Qt::FindDirectChildrenOnly);
            for (auto* child : children) {
                rows.append(static_cast<NotificationRow*>(child));
            }
            return rows;
        }

        void dismiss()
        {
            // The list is rebuilt when an entry goes, which deletes this row, so let the click finish first
            QMetaObject::invokeMethod(
                m_center, [center = m_center, id = m_id] { center->dismiss(id); }, Qt::QueuedConnection);
        }

    protected:
        void keyPressEvent(QKeyEvent* event) override
        {
            switch (event->key()) {
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                dismiss();
                return;
            case Qt::Key_Up:
            case Qt::Key_Down: {
                const auto rows = rowsOf(parentWidget());
                const int index = rows.indexOf(this) + (event->key() == Qt::Key_Down ? 1 : -1);
                if (index >= 0 && index < rows.size()) {
                    rows.at(index)->setFocus(Qt::TabFocusReason);
                }
                return;
            }
            default:
                break;
            }
            QFrame::keyPressEvent(event);
        }

        void focusInEvent(QFocusEvent* event) override
        {
            QFrame::focusInEvent(event);
            // The focus ring is for keyboard users; opening the list with the mouse focuses the first entry too
            m_keyboardFocus = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
            update();
        }

        void focusOutEvent(QFocusEvent* event) override
        {
            QFrame::focusOutEvent(event);
            update();
        }

        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            path.addRoundedRect(r, 6, 6);
            if (m_pinned) {
                painter.fillPath(path, p.cautionBg);
            }
            if (underMouse()) {
                painter.fillPath(path, p.subtleHover);
            }
            if (hasFocus() && m_keyboardFocus) {
                painter.setPen(QPen(p.text, 2));
                painter.drawPath(path);
            }
            if (m_unread) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(p.accent);
                painter.drawEllipse(QPointF(r.right() - 10, r.top() + 14), 4, 4);
            }
        }

    private:
        NotificationCenter* m_center;
        quint64 m_id;
        bool m_pinned;
        bool m_unread;
        bool m_keyboardFocus = false;
    };
} // namespace

QString notificationRelativeTime(const QDateTime& time)
{
    const auto now = Clock::currentDateTime();
    const qint64 seconds = time.secsTo(now);
    if (seconds < 60) {
        return NotificationFlyout::tr("Just now");
    }
    if (seconds < 60 * 60) {
        return NotificationFlyout::tr("%n min ago", nullptr, int(seconds / 60));
    }
    if (time.date() == now.date()) {
        return QLocale().toString(time.time(), QLocale::ShortFormat);
    }
    return QLocale().toString(time.date(), QLocale::ShortFormat);
}

NotificationButton::NotificationButton(NotificationCenter* center, QWidget* parent)
    : QAbstractButton(parent)
    , m_center(center)
    , m_badge(new InfoBadge(this))
{
    setObjectName("notificationButton");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setToolTip(tr("Notifications"));

    m_badge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_badge->setDisplayMode(InfoBadge::InfoBadgeDisplayMode::Value);

    connect(m_center, &NotificationCenter::changed, this, &NotificationButton::refresh);
    refresh();
}

QSize NotificationButton::sizeHint() const
{
    return {32, 32};
}

void NotificationButton::onThemeUpdated()
{
    update();
}

void NotificationButton::refresh()
{
    const int unread = m_center->unreadCount();
    m_badge->setValue(unread);
    m_badge->setStatus(badgeStatus(m_center->highestUnreadType()));
    m_badge->setVisible(unread > 0);
    placeBadge();
    setAccessibleName(unread > 0 ? tr("Notifications, %n unread", nullptr, unread) : tr("Notifications"));
    update();
}

void NotificationButton::placeBadge()
{
    const auto size = m_badge->sizeHint();
    m_badge->setGeometry(width() - size.width(), 0, size.width(), size.height());
}

QIcon NotificationButton::icon(const NotificationCenter* center)
{
    const auto p = FluentTheme::palette();
    QPixmap pixmap(QSize(48, 48));
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(20);
    painter.setFont(glyphFont);
    painter.setPen(p.text);
    painter.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, BellGlyph);
    if (center && center->unreadCount() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(badgeColor(center->highestUnreadType()));
        painter.drawEllipse(QPointF(18, 6), 4.5, 4.5);
    }
    painter.end();
    return QIcon(pixmap);
}

void NotificationButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 4, 4);
    if (isDown()) {
        painter.fillPath(path, p.subtlePressed);
    } else if (m_hovered) {
        painter.fillPath(path, p.subtleHover);
    }
    if (hasFocus()) {
        painter.setPen(QPen(p.text, 2));
        painter.drawPath(path);
    }

    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(16);
    painter.setFont(glyphFont);
    painter.setPen(p.text);
    painter.drawText(r, Qt::AlignCenter, BellGlyph);
}

void NotificationButton::resizeEvent(QResizeEvent* event)
{
    QAbstractButton::resizeEvent(event);
    placeBadge();
}

void NotificationButton::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void NotificationButton::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

BuildBadge::BuildBadge(QWidget* parent)
    : QAbstractButton(parent)
{
    setObjectName("buildBadge");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setText(BuildInfo::shortLabel());
    setAccessibleName(BuildInfo::accessibleName());
    setAccessibleDescription(BuildInfo::noticeMessage());
    setToolTip(BuildInfo::noticeMessage());
    setVisible(!BuildInfo::isStable());
}

QSize BuildBadge::sizeHint() const
{
    auto font = Vault::font(Typography::FontRole::Caption);
    font.setWeight(QFont::DemiBold);
    return {QFontMetrics(font).horizontalAdvance(text()) + 34, 22};
}

void BuildBadge::onThemeUpdated()
{
    update();
}

void BuildBadge::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Caution colors: "not a stable release", in the same tone in every theme
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, r.height() / 2, r.height() / 2);
    painter.fillPath(path, p.cautionBg);
    if (underMouse() || isDown()) {
        painter.fillPath(path, isDown() ? p.subtlePressed : p.subtleHover);
    }
    painter.setPen(QPen(hasFocus() ? p.text : p.stroke, hasFocus() ? 2 : 1));
    painter.drawPath(path);

    QFont glyphFont(Typography::FontFamily::FluentIcons);
    glyphFont.setPixelSize(12);
    painter.setFont(glyphFont);
    painter.setPen(p.caution);
    const QRectF glyphRect(r.left() + 8, r.top(), 12, r.height());
    painter.drawText(glyphRect, Qt::AlignCenter, BeakerGlyph);

    auto font = Vault::font(Typography::FontRole::Caption);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.drawText(r.adjusted(24, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
}

NotificationFlyout::NotificationFlyout(NotificationCenter* center, QWidget* parent)
    : Flyout(parent)
    , m_center(center)
{
    setObjectName("notificationFlyout");
    setPlacement(Auto);
    setAnchorOffset(6);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(contentsMargins() + QMargins(8, 10, 8, 8));
    layout->setSpacing(4);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(8, 0, 0, 4);
    header->addWidget(themedLabel(tr("Notifications"), Typography::FontRole::BodyStrong, false, this));
    header->addStretch();
    auto* clearAll = new fluent::basicinput::HyperlinkButton(tr("Clear all"), this);
    connect(clearAll, &QAbstractButton::clicked, m_center, &NotificationCenter::clearAll);
    m_clearAll = clearAll;
    header->addWidget(clearAll);
    layout->addLayout(header);

    m_list = new QWidget();
    m_list->setAutoFillBackground(false);
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(4);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFixedWidth(392);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->setWidget(m_list);
    layout->addWidget(m_scroll);

    m_empty = themedLabel(tr("No notifications"), Typography::FontRole::Body, true, this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setContentsMargins(0, 16, 0, 20);
    layout->addWidget(m_empty);

    connect(m_center, &NotificationCenter::changed, this, &NotificationFlyout::rebuild);
    // Everything shown has now been seen
    connect(this, &Popup::closed, m_center, &NotificationCenter::markAllRead);
    connect(this, &Popup::opened, this, [this] {
        const auto rows = NotificationRow::rowsOf(m_list);
        for (auto* row : rows) {
            if (m_focusId == 0 || row->id() == m_focusId) {
                row->setFocus(Qt::OtherFocusReason);
                m_scroll->ensureWidgetVisible(row);
                return;
            }
        }
    });
    rebuild();
}

void NotificationFlyout::showFor(QWidget* anchor, quint64 focusId)
{
    m_focusId = focusId;
    rebuild();
    showAt(anchor);
}

void NotificationFlyout::rebuild()
{
    // Keep focus in the list when the focused entry goes away
    const bool hadFocus = m_list->isAncestorOf(QApplication::focusWidget());

    while (auto* item = m_listLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const auto notifications = m_center->notifications();
    for (const auto& notification : notifications) {
        m_listLayout->addWidget(createRow(notification));
    }

    const bool empty = notifications.isEmpty();
    m_scroll->setVisible(!empty);
    m_empty->setVisible(empty);
    bool anyUnpinned = false;
    for (const auto& notification : notifications) {
        anyUnpinned |= !notification.pinned;
    }
    m_clearAll->setVisible(anyUnpinned);

    // Grow with the entries up to a limit, then scroll
    int contentHeight = m_listLayout->totalHeightForWidth(m_scroll->width());
    if (contentHeight < 0) {
        contentHeight = m_list->sizeHint().height();
    }
    m_scroll->setFixedHeight(qBound(0, contentHeight, 420));
    adjustSize();

    if (hadFocus) {
        const auto rows = NotificationRow::rowsOf(m_list);
        if (!rows.isEmpty()) {
            rows.first()->setFocus(Qt::OtherFocusReason);
        } else {
            setFocus(Qt::OtherFocusReason);
        }
    }
}

QWidget* NotificationFlyout::createRow(const NotificationCenter::Notification& notification)
{
    auto* row = new NotificationRow(m_center, notification, m_list);
    auto* layout = new QGridLayout(row);
    layout->setContentsMargins(12, 10, 6, 10);
    layout->setHorizontalSpacing(10);
    layout->setVerticalSpacing(4);

    layout->addWidget(new StatusBadge(notification.type, row), 0, 0, Qt::AlignTop | Qt::AlignHCenter);

    auto* body = new QVBoxLayout();
    body->setSpacing(4);
    if (!notification.title.isEmpty()) {
        auto* title = themedLabel(notification.title, Typography::FontRole::BodyStrong, false, row);
        title->setWordWrap(true);
        body->addWidget(title);
    }
    body->addWidget(messageLabel(themedLabel(notification.text, Typography::FontRole::Body, false, row)));

    auto* meta = new QHBoxLayout();
    meta->setSpacing(12);
    meta->addWidget(themedLabel(notification.pinned ? tr("Pinned") : notificationRelativeTime(notification.time),
                                Typography::FontRole::Caption,
                                true,
                                row));
    for (const auto& action : notification.actions) {
        auto* link = new fluent::basicinput::HyperlinkButton(action.text, row);
        const auto trigger = action.trigger;
        connect(link, &QAbstractButton::clicked, this, [this, trigger] {
            close();
            if (trigger) {
                QMetaObject::invokeMethod(this, trigger, Qt::QueuedConnection);
            }
        });
        meta->addWidget(link);
    }
    meta->addStretch();
    body->addLayout(meta);
    layout->addLayout(body, 0, 1);

    auto* dismiss = Vault::iconButton(Typography::Icons::Dismiss, tr("Dismiss"), row);
    connect(dismiss, &QAbstractButton::clicked, row, &NotificationRow::dismiss);
    layout->addWidget(dismiss, 0, 2, Qt::AlignTop);
    layout->setColumnStretch(1, 1);
    // Room for the unread dot above the dismiss button
    layout->setColumnMinimumWidth(2, 32);

    return row;
}

const int NotificationPeek::Duration = 6000;

NotificationPeek::NotificationPeek(QWidget* parent)
    : Flyout(parent)
    , m_timer(new QTimer(this))
{
    setObjectName("notificationPeek");
    setPlacement(Auto);
    setAnchorOffset(6);
    // A glance, not an interruption: typing carries on and clicks elsewhere go through
    setFocusOnOpenEnabled(false);
    setLightDismissConsumesPress(false);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(contentsMargins() + QMargins(4, 0, 0, 0));
    layout->setSpacing(10);
    auto* badge = new StatusBadge(m_type, this);
    m_badge = badge;
    layout->addWidget(badge, 0, Qt::AlignVCenter);
    m_text = messageLabel(themedLabel({}, Typography::FontRole::Body, false, this));
    m_text->setFixedWidth(280);
    layout->addWidget(m_text, 1);
    auto* showAll = new fluent::basicinput::HyperlinkButton(tr("Show all"), this);
    connect(showAll, &QAbstractButton::clicked, this, [this] {
        close();
        emit showAllRequested();
    });
    layout->addWidget(showAll, 0, Qt::AlignVCenter);

    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { close(); });
}

void NotificationPeek::peek(QWidget* anchor, const NotificationCenter::Notification& notification)
{
    m_type = notification.type;
    static_cast<StatusBadge*>(m_badge)->setType(m_type);
    m_text->setText(notification.title.isEmpty() ? notification.text : notification.title + "\n" + notification.text);
    adjustSize();
    showAt(anchor);
    m_timer->start(Duration);
}
