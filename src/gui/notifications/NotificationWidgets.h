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

#ifndef KEEPASSXC_NOTIFICATIONWIDGETS_H
#define KEEPASSXC_NOTIFICATIONWIDGETS_H

#include "gui/notifications/NotificationCenter.h"

#include <QAbstractButton>
#include <QPointer>

#include <components/dialogs_flyouts/Flyout.h>
#include <components/foundation/FluentElement.h>

class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace fluent::status_info
{
    class InfoBadge;
}

/**
 * The bell that opens the notification list, with the unread count on a badge colored by the most serious
 * unread message.
 */
class NotificationButton : public QAbstractButton, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit NotificationButton(NotificationCenter* center, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void onThemeUpdated() override;

    // The bell with its badge painted in, for a toolbar action
    static QIcon icon(const NotificationCenter* center);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void refresh();
    void placeBadge();

    NotificationCenter* m_center;
    fluent::status_info::InfoBadge* m_badge;
    bool m_hovered = false;
};

/**
 * The "Nightly" / "Preview" / "Dev build" pill shown on builds that aren't stable releases.
 */
class BuildBadge : public QAbstractButton, public fluent::FluentElement
{
    Q_OBJECT

public:
    explicit BuildBadge(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void onThemeUpdated() override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/**
 * The list behind the bell: the pinned pre-release notice first, then messages newest first.
 * Opening it and closing it again marks everything read.
 */
class NotificationFlyout : public fluent::dialogs_flyouts::Flyout
{
    Q_OBJECT

public:
    explicit NotificationFlyout(NotificationCenter* center, QWidget* parent = nullptr);

    // Open at the anchor with one entry focused, or the first one when focusId is 0
    void showFor(QWidget* anchor, quint64 focusId = 0);

private:
    void rebuild();
    QWidget* createRow(const NotificationCenter::Notification& notification);

    NotificationCenter* m_center;
    QScrollArea* m_scroll;
    QWidget* m_list;
    QVBoxLayout* m_listLayout;
    QLabel* m_empty;
    QWidget* m_clearAll;
    quint64 m_focusId = 0;
};

/**
 * A short look at a new error next to the bell, so it isn't missed. It doesn't take focus and closes by itself.
 */
class NotificationPeek : public fluent::dialogs_flyouts::Flyout
{
    Q_OBJECT

public:
    explicit NotificationPeek(QWidget* parent = nullptr);

    static const int Duration;

    void peek(QWidget* anchor, const NotificationCenter::Notification& notification);

signals:
    void showAllRequested();

private:
    QLabel* m_text;
    QWidget* m_badge;
    QTimer* m_timer;
    MessageWidget::MessageType m_type = MessageWidget::Error;
};

// "Just now", "5 min ago", or the time or date for older entries
QString notificationRelativeTime(const QDateTime& time);

#endif // KEEPASSXC_NOTIFICATIONWIDGETS_H
