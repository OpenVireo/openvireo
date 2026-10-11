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

#include "TestRemoteSyncServer.h"

#include "config-keepassx-tests.h"

#include "core/Database.h"
#include "crypto/Crypto.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"

#include <QFile>
#include <QTest>
#include <QUuid>

QTEST_GUILESS_MAIN(TestRemoteSyncServer)

namespace
{
    /*
     * The sync server tests run against a live server from sync_server/, e.g.:
     *   npm run add-user -- test && npm start
     *   KPXC_SYNC_SERVER_URL=http://127.0.0.1:8787 KPXC_SYNC_SERVER_TOKEN=<token> ./testremotesyncserver
     */
    RemoteParams serverParams()
    {
        RemoteParams params;
        params.name = "server";
        params.type = RemoteParams::Type::SyncServer;
        params.serverUrl = qEnvironmentVariable("KPXC_SYNC_SERVER_URL");
        params.serverToken = qEnvironmentVariable("KPXC_SYNC_SERVER_TOKEN");
        params.serverDatabaseId = "test-" + QUuid::createUuid().toString(QUuid::Id128);
        return params;
    }

    QByteArray readFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }
} // namespace

#define SKIP_WITHOUT_SERVER()                                                                                          \
    if (qEnvironmentVariableIsEmpty("KPXC_SYNC_SERVER_URL")                                                            \
        || qEnvironmentVariableIsEmpty("KPXC_SYNC_SERVER_TOKEN")) {                                                    \
        QSKIP("Set KPXC_SYNC_SERVER_URL and KPXC_SYNC_SERVER_TOKEN to run sync server tests");                         \
    }

void TestRemoteSyncServer::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestRemoteSyncServer::testSettingsRoundTrip()
{
    auto db = QSharedPointer<Database>::create();

    RemoteSettings settings(db);
    auto* server = new RemoteParams();
    server->name = "server";
    server->type = RemoteParams::Type::SyncServer;
    server->serverUrl = "https://sync.example.com";
    server->serverDatabaseId = "personal";
    server->serverToken = "secret-token";
    server->downloadTimeoutMsec = 30000;
    server->uploadTimeoutMsec = 30000;
    settings.addRemoteParams(server);

    auto* command = new RemoteParams();
    command->name = "command";
    command->downloadCommand = "scp host:db.kdbx {TEMP_DATABASE}";
    settings.addRemoteParams(command);
    settings.saveSettings();

    RemoteSettings loaded(db);
    auto* loadedServer = loaded.getRemoteParams("server");
    QVERIFY(loadedServer);
    QCOMPARE(loadedServer->type, RemoteParams::Type::SyncServer);
    QCOMPARE(loadedServer->serverUrl, QString("https://sync.example.com"));
    QCOMPARE(loadedServer->serverDatabaseId, QString("personal"));
    QCOMPARE(loadedServer->serverToken, QString("secret-token"));
    QCOMPARE(loadedServer->downloadTimeoutMsec, 30000);
    QVERIFY(loadedServer->hasDownload());
    QVERIFY(loadedServer->hasUpload());

    auto* loadedCommand = loaded.getRemoteParams("command");
    QVERIFY(loadedCommand);
    QCOMPARE(loadedCommand->type, RemoteParams::Type::Command);
    QVERIFY(loadedCommand->serverToken.isEmpty());
    QVERIFY(loadedCommand->hasDownload());
    QVERIFY(!loadedCommand->hasUpload());
}

void TestRemoteSyncServer::testSyncServerCycle()
{
    SKIP_WITHOUT_SERVER();

    auto params = serverParams();
    RemoteHandler handler;
    const QString firstDb = QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase.kdbx");
    const QString secondDb = QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase2.kdbx");

    // Nothing on the server yet
    auto result = handler.download(&params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.remoteMissing);

    // First upload creates it
    result = handler.upload(firstDb, &params, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    const auto firstEtag = result.etag;
    QVERIFY(!firstEtag.isEmpty());

    // It shows up in the listing used by the import wizard
    auto listing = handler.listSyncServerDatabases(&params);
    QVERIFY2(listing.success, qPrintable(listing.errorMessage));
    QVERIFY(listing.databaseIds.contains(params.serverDatabaseId));

    // A second "first upload" from another device must not overwrite it
    result = handler.upload(secondDb, &params, {});
    QVERIFY(!result.success);

    // Download returns the exact bytes and version
    result = handler.download(&params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(!result.remoteMissing);
    QCOMPARE(result.etag, firstEtag);
    QCOMPARE(readFile(result.filePath), readFile(firstDb));
    QFile::remove(result.filePath);

    // Uploading unchanged content is skipped and keeps the version
    result = handler.upload(firstDb, &params, firstEtag);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.etag, firstEtag);

    // Uploading new content against the current version succeeds
    result = handler.upload(secondDb, &params, firstEtag);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.etag != firstEtag);

    // A device that merged new changes onto the old version is told to sync again
    const QString otherDb = QString(KEEPASSX_TEST_DATA_DIR).append("/SyncDatabase.kdbx");
    result = handler.upload(otherDb, &params, firstEtag);
    QVERIFY(!result.success);
    QVERIFY2(result.errorMessage.contains("another device"), qPrintable(result.errorMessage));
}

void TestRemoteSyncServer::testSyncServerRejectsBadToken()
{
    SKIP_WITHOUT_SERVER();

    auto params = serverParams();
    params.serverToken = "not-a-valid-token";
    RemoteHandler handler;

    auto result = handler.download(&params);
    QVERIFY(!result.success);
    QVERIFY2(result.errorMessage.contains("access token"), qPrintable(result.errorMessage));

    auto listing = handler.listSyncServerDatabases(&params);
    QVERIFY(!listing.success);
    QVERIFY2(listing.errorMessage.contains("access token"), qPrintable(listing.errorMessage));
}
