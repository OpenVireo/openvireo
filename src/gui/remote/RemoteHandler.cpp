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

#include "core/AsyncTask.h"
#include "core/Database.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#ifdef KPXC_FEATURE_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#endif

namespace
{
    QString getTempFileLocation()
    {
        QString uuid = QUuid::createUuid().toString().remove(0, 1);
        uuid.chop(1);
        return QDir::toNativeSeparators(QDir::temp().absoluteFilePath("RemoteDatabase-" + uuid + ".kdbx"));
    }

#ifdef KPXC_FEATURE_NETWORK
    struct HttpResponse
    {
        int status = 0;
        QByteArray body;
        QByteArray etag;
        QString networkError;
    };

    // The database URL, or the database listing when forListing is set
    QUrl syncServerUrl(const RemoteParams* params, bool forListing = false)
    {
        auto base = params->serverUrl.trimmed();
        while (base.endsWith('/')) {
            base.chop(1);
        }
        if (forListing) {
            return QUrl(base + "/api/v1/databases");
        }
        return QUrl(base + "/api/v1/databases/" + QUrl::toPercentEncoding(params->serverDatabaseId.trimmed()));
    }

    /*
     * Perform one request against the sync server and block until it completes.
     * Called from an AsyncTask worker thread, so the network manager and event loop live there.
     */
    HttpResponse syncServerRequest(const RemoteParams* params,
                                   const QByteArray& verb,
                                   const QList<QPair<QByteArray, QByteArray>>& headers,
                                   const QByteArray& body,
                                   int timeoutMsec,
                                   bool forListing = false)
    {
        HttpResponse response;

        auto url = syncServerUrl(params, forListing);
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
            response.networkError = RemoteHandler::tr("Invalid sync server URL: %1").arg(params->serverUrl);
            return response;
        }

        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + params->serverToken.trimmed().toUtf8());
        request.setRawHeader("User-Agent", "KeePassXC");
        // Never follow redirects, they could forward the access token to another host
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(timeoutMsec);
        for (const auto& header : headers) {
            request.setRawHeader(header.first, header.second);
        }

        QNetworkAccessManager nam;
        QEventLoop loop;
        QScopedPointer<QNetworkReply> reply(nam.sendCustomRequest(request, verb, body));
        QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        response.etag = reply->rawHeader("ETag");
        if (response.status == 0) {
            response.networkError = reply->errorString();
        }
        return response;
    }

    QString syncServerError(const HttpResponse& response)
    {
        if (response.status == 0) {
            return RemoteHandler::tr("Could not reach the sync server: %1").arg(response.networkError);
        }
        if (response.status == 401 || response.status == 403) {
            return RemoteHandler::tr("The sync server rejected the access token.");
        }
        if (response.status >= 300 && response.status < 400) {
            return RemoteHandler::tr("The sync server responded with a redirect, check the server URL.");
        }
        auto message = QJsonDocument::fromJson(response.body).object().value("error").toString();
        if (message.isEmpty()) {
            message = RemoteHandler::tr("Unexpected response.");
        }
        return RemoteHandler::tr("The sync server responded with HTTP %1: %2").arg(response.status).arg(message);
    }
#endif
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

        if (params->type == RemoteParams::Type::SyncServer) {
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

        if (params->type == RemoteParams::Type::SyncServer) {
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
#ifdef KPXC_FEATURE_NETWORK
    auto response = syncServerRequest(params, "GET", {}, {}, params->downloadTimeoutMsec);
    if (response.status == 404) {
        // Nothing uploaded yet, the first sync publishes the local database
        result.success = true;
        result.remoteMissing = true;
        return result;
    }
    if (response.status != 200) {
        result.errorMessage = syncServerError(response);
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
    result.etag = QString::fromLatin1(response.etag);
#else
    Q_UNUSED(params)
    result.errorMessage = tr("This build of KeePassXC was compiled without network support.");
#endif
    return result;
}

RemoteHandler::RemoteResult
RemoteHandler::uploadToSyncServer(const QString& filePath, const RemoteParams* params, const QString& etag)
{
    RemoteResult result;
    result.success = false;
#ifdef KPXC_FEATURE_NETWORK
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errorMessage = tr("Failed to read database %1: %2").arg(filePath, file.errorString());
        return result;
    }
    auto data = file.readAll();
    file.close();

    // The server's ETag is the SHA-256 of its copy, so an unchanged merge needs no upload
    auto localEtag = "\"" + QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() + "\"";
    if (!etag.isEmpty() && etag.toLatin1() == localEtag) {
        result.success = true;
        result.etag = etag;
        return result;
    }

    QList<QPair<QByteArray, QByteArray>> headers{{"Content-Type", "application/octet-stream"}};
    // Only overwrite the exact version we merged with, or only create if we saw none
    if (etag.isEmpty()) {
        headers.append(qMakePair(QByteArray("If-None-Match"), QByteArray("*")));
    } else {
        headers.append(qMakePair(QByteArray("If-Match"), etag.toLatin1()));
    }

    auto response = syncServerRequest(params, "PUT", headers, data, params->uploadTimeoutMsec);
    if (response.status == 412) {
        result.errorMessage = tr("The database on the sync server was changed by another device during the sync. "
                                 "Your local changes are saved, sync again to merge them.");
        return result;
    }
    if (response.status != 200 && response.status != 201) {
        result.errorMessage = tr("Failed to upload merged database. %1").arg(syncServerError(response));
        return result;
    }

    result.success = true;
    result.filePath = filePath;
    result.etag = QString::fromLatin1(response.etag);
#else
    Q_UNUSED(filePath)
    Q_UNUSED(params)
    Q_UNUSED(etag)
    result.errorMessage = tr("This build of KeePassXC was compiled without network support.");
#endif
    return result;
}

RemoteHandler::SyncServerListing RemoteHandler::listSyncServerDatabases(const RemoteParams* params)
{
    return AsyncTask::runAndWaitForFuture([params] {
        SyncServerListing listing;
#ifdef KPXC_FEATURE_NETWORK
        auto response = syncServerRequest(params, "GET", {}, {}, params->downloadTimeoutMsec, true);
        if (response.status != 200) {
            listing.errorMessage = syncServerError(response);
            return listing;
        }
        for (const auto& database : QJsonDocument::fromJson(response.body).object().value("databases").toArray()) {
            listing.databaseIds << database.toObject().value("id").toString();
        }
        listing.success = true;
#else
        Q_UNUSED(params)
        listing.errorMessage = tr("This build of KeePassXC was compiled without network support.");
#endif
        return listing;
    });
}
