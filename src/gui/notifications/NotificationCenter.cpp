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

#include "NotificationCenter.h"

#include "core/Clock.h"
#include "core/Global.h"

#include <algorithm>

const int NotificationCenter::MaxEntries = 50;
const int NotificationCenter::DuplicateWindowMs = 10000;

namespace
{
    int severityRank(MessageWidget::MessageType type)
    {
        switch (type) {
        case MessageWidget::Error:
            return 2;
        case MessageWidget::Warning:
            return 1;
        case MessageWidget::Positive:
        case MessageWidget::Information:
            break;
        }
        return 0;
    }
} // namespace

NotificationCenter::NotificationCenter(QObject* parent)
    : QObject(parent)
{
}

quint64 NotificationCenter::post(const QString& text, MessageWidget::MessageType type, const QString& source)
{
    // A repeat of a recent message, such as the same sync error, refreshes it instead of stacking up
    const auto now = Clock::currentDateTime();
    for (auto& entry : m_entries) {
        if (!entry.pinned && entry.text == text && entry.type == type && entry.time.msecsTo(now) < DuplicateWindowMs) {
            entry.time = now;
            entry.read = false;
            entry.source = source;
            const auto id = entry.id;
            moveToTop(id);
            emit changed();
            emit posted(id);
            return id;
        }
    }

    Notification notification;
    notification.type = type;
    notification.text = text;
    notification.source = source;
    return add(notification);
}

quint64 NotificationCenter::add(Notification notification)
{
    notification.id = m_nextId++;
    if (!notification.time.isValid()) {
        notification.time = Clock::currentDateTime();
    }
    const auto id = notification.id;
    m_entries.append(std::move(notification));
    moveToTop(id);
    trim();
    emit changed();
    emit posted(id);
    return id;
}

void NotificationCenter::dismiss(quint64 id)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            const auto onDismiss = m_entries.at(i).onDismiss;
            m_entries.removeAt(i);
            if (onDismiss) {
                onDismiss();
            }
            emit changed();
            return;
        }
    }
}

void NotificationCenter::dismissLatestFrom(const QString& source)
{
    // Entries are newest first, so the first match is the latest
    for (const auto& entry : asConst(m_entries)) {
        if (!entry.pinned && entry.source == source) {
            dismiss(entry.id);
            return;
        }
    }
}

void NotificationCenter::clearAll()
{
    const auto removed =
        std::remove_if(m_entries.begin(), m_entries.end(), [](const Notification& entry) { return !entry.pinned; });
    if (removed == m_entries.end()) {
        return;
    }
    m_entries.erase(removed, m_entries.end());
    emit changed();
}

void NotificationCenter::markAllRead()
{
    bool marked = false;
    for (auto& entry : m_entries) {
        if (!entry.read) {
            entry.read = true;
            marked = true;
        }
    }
    if (marked) {
        emit changed();
    }
}

QList<NotificationCenter::Notification> NotificationCenter::notifications() const
{
    return m_entries;
}

std::optional<NotificationCenter::Notification> NotificationCenter::find(quint64 id) const
{
    for (const auto& entry : m_entries) {
        if (entry.id == id) {
            return entry;
        }
    }
    return {};
}

bool NotificationCenter::isEmpty() const
{
    return m_entries.isEmpty();
}

int NotificationCenter::unreadCount() const
{
    return std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Notification& entry) { return !entry.read; });
}

std::optional<MessageWidget::MessageType> NotificationCenter::highestUnreadType() const
{
    std::optional<MessageWidget::MessageType> highest;
    for (const auto& entry : m_entries) {
        if (!entry.read && (!highest || severityRank(entry.type) > severityRank(*highest))) {
            highest = entry.type;
        }
    }
    return highest;
}

void NotificationCenter::moveToTop(quint64 id)
{
    // Newest first, pinned entries above the rest
    int from = -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            from = i;
            break;
        }
    }
    if (from < 0) {
        return;
    }
    int to = 0;
    if (!m_entries.at(from).pinned) {
        while (to < m_entries.size() && m_entries.at(to).pinned && to != from) {
            ++to;
        }
    }
    m_entries.move(from, to > from ? from : to);
}

void NotificationCenter::trim()
{
    // Drop the oldest unpinned entries, which are at the end
    while (m_entries.size() > MaxEntries) {
        auto oldest =
            std::find_if(m_entries.rbegin(), m_entries.rend(), [](const Notification& entry) { return !entry.pinned; });
        if (oldest == m_entries.rend()) {
            return;
        }
        m_entries.erase(std::next(oldest).base());
    }
}
