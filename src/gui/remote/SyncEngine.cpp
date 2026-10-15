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

#include "SyncEngine.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimeZone>

namespace
{
    bool readFile(const QString& filePath, QByteArray* data, QString* error)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = SyncEngine::tr("Failed to read database %1: %2").arg(filePath, file.errorString());
            return false;
        }
        *data = file.readAll();
        return true;
    }
} // namespace

SyncEngine::SyncEngine(const QSharedPointer<Database>& db,
                       SyncServerClient* client,
                       const SyncStateStore& stateStore,
                       QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_client(client)
    , m_stateStore(stateStore)
{
    m_client->setParent(this);
    m_backoffTimer.setSingleShot(true);
    connect(&m_backoffTimer, &QTimer::timeout, this, &SyncEngine::sync);
}

SyncEngine::~SyncEngine()
{
    m_client->abort();
    cleanup();
}

void SyncEngine::setMergeHandler(MergeHandler handler)
{
    m_mergeHandler = std::move(handler);
}

void SyncEngine::setSaveHandler(SaveHandler handler)
{
    m_saveHandler = std::move(handler);
}

void SyncEngine::setMergeGate(MergeGate gate)
{
    m_mergeGate = std::move(gate);
}

void SyncEngine::setRetryOnNetworkError(bool retry)
{
    m_retryOnNetworkError = retry;
}

QString SyncEngine::pendingUnlockPath() const
{
    return m_state == State::WaitingForUnlock ? m_remotePath : QString();
}

SyncEngine::State SyncEngine::state() const
{
    return m_state;
}

bool SyncEngine::isBusy() const
{
    return m_state != State::Idle && m_state != State::Backoff && m_state != State::NeedsAttention;
}

QSharedPointer<Database> SyncEngine::database() const
{
    return m_db;
}

const RemoteParams& SyncEngine::params() const
{
    return m_client->params();
}

int SyncEngine::backoffMsec() const
{
    return m_backoffMsec;
}

void SyncEngine::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);
    }
}

SyncEngine::Attention SyncEngine::attention() const
{
    return m_attention;
}

void SyncEngine::sync()
{
    if (isBusy()) {
        return;
    }
    m_acceptServerState = false;
    start();
}

void SyncEngine::syncAcceptingServerState()
{
    if (isBusy()) {
        return;
    }
    m_acceptServerState = true;
    start();
}

void SyncEngine::start()
{
    m_attention = Attention::None;
    m_backoffTimer.stop();
    m_attempt = 0;
    m_unlockedViaDialog = false;
    m_syncState = m_stateStore.load(m_db->filePath(), params());

    m_workDir.reset(new QTemporaryDir());
    if (!m_workDir->isValid()) {
        fail(tr("Failed to create a temporary folder for the sync: %1").arg(m_workDir->errorString()));
        return;
    }
    m_remotePath = m_workDir->filePath("remote.kdbx");

    startCheck();
}

void SyncEngine::cancel()
{
    const bool wasBusy = isBusy();
    m_client->abort();
    m_backoffTimer.stop();
    m_backoffMsec = 0;
    cleanup();
    setState(State::Idle);
    if (wasBusy) {
        emit finished(false, tr("The sync was cancelled."));
    }
}

bool SyncEngine::hasLocalChanges() const
{
    if (m_db->isModified() || !m_syncState.lastSync.isValid()) {
        return true;
    }
    // Saved since the last sync, by this app or by something else writing the file
    return QFileInfo(m_db->filePath()).lastModified() > m_syncState.lastSync;
}

void SyncEngine::startCheck()
{
    if (m_syncState.isEmpty() || hasLocalChanges()) {
        startDownload();
        return;
    }

    // Nothing changed here since the last sync: only download when the server has a new version
    setState(State::Checking);
    emit progress(10, tr("Checking…"));
    m_client->check(m_syncState.etag, [this](const SyncServerClient::Response& response) {
        if (response.status == 304) {
            if (acceptRevision(response.revision)) {
                succeed(m_syncState.etag, response.revision);
            }
        } else if (response.status == 200 || response.status == 404) {
            startDownload();
        } else {
            fail(SyncServerClient::errorMessage(response), response.status == 0);
        }
    });
}

void SyncEngine::startDownload()
{
    setState(State::Downloading);
    emit progress(25, tr("Downloading..."));
    m_client->download([this](const SyncServerClient::Response& response) {
        if (response.status == 404) {
            if (!m_acceptServerState && m_syncState.revision > 0) {
                needAttention(Attention::ServerLostDatabase,
                              tr("The sync server no longer has this database, although this device synced it "
                                 "before. It may have been deleted or the server reset."));
                return;
            }
            // Nothing on the server yet: publish the local database as the first version
            QString error;
            if (m_db->isModified() && (!m_saveHandler || !m_saveHandler(&error))) {
                fail(error.isEmpty() ? tr("The database must be saved before it can be uploaded.") : error);
                return;
            }
            m_remoteEtag.clear();
            m_remoteRevision = -1;
            uploadFile(m_db->filePath());
            return;
        }
        if (response.status != 200) {
            fail(SyncServerClient::errorMessage(response), response.status == 0);
            return;
        }
        if (!acceptRevision(response.revision)) {
            return;
        }

        QFile file(m_remotePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size()) {
            fail(tr("Failed to write downloaded database to %1: %2").arg(m_remotePath, file.errorString()));
            return;
        }
        file.close();
        m_remoteEtag = response.etag;
        m_remoteRevision = response.revision;
        mergeDownload();
    });
}

void SyncEngine::mergeDownload()
{
    if (m_mergeGate && !m_mergeGate()) {
        setState(State::WaitingToMerge);
        emit mergeDeferred();
        return;
    }

    setState(State::Merging);
    emit progress(50, tr("Syncing..."));

    m_remoteDb = QSharedPointer<Database>::create();
    if (!m_remoteDb->open(m_remotePath, m_db->key())) {
        // Another device changed the password or key file: the owner asks the user to unlock it
        m_remoteDb.reset();
        setState(State::WaitingForUnlock);
        emit unlockRequired(m_remotePath);
        return;
    }
    m_remoteDb->markAsTemporaryDatabase();

    // The revision comes from the server, the generation from inside the encrypted copy
    m_remoteGeneration = generationOf(m_remoteDb.data());
    if (!m_acceptServerState && m_remoteGeneration < m_syncState.generation) {
        needAttention(Attention::ServerRolledBack,
                      tr("The sync server returned a copy of the database that is older than one this device has "
                         "already synced (sync generation %1, expected at least %2).")
                          .arg(m_remoteGeneration)
                          .arg(m_syncState.generation));
        return;
    }

    const auto conflicts = findConflicts(m_db.data(), m_remoteDb.data(), m_syncState.entries);

    QString error;
    if (!m_mergeHandler || !m_mergeHandler(m_remoteDb, &error)) {
        fail(error.isEmpty() ? tr("The databases could not be merged.") : error);
        return;
    }
    m_remoteDb.reset();
    if (!conflicts.isEmpty()) {
        emit conflictsDetected(conflicts);
    }
    uploadFile(m_remotePath);
}

void SyncEngine::resumeMerge()
{
    if (m_state == State::WaitingToMerge && (!m_mergeGate || m_mergeGate())) {
        mergeDownload();
    }
}

void SyncEngine::resumeAfterUnlock(bool merged, const QString& error)
{
    if (m_state != State::WaitingForUnlock) {
        return;
    }
    if (!merged) {
        fail(error.isEmpty() ? tr("Remote database unlock cancelled.") : error);
        return;
    }
    m_unlockedViaDialog = true;
    uploadFile(m_remotePath);
}

void SyncEngine::uploadFile(const QString& filePath)
{
    QByteArray data;
    QString error;
    if (!readFile(filePath, &data, &error)) {
        fail(error);
        return;
    }
    startUpload(data);
}

void SyncEngine::startUpload(const QByteArray& data)
{
    // The merge didn't change the server copy, so there is nothing to upload
    if (!m_remoteEtag.isEmpty() && SyncServerClient::etagFor(data) == m_remoteEtag) {
        succeed(m_remoteEtag, m_remoteRevision);
        return;
    }

    setState(State::Uploading);
    emit progress(75, tr("Uploading..."));
    m_client->upload(data, m_remoteEtag, [this](const SyncServerClient::Response& response) {
        if (response.status == 200 || response.status == 201) {
            succeed(response.etag, response.revision);
            return;
        }
        if (response.status == 412) {
            // Another device uploaded since our download: merge its version too and try again
            if (!m_unlockedViaDialog && ++m_attempt < MaxAttempts) {
                startDownload();
                return;
            }
            fail(tr("The database on the sync server was changed by another device during the sync. "
                    "Your local changes are saved, sync again to merge them."));
            return;
        }
        fail(tr("Failed to upload merged database. %1").arg(SyncServerClient::errorMessage(response)),
             response.status == 0);
    });
}

void SyncEngine::succeed(const QString& etag, qint64 revision)
{
    SyncState state;
    state.etag = etag;
    // Accepting the server's state starts the comparison over from what it has now
    const auto& seen = m_acceptServerState ? SyncState() : m_syncState;
    state.revision = qMax(seen.revision, revision);
    state.generation = qMax(qMax(seen.generation, m_remoteGeneration), generationOf(m_db.data()));
    state.entries = snapshotOf(m_db.data());
    state.lastSync = QDateTime::currentDateTimeUtc();
    m_stateStore.save(m_db->filePath(), params(), state);
    m_syncState = state;

    cleanup();
    m_backoffMsec = 0;
    m_acceptServerState = false;
    setState(State::Idle);
    emit finished(true, {});
}

bool SyncEngine::acceptRevision(qint64 revision)
{
    if (m_acceptServerState || revision < 0 || revision >= m_syncState.revision) {
        return true;
    }
    needAttention(Attention::ServerRolledBack,
                  tr("The sync server returned an older version of the database (revision %1) than this device has "
                     "already synced (revision %2).")
                      .arg(revision)
                      .arg(m_syncState.revision));
    return false;
}

void SyncEngine::needAttention(Attention attention, const QString& message)
{
    // Nothing is merged or uploaded, and nothing retries until the user decides
    cleanup();
    m_backoffTimer.stop();
    m_backoffMsec = 0;
    m_attention = attention;
    setState(State::NeedsAttention);
    emit finished(false, message);
}

qint64 SyncEngine::generationOf(const Database* db)
{
    return db->metadata()->customData()->value(CustomData::SyncGeneration).toLongLong();
}

Merger::ChangeList SyncEngine::mergeDatabases(Database* local, Database* remote)
{
    Merger firstMerge(local, remote);
    Merger secondMerge(remote, local);
    auto changes = firstMerge.merge() + secondMerge.merge();
    if (!changes.isEmpty()) {
        const auto generation = QString::number(qMax(generationOf(local), generationOf(remote)) + 1);
        local->metadata()->customData()->set(CustomData::SyncGeneration, generation);
        remote->metadata()->customData()->set(CustomData::SyncGeneration, generation);
    }
    return changes;
}

QHash<QUuid, QDateTime> SyncEngine::snapshotOf(const Database* db)
{
    QHash<QUuid, QDateTime> snapshot;
    for (const auto* entry : db->rootGroup()->entriesRecursive()) {
        // Saved files keep whole seconds, so compare at that precision
        const auto seconds = entry->timeInfo().lastModificationTime().toSecsSinceEpoch();
        snapshot.insert(entry->uuid(), QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::utc()));
    }
    return snapshot;
}

QList<SyncConflict>
SyncEngine::findConflicts(const Database* local, const Database* remote, const QHash<QUuid, QDateTime>& snapshot)
{
    QList<SyncConflict> conflicts;
    const auto changedSince = [](const Entry* entry, const QDateTime& base) {
        return entry->timeInfo().lastModificationTime().toSecsSinceEpoch() != base.toSecsSinceEpoch();
    };
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        const auto* localEntry = local->rootGroup()->findEntryByUuid(it.key());
        const auto* remoteEntry = remote->rootGroup()->findEntryByUuid(it.key());
        SyncConflict conflict;
        conflict.uuid = it.key();
        if (localEntry && remoteEntry) {
            if (!changedSince(localEntry, it.value()) || !changedSince(remoteEntry, it.value())
                || !changedSince(localEntry, remoteEntry->timeInfo().lastModificationTime())) {
                continue;
            }
            conflict.kind = SyncConflict::Kind::EditedOnBoth;
            conflict.title = localEntry->title();
        } else if (remoteEntry && local->containsDeletedObject(it.key()) && changedSince(remoteEntry, it.value())) {
            conflict.kind = SyncConflict::Kind::DeletedHereEditedThere;
            conflict.title = remoteEntry->title();
        } else if (localEntry && remote->containsDeletedObject(it.key()) && changedSince(localEntry, it.value())) {
            conflict.kind = SyncConflict::Kind::EditedHereDeletedThere;
            conflict.title = localEntry->title();
        } else {
            continue;
        }
        conflicts << conflict;
    }
    return conflicts;
}

void SyncEngine::fail(const QString& error, bool networkError)
{
    cleanup();
    if (networkError && m_retryOnNetworkError) {
        m_backoffMsec = m_backoffMsec == 0 ? FirstBackoffMsec : qMin(m_backoffMsec * 2, MaxBackoffMsec);
        m_backoffTimer.start(m_backoffMsec);
        setState(State::Backoff);
    } else {
        m_backoffMsec = 0;
        setState(State::Idle);
    }
    emit finished(false, error);
}

void SyncEngine::cleanup()
{
    m_remoteDb.reset();
    m_workDir.reset();
    m_remotePath.clear();
    m_remoteEtag.clear();
    m_remoteRevision = -1;
    m_remoteGeneration = 0;
}
