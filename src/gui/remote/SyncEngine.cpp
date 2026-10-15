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

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

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
    return m_state != State::Idle && m_state != State::Backoff;
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

void SyncEngine::sync()
{
    if (isBusy()) {
        return;
    }
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
            succeed(m_syncState.etag, response.revision);
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

    QString error;
    if (!m_mergeHandler || !m_mergeHandler(m_remoteDb, &error)) {
        fail(error.isEmpty() ? tr("The databases could not be merged.") : error);
        return;
    }
    m_remoteDb.reset();
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
    state.revision = qMax(m_syncState.revision, revision);
    state.lastSync = QDateTime::currentDateTimeUtc();
    m_stateStore.save(m_db->filePath(), params(), state);
    m_syncState = state;

    cleanup();
    m_backoffMsec = 0;
    setState(State::Idle);
    emit finished(true, {});
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
}
