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

#include "core/Merger.h"

#include <QObject>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QTimer>

#include <functional>

class Database;
class Entry;
class QTemporaryDir;

/**
 * An entry that changed on both sides since the last sync, found before merging.
 */
struct SyncConflict
{
    enum class Kind
    {
        EditedOnBoth,
        // Deleted on this device, edited on another
        DeletedHereEditedThere,
        // Edited on this device, deleted on another
        EditedHereDeletedThere
    };
    QUuid uuid;
    QString title;
    Kind kind = Kind::EditedOnBoth;
    // EditedOnBoth: the fields both sides changed differently, where the newer change was kept
    QStringList fields;
};

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
 * The server is trusted to store files, not to be honest about them. A sync stops in NeedsAttention,
 * without merging, when the server returns an older revision than this device has seen, a copy whose
 * sync generation (kept inside the encrypted database) is older than one already seen, or nothing at
 * all for a database this device has synced before. syncAcceptingServerState() continues anyway.
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
        Backoff,
        // The server's copy looks older than expected; see attention()
        NeedsAttention
    };
    Q_ENUM(State)

    enum class Attention
    {
        None,
        // The server returned an older version than this device has already seen
        ServerRolledBack,
        // The server no longer has a database this device synced with before
        ServerLostDatabase
    };
    Q_ENUM(Attention)

    // Merge the opened server copy and the local database into each other (with mergeDatabases(), using
    // `base` when there is one) and save both
    using MergeHandler =
        std::function<bool(const QSharedPointer<Database>& remoteDb, const Database* base, QString* error)>;
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
    // True while a sync is running; a scheduled retry or a stop for attention doesn't count
    bool isBusy() const;
    QSharedPointer<Database> database() const;
    const RemoteParams& params() const;
    int backoffMsec() const;
    // The downloaded server copy waiting for the user to unlock it, while WaitingForUnlock
    QString pendingUnlockPath() const;
    Attention attention() const;
    // The server version this device last synced with, empty before the first sync
    QString lastSyncedEtag() const;

    // The sync generation stored in a database, 0 when it has none
    static qint64 generationOf(const Database* db);
    // Merge the two databases into each other. When that changes anything, both get the next sync
    // generation, so the result is newer than every copy it came from. The caller saves both.
    //
    // With `base`, the copy both sides last synced from, entries changed on both sides are combined
    // field by field first: a field changed on one side only takes that change, and only a field both
    // changed differently falls back to the newer one. An entry changed on one side only keeps that
    // change even when the device's clock made it look older.
    static Merger::ChangeList mergeDatabases(Database* local, Database* remote, const Database* base = nullptr);
    // The fields `local` and `remote` both changed differently since `base`, by name
    static QStringList conflictingFields(const Entry* local, const Entry* remote, const Entry* base);
    // Entry modification times, to be compared with the next sync's copies
    static QHash<QUuid, QDateTime> snapshotOf(const Database* db);
    // Entries both sides changed since `snapshot`, which is the state after the last sync
    static QList<SyncConflict>
    findConflicts(const Database* local, const Database* remote, const QHash<QUuid, QDateTime>& snapshot);

public slots:
    // Start a sync, or run a scheduled retry now. Does nothing while a sync is running.
    void sync();
    // Sync even though the server's copy looks older than expected, and take what the server
    // has as the new starting point. Only after the user agreed to it.
    void syncAcceptingServerState();
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
    // Entries changed on both sides were merged; the older version of each is in its history
    void conflictsDetected(const QList<SyncConflict>& conflicts);
    void finished(bool success, const QString& error);

private:
    void setState(State state);
    void start();
    bool hasLocalChanges() const;
    bool acceptRevision(qint64 revision);
    void needAttention(Attention attention, const QString& message);
    void startCheck();
    void startDownload();
    void mergeDownload();
    void uploadFile(const QString& filePath);
    void startUpload(const QByteArray& data);
    // `serverData` is what the server now has, kept as the base for the next merge; empty keeps the old base
    void succeed(const QString& etag, qint64 revision, const QByteArray& serverData = {});
    QSharedPointer<Database> openBase();
    void fail(const QString& error, bool networkError = false);
    void cleanup();

    QSharedPointer<Database> m_db;
    SyncServerClient* m_client;
    SyncStateStore m_stateStore;
    MergeHandler m_mergeHandler;
    SaveHandler m_saveHandler;
    MergeGate m_mergeGate;

    State m_state = State::Idle;
    Attention m_attention = Attention::None;
    bool m_acceptServerState = false;
    SyncState m_syncState;
    int m_attempt = 0;
    // The last merge went through the unlock dialog, so a conflict isn't retried without the user
    bool m_unlockedViaDialog = false;

    QScopedPointer<QTemporaryDir> m_workDir;
    QString m_remotePath;
    QString m_remoteEtag;
    qint64 m_remoteRevision = -1;
    qint64 m_remoteGeneration = 0;
    // What the running sync is uploading
    QByteArray m_uploadData;
    QSharedPointer<Database> m_remoteDb;

    bool m_retryOnNetworkError = false;
    int m_backoffMsec = 0;
    QTimer m_backoffTimer;

    Q_DISABLE_COPY(SyncEngine)
};

#endif // KEEPASSXC_SYNCENGINE_H
