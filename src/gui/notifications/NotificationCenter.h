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

#ifndef KEEPASSXC_NOTIFICATIONCENTER_H
#define KEEPASSXC_NOTIFICATIONCENTER_H

#include "gui/MessageWidget.h"

#include <QDateTime>
#include <QList>
#include <QObject>

#include <functional>
#include <optional>

/**
 * The messages the main window has shown this session, newest first with pinned ones on top.
 *
 * Global messages land here instead of replacing each other in one bar; the header's bell shows how many are
 * unread and lists them. Nothing is kept across restarts.
 */
class NotificationCenter : public QObject
{
    Q_OBJECT

public:
    struct Action
    {
        QString text;
        std::function<void()> trigger;
    };

    struct Notification
    {
        quint64 id = 0;
        MessageWidget::MessageType type = MessageWidget::Information;
        QString title;
        QString text;
        QDateTime time;
        QList<Action> actions;
        // Who posted it, so they can take it back again (see dismissLatestFrom)
        QString source;
        // Pinned entries stay on top and survive Clear all
        bool pinned = false;
        bool read = false;
        // Runs when the user dismisses it, not when it is cleared or replaced
        std::function<void()> onDismiss;
    };

    static const int MaxEntries;
    static const int DuplicateWindowMs;

    explicit NotificationCenter(QObject* parent = nullptr);

    // Add a message; the same text and type again within a few seconds bumps the existing entry instead
    quint64 post(const QString& text, MessageWidget::MessageType type, const QString& source = {});
    quint64 add(Notification notification);
    // Remove an entry as the user dismissing it, which runs its onDismiss
    void dismiss(quint64 id);
    void dismissLatestFrom(const QString& source);
    // Remove everything except pinned entries
    void clearAll();
    void markAllRead();

    QList<Notification> notifications() const;
    std::optional<Notification> find(quint64 id) const;
    bool isEmpty() const;
    int unreadCount() const;
    // The most serious type among the unread entries: Error, then Warning, then the rest
    std::optional<MessageWidget::MessageType> highestUnreadType() const;

signals:
    void changed();
    // A new entry, or an existing one bumped by a duplicate
    void posted(quint64 id);

private:
    void moveToTop(quint64 id);
    void trim();

    QList<Notification> m_entries;
    quint64 m_nextId = 1;
};

#endif // KEEPASSXC_NOTIFICATIONCENTER_H
