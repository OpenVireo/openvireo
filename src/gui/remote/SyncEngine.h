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

#ifndef KEEPASSXC_SYNCENGINE_H
#define KEEPASSXC_SYNCENGINE_H

#include "SyncServerClient.h"
#include "SyncStateStore.h"

#include <QObject>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QTimer>

#include <functional>

class Database;
class QTemporaryDir;

/**
 * Syncs one local database with one database on a sync server without blocking the event loop:
 *
 *   check (HEAD) ─304─▶ done
 *        │ changed, or local changes
 *        ▼
 *   download ─▶ [merge gate] ─▶ open with the local key ─▶ merge ─▶ upload ─▶ done
 *        ▲         │ closed                 │ wrong key                  │ 412
 *        │         ▼                        ▼                            │
 *        │   mergeDeferred()         unlockRequired()                    │
 *        └──────────── retry, up to MaxAttempts ─────┘
 *
 * The merge and the save are delegated to the owner (DatabaseWidget), which knows how to
 * save with the user's settings. Opening the downloaded database runs on the GUI thread,
 * like unlocking does.
 */
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,
        Checking,
        Downloading,
        Merging,
        // Downloaded, but the merge gate is closed (the user is editing); resumeMerge() continues
        WaitingToMerge,
        WaitingForUnlock,
        Uploading,
        // A network error happened and a retry is scheduled
        Backoff
    };
    Q_ENUM(State)

    // Merge the opened server copy and the local database into each other and save both
    using MergeHandler = std::function<bool(const QSharedPointer<Database>& remoteDb, QString* error)>;
    // Save the local database
    using SaveHandler = std::function<bool(QString* error)>;
    // Whether the local database may be merged into right now
    using MergeGate = std::function<bool()>;

    static constexpr int MaxAttempts = 3;
    static constexpr int FirstBackoffMsec = 30 * 1000;
    static constexpr int MaxBackoffMsec = 15 * 60 * 1000;

    // Takes ownership of the client
    SyncEngine(const QSharedPointer<Database>& db,
               SyncServerClient* client,
               const SyncStateStore& stateStore = SyncStateStore(),
               QObject* parent = nullptr);
    ~SyncEngine() override;

    void setMergeHandler(MergeHandler handler);
    void setSaveHandler(SaveHandler handler);
    void setMergeGate(MergeGate gate);
    // Retry with a growing delay after network errors, instead of only reporting them
    void setRetryOnNetworkError(bool retry);

    State state() const;
    // True while a sync is running; a scheduled retry doesn't count
    bool isBusy() const;
    QSharedPointer<Database> database() const;
    const RemoteParams& params() const;
    int backoffMsec() const;
    // The downloaded server copy waiting for the user to unlock it, while WaitingForUnlock
    QString pendingUnlockPath() const;

public slots:
    // Start a sync, or run a scheduled retry now. Does nothing while a sync is running.
    void sync();
    // Stop the running sync or scheduled retry. A running sync finishes with an error.
    void cancel();
    // Continue after unlockRequired(): `merged` is true when the owner unlocked the server
    // copy at the given path and merged it, false when the user cancelled or it failed
    void resumeAfterUnlock(bool merged, const QString& error = {});
    // Continue a merge held back by the merge gate, if the gate is open now
    void resumeMerge();

signals:
    void stateChanged(SyncEngine::State state);
    void progress(int percent, const QString& message);
    // The server copy can't be opened with the local key; the owner asks the user to unlock it
    void unlockRequired(const QString& filePath);
    // The merge waits until the gate opens and resumeMerge() is called
    void mergeDeferred();
    void finished(bool success, const QString& error);

private:
    void setState(State state);
    bool hasLocalChanges() const;
    void startCheck();
    void startDownload();
    void mergeDownload();
    void uploadFile(const QString& filePath);
    void startUpload(const QByteArray& data);
    void succeed(const QString& etag, qint64 revision);
    void fail(const QString& error, bool networkError = false);
    void cleanup();

    QSharedPointer<Database> m_db;
    SyncServerClient* m_client;
    SyncStateStore m_stateStore;
    MergeHandler m_mergeHandler;
    SaveHandler m_saveHandler;
    MergeGate m_mergeGate;

    State m_state = State::Idle;
    SyncState m_syncState;
    int m_attempt = 0;
    // The last merge went through the unlock dialog, so a conflict isn't retried without the user
    bool m_unlockedViaDialog = false;

    QScopedPointer<QTemporaryDir> m_workDir;
    QString m_remotePath;
    QString m_remoteEtag;
    qint64 m_remoteRevision = -1;
    QSharedPointer<Database> m_remoteDb;

    bool m_retryOnNetworkError = false;
    int m_backoffMsec = 0;
    QTimer m_backoffTimer;

    Q_DISABLE_COPY(SyncEngine)
};

#endif // KEEPASSXC_SYNCENGINE_H
