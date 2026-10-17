/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
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

#include "RemoteHandler.h"

#include "config-keepassx.h"

#include "RemoteProcess.h"
#include "RemoteSettings.h"
#include "SyncServerClient.h"

#include "core/AsyncTask.h"
#include "core/Database.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QUuid>

namespace
{
    QString getTempFileLocation()
    {
        QString uuid = QUuid::createUuid().toString().remove(0, 1);
        uuid.chop(1);
        return QDir::toNativeSeparators(QDir::temp().absoluteFilePath("RemoteDatabase-" + uuid + ".kdbx"));
    }

    /*
     * Run one sync server request and wait for it.
     * Called from an AsyncTask worker thread, so the client and its event loop live there.
     */
    SyncServerClient::Response
    waitForSyncServer(const RemoteParams* params,
                      const std::function<void(SyncServerClient&, SyncServerClient::Callback)>& request)
    {
        SyncServerClient client(*params);
        SyncServerClient::Response response;
        QEventLoop loop;
        request(client, [&](const SyncServerClient::Response& reply) {
            response = reply;
            loop.quit();
        });
        loop.exec();
        return response;
    }
} // namespace

std::function<QScopedPointer<RemoteProcess>(QObject*)> RemoteHandler::m_createRemoteProcess([](QObject* parent) {
    return QScopedPointer<RemoteProcess>(new RemoteProcess(parent));
});

RemoteHandler::RemoteHandler(QObject* parent)
    : QObject(parent)
{
}

void RemoteHandler::setRemoteProcessFunc(std::function<QScopedPointer<RemoteProcess>(QObject*)> func)
{
    m_createRemoteProcess = std::move(func);
}

RemoteHandler::RemoteResult RemoteHandler::download(const RemoteParams* params)
{
    return AsyncTask::runAndWaitForFuture([params] {
        RemoteResult result;
        if (!params) {
            result.success = false;
            result.errorMessage = tr("Invalid download parameters provided.");
            return result;
        }

        if (params->isServer()) {
            return downloadFromSyncServer(params);
        }

        auto filePath = getTempFileLocation();
        auto remoteProcess = m_createRemoteProcess(nullptr); // use nullptr parent, otherwise there is a warning
        remoteProcess->setTempFileLocation(filePath);
        remoteProcess->start(params->downloadCommand);
        if (!params->downloadInput.isEmpty()) {
            remoteProcess->write(params->downloadInput + "\n");
            remoteProcess->waitForBytesWritten();
            remoteProcess->closeWriteChannel();
        }

        bool finished = remoteProcess->waitForFinished(params->downloadTimeoutMsec);
        int statusCode = remoteProcess->exitCode();

        // TODO: For future use
        result.stdOutput = remoteProcess->readOutput();
        result.stdError = remoteProcess->readError();

        if (finished && statusCode == 0) {
            // Check if the file actually downloaded
            QFileInfo fileInfo(filePath);
            if (!fileInfo.exists() || fileInfo.size() == 0) {
                result.success = false;
                result.errorMessage = tr("Command `%1` failed to download database.").arg(params->downloadCommand);
            } else {
                result.success = true;
                result.filePath = filePath;
            }
        } else if (finished) {
            result.success = false;
            result.errorMessage =
                tr("Command `%1` exited with status code: %2").arg(params->downloadCommand).arg(statusCode);
        } else {
            remoteProcess->kill();
            result.success = false;
            result.errorMessage =
                tr("Command `%1` did not finish in time. Process was killed.").arg(params->downloadCommand);
        }

        return result;
    });
}

RemoteHandler::RemoteResult
RemoteHandler::upload(const QString& filePath, const RemoteParams* params, const QString& etag)
{
    return AsyncTask::runAndWaitForFuture([filePath, params, etag] {
        RemoteResult result;
        if (!params) {
            result.success = false;
            result.errorMessage = tr("Invalid database pointer or upload parameters provided.");
            return result;
        }

        if (params->isServer()) {
            return uploadToSyncServer(filePath, params, etag);
        }

        auto remoteProcess = m_createRemoteProcess(nullptr); // use nullptr parent, otherwise there is a warning
        remoteProcess->setTempFileLocation(filePath);
        remoteProcess->start(params->uploadCommand);
        if (!params->uploadInput.isEmpty()) {
            remoteProcess->write(params->uploadInput + "\n");
            remoteProcess->waitForBytesWritten();
            remoteProcess->closeWriteChannel();
        }

        bool finished = remoteProcess->waitForFinished(params->uploadTimeoutMsec);
        int statusCode = remoteProcess->exitCode();

        // TODO: For future use
        result.stdOutput = remoteProcess->readOutput();
        result.stdError = remoteProcess->readError();

        if (finished && statusCode == 0) {
            result.success = true;
        } else if (finished) {
            result.success = false;
            result.errorMessage = tr("Failed to upload merged database. Command `%1` exited with status code: %2")
                                      .arg(params->uploadCommand)
                                      .arg(statusCode);
        } else {
            remoteProcess->kill();
            result.success = false;
            result.errorMessage =
                tr("Failed to upload merged database. Command `%1` did not finish in time. Process was killed.")
                    .arg(params->uploadCommand);
        }

        return result;
    });
}

RemoteHandler::RemoteResult RemoteHandler::downloadFromSyncServer(const RemoteParams* params)
{
    RemoteResult result;
    result.success = false;
    auto response = waitForSyncServer(
        params, [](SyncServerClient& client, SyncServerClient::Callback done) { client.download(std::move(done)); });
    if (response.status == 404) {
        // Nothing uploaded yet, the first sync publishes the local database
        result.success = true;
        result.remoteMissing = true;
        return result;
    }
    if (response.status != 200) {
        result.errorMessage = SyncServerClient::errorMessage(response);
        return result;
    }

    auto filePath = getTempFileLocation();
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size()) {
        result.errorMessage = tr("Failed to write downloaded database to %1: %2").arg(filePath, file.errorString());
        file.remove();
        return result;
    }
    file.close();

    result.success = true;
    result.filePath = filePath;
    result.etag = response.etag;
    return result;
}

RemoteHandler::RemoteResult
RemoteHandler::uploadToSyncServer(const QString& filePath, const RemoteParams* params, const QString& etag)
{
    RemoteResult result;
    result.success = false;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errorMessage = tr("Failed to read database %1: %2").arg(filePath, file.errorString());
        return result;
    }
    auto data = file.readAll();
    file.close();

    // The server's ETag is the SHA-256 of its copy, so an unchanged merge needs no upload
    if (!etag.isEmpty() && etag == SyncServerClient::etagFor(data)) {
        result.success = true;
        result.etag = etag;
        return result;
    }

    auto response = waitForSyncServer(params, [&](SyncServerClient& client, SyncServerClient::Callback done) {
        client.upload(data, etag, std::move(done));
    });
    if (response.status == 412) {
        result.errorMessage = tr("The database on the sync server was changed by another device during the sync. "
                                 "Your local changes are saved, sync again to merge them.");
        return result;
    }
    if (response.status != 200 && response.status != 201) {
        result.errorMessage = tr("Failed to upload merged database. %1").arg(SyncServerClient::errorMessage(response));
        return result;
    }

    result.success = true;
    result.filePath = filePath;
    result.etag = response.etag;
    return result;
}

RemoteHandler::SyncServerListing RemoteHandler::listSyncServerDatabases(const RemoteParams* params)
{
    return AsyncTask::runAndWaitForFuture([params] {
        SyncServerListing listing;
        auto response = waitForSyncServer(params, [](SyncServerClient& client, SyncServerClient::Callback done) {
            client.listDatabases(std::move(done));
        });
        if (response.status != 200) {
            listing.errorMessage = SyncServerClient::errorMessage(response);
            return listing;
        }
        for (const auto& database : QJsonDocument::fromJson(response.body).object().value("databases").toArray()) {
            listing.databaseIds << database.toObject().value("id").toString();
        }
        listing.success = true;
        return listing;
    });
}

RemoteHandler::SyncServerPairing RemoteHandler::createSyncServerPairingCode(const RemoteParams* params)
{
    return AsyncTask::runAndWaitForFuture([params] {
        SyncServerPairing pairing;
        auto response = waitForSyncServer(params, [](SyncServerClient& client, SyncServerClient::Callback done) {
            client.createPairingCode(std::move(done));
        });
        const auto body = QJsonDocument::fromJson(response.body).object();
        pairing.code = body.value("code").toString();
        if (response.status != 201 || pairing.code.isEmpty()) {
            pairing.errorMessage = SyncServerClient::errorMessage(response);
            return pairing;
        }
        pairing.expiresAt = QDateTime::fromString(body.value("expiresAt").toString(), Qt::ISODateWithMs);
        pairing.success = true;
        return pairing;
    });
}

RemoteHandler::SyncServerPairing RemoteHandler::claimSyncServerPairingCode(const RemoteParams* params,
                                                                           const QString& code)
{
    // The code stands in for a token; never send one along with it
    RemoteParams anonymous = *params;
    anonymous.type = RemoteParams::Type::SyncServer;
    anonymous.serverToken.clear();
    anonymous.tokenProvider = nullptr;
    const auto name = deviceName();
    return AsyncTask::runAndWaitForFuture([anonymous, code, name] {
        SyncServerPairing pairing;
        auto response = waitForSyncServer(&anonymous, [&](SyncServerClient& client, SyncServerClient::Callback done) {
            client.claimPairingCode(code, name, std::move(done));
        });
        pairing.token = QJsonDocument::fromJson(response.body).object().value("token").toString();
        if (response.status != 201 || pairing.token.isEmpty()) {
            pairing.errorMessage = SyncServerClient::errorMessage(response);
            return pairing;
        }
        pairing.success = true;
        return pairing;
    });
}

QString RemoteHandler::deviceName()
{
    auto name = QSysInfo::machineHostName().trimmed();
    // macOS reports its Bonjour name, e.g. "Alices-MacBook.local"
    if (name.endsWith(".local", Qt::CaseInsensitive)) {
        name.chop(6);
    }
    if (name.isEmpty() || name == "localhost") {
        name = QSysInfo::prettyProductName();
    }
    // The server takes up to 64 bytes
    while (name.toUtf8().size() > 64) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("KeePassXC") : name;
}
