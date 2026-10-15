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

#ifndef KEEPASSXC_SYNCSCHEDULER_H
#define KEEPASSXC_SYNCSCHEDULER_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

/**
 * Decides when a database with automatic sync should sync in the background:
 * shortly after a save, when asked to (unlock), every poll interval, when the
 * network comes back and when the app comes to the front.
 *
 * It only emits syncRequested(); the owner decides whether a sync can run right now.
 */
class SyncScheduler : public QObject
{
    Q_OBJECT

public:
    static constexpr int DefaultSaveDelayMsec = 3 * 1000;
    static constexpr int DefaultPollIntervalMsec = 60 * 1000;
    // Coming online or to the front again soon after a sync doesn't sync again
    static constexpr int DefaultMinEventGapMsec = 10 * 1000;

    explicit SyncScheduler(QObject* parent = nullptr);

    void setEnabled(bool enabled);
    bool isEnabled() const;
    void setSaveDelay(int msec);
    void setPollInterval(int msec);
    void setMinEventGap(int msec);

public slots:
    // A save happened: sync once the saves settle down
    void notifySaved();
    // Sync now, e.g. after unlocking or when the user asks
    void requestNow();
    // The network came back or the app came to the front: sync unless one was just requested
    void notifyEvent();

signals:
    void syncRequested();

private:
    void request();

    bool m_enabled = false;
    QTimer m_saveTimer;
    QTimer m_pollTimer;
    int m_minEventGapMsec = DefaultMinEventGapMsec;
    QElapsedTimer m_sinceLastRequest;
};

#endif // KEEPASSXC_SYNCSCHEDULER_H
