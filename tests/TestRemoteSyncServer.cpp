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
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Merger.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncEngine.h"
#include "gui/remote/SyncEventStream.h"
#include "gui/remote/SyncTokenStore.h"
#include "keys/PasswordKey.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

QTEST_GUILESS_MAIN(TestRemoteSyncServer)

namespace
{
    /*
     * The sync server tests run against a live server from sync_server/, e.g.:
     *   ./build/keepassxc-sync-server users add test && ./build/keepassxc-sync-server
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
    // Never touch the real Keychain or token file
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
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
    server->autoSync = true;
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
    QVERIFY(loadedServer->autoSync);
    QCOMPARE(loadedServer->downloadTimeoutMsec, 30000);
    QVERIFY(loadedServer->hasDownload());
    QVERIFY(loadedServer->hasUpload());

    auto* loadedCommand = loaded.getRemoteParams("command");
    QVERIFY(loadedCommand);
    QCOMPARE(loadedCommand->type, RemoteParams::Type::Command);
    QVERIFY(loadedCommand->serverToken.isEmpty());
    QVERIFY(!loadedCommand->autoSync);
    QVERIFY(loadedCommand->hasDownload());
    QVERIFY(!loadedCommand->hasUpload());
}

namespace
{
    RemoteParams* tokenParams(const QString& token)
    {
        auto* params = new RemoteParams();
        params->name = "server";
        params->type = RemoteParams::Type::SyncServer;
        params->serverUrl = "https://sync.example.com";
        params->serverDatabaseId = "personal";
        params->serverToken = token;
        return params;
    }

    QString storedSettings(const QSharedPointer<Database>& db)
    {
        return db->metadata()->customData()->value(CustomData::RemoteProgramSettings);
    }

    // A system store that can be switched off and counts its reads
    class FakeSystemStore : public SyncTokenStore::Backend
    {
    public:
        bool isAvailable() override
        {
            return available;
        }
        QString load(const QString& key) override
        {
            ++loads;
            return tokens.value(key);
        }
        bool save(const QString& key, const QString& token) override
        {
            tokens.insert(key, token);
            return true;
        }
        bool remove(const QString& key) override
        {
            tokens.remove(key);
            return true;
        }
        QString description() const override
        {
            return "fake";
        }

        bool available = true;
        int loads = 0;
        QHash<QString, QString> tokens;
    };
} // namespace

void TestRemoteSyncServer::testTokenStaysOnThisDevice()
{
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    auto db = QSharedPointer<Database>::create();
    RemoteSettings settings(db);
    settings.addRemoteParams(tokenParams("device-token"));
    settings.saveSettings();

    // The database, which every device shares, doesn't carry the token
    QVERIFY(storedSettings(db).contains("sync.example.com"));
    QVERIFY(!storedSettings(db).contains("device-token"));
    QCOMPARE(RemoteSettings(db).getRemoteParams("server")->serverToken, QString("device-token"));

    // Another device opening the same database has no token until it gets its own
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    QVERIFY(RemoteSettings(db).getRemoteParams("server")->serverToken.isEmpty());

    // Tokens belong to one database: another one syncing the same server database needs its own entry
    auto other = QSharedPointer<Database>::create();
    QVERIFY(SyncTokenStore::keyFor(db->rootGroup()->uuid(), *tokenParams({}))
            != SyncTokenStore::keyFor(other->rootGroup()->uuid(), *tokenParams({})));
}

void TestRemoteSyncServer::testMovesTokenOutOfOlderDatabases()
{
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    auto db = QSharedPointer<Database>::create();
    // Settings as an older version saved them, with the token inside
    db->metadata()->customData()->set(CustomData::RemoteProgramSettings,
                                      R"([{"name":"server","type":"syncServer","serverUrl":"https://sync.example.com",)"
                                      R"("serverDatabaseId":"personal","serverToken":"shared-token"}])");

    RemoteSettings settings(db);
    QCOMPARE(settings.getRemoteParams("server")->serverToken, QString("shared-token"));
    const auto key = SyncTokenStore::keyFor(db->rootGroup()->uuid(), *settings.getRemoteParams("server"));
    QCOMPARE(SyncTokenStore::load(key), QString("shared-token"));

    // Saving the settings takes it out of the database; this device keeps it
    settings.saveSettings();
    QVERIFY(!storedSettings(db).contains("shared-token"));
    QCOMPARE(RemoteSettings(db).getRemoteParams("server")->serverToken, QString("shared-token"));

    // A token this device already has wins over one still inside the database
    SyncTokenStore::save(key, "own-token");
    db->metadata()->customData()->set(CustomData::RemoteProgramSettings,
                                      R"([{"name":"server","type":"syncServer","serverUrl":"https://sync.example.com",)"
                                      R"("serverDatabaseId":"personal","serverToken":"shared-token"}])");
    QCOMPARE(RemoteSettings(db).getRemoteParams("server")->serverToken, QString("own-token"));
}

void TestRemoteSyncServer::testForgetsTokensOfRemovedRemotes()
{
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    auto db = QSharedPointer<Database>::create();
    RemoteSettings settings(db);
    settings.addRemoteParams(tokenParams("device-token"));
    settings.saveSettings();
    const auto key = SyncTokenStore::keyFor(db->rootGroup()->uuid(), *tokenParams({}));
    QCOMPARE(SyncTokenStore::load(key), QString("device-token"));

    RemoteSettings editor(db);
    editor.removeRemoteParams("server");
    editor.saveSettings();
    QVERIFY(SyncTokenStore::load(key).isEmpty());
}

void TestRemoteSyncServer::testTokenFileWithoutSystemStore()
{
    QTemporaryDir dir;
    // The settings folder doesn't exist yet on a fresh install
    const auto path = dir.filePath("config/sync-tokens.json");
    auto system = std::make_unique<FakeSystemStore>();
    system->available = false;
    auto* fake = system.get();
    SyncTokenStore::setBackend(
        SyncTokenStore::createFallbackBackend(std::move(system), SyncTokenStore::createFileBackend(path)));
    auto restore = qScopeGuard([] { SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend()); });

    QVERIFY(SyncTokenStore::save("key", "file-token"));
    QVERIFY(fake->tokens.isEmpty());
    QVERIFY(QFile::exists(path));
    QCOMPARE(QFile::permissions(path) & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    QCOMPARE(SyncTokenStore::description(), SyncTokenStore::createFileBackend(path)->description());

    // Removing the last token leaves no file behind
    QVERIFY(SyncTokenStore::remove("key"));
    QVERIFY(!QFile::exists(path));
}

void TestRemoteSyncServer::testTokenMovesFromFileToSystemStore()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("sync-tokens.json");
    // Kept while the keyring couldn't be reached
    QVERIFY(SyncTokenStore::createFileBackend(path)->save("key", "file-token"));
    QVERIFY(SyncTokenStore::createFileBackend(path)->save("other", "other-token"));

    auto system = std::make_unique<FakeSystemStore>();
    auto* fake = system.get();
    SyncTokenStore::setBackend(
        SyncTokenStore::createFallbackBackend(std::move(system), SyncTokenStore::createFileBackend(path)));
    auto restore = qScopeGuard([] { SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend()); });
    QCOMPARE(SyncTokenStore::description(), QString("fake"));

    QCOMPARE(SyncTokenStore::load("key"), QString("file-token"));
    QCOMPARE(fake->tokens.value("key"), QString("file-token"));
    QVERIFY(SyncTokenStore::createFileBackend(path)->load("key").isEmpty());
    QCOMPARE(SyncTokenStore::createFileBackend(path)->load("other"), QString("other-token"));

    // A token saved now goes to the system store and takes the file's copy with it
    QVERIFY(SyncTokenStore::save("other", "new-token"));
    QCOMPARE(fake->tokens.value("other"), QString("new-token"));
    QVERIFY(!QFile::exists(path));
}

void TestRemoteSyncServer::testTokensAreReadOnce()
{
    auto system = std::make_unique<FakeSystemStore>();
    auto* fake = system.get();
    fake->tokens.insert("key", "token");
    SyncTokenStore::setBackend(std::move(system));
    auto restore = qScopeGuard([] { SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend()); });

    // The remote settings are read again after every change to the database
    QCOMPARE(SyncTokenStore::load("key"), QString("token"));
    QCOMPARE(SyncTokenStore::load("key"), QString("token"));
    QCOMPARE(fake->loads, 1);

    // A token that isn't there yet is looked up again, since it may be added later
    QVERIFY(SyncTokenStore::load("missing").isEmpty());
    fake->tokens.insert("missing", "added");
    QCOMPARE(SyncTokenStore::load("missing"), QString("added"));

    QVERIFY(SyncTokenStore::save("key", "changed"));
    QCOMPARE(SyncTokenStore::load("key"), QString("changed"));
    QVERIFY(SyncTokenStore::remove("key"));
    QVERIFY(SyncTokenStore::load("key").isEmpty());
    QVERIFY(!fake->tokens.contains("key"));
}

void TestRemoteSyncServer::testPlatformTokenStore()
{
    // Writes to the real Keychain, Credential Manager, keyring or token file, so only on request
    if (qEnvironmentVariable("KPXC_TEST_KEYCHAIN") != "1") {
        QSKIP("Set KPXC_TEST_KEYCHAIN=1 to test the platform token store");
    }
    // A fresh platform store each time, so that reads reach it instead of the cache
    auto reopen = [] { SyncTokenStore::setBackend(nullptr); };
    reopen();
    auto restore = qScopeGuard([] { SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend()); });
    qInfo("Platform token store: %s", qPrintable(SyncTokenStore::description()));

    const auto key = "keepassxc-test-" + QUuid::createUuid().toString(QUuid::Id128);
    QVERIFY(SyncTokenStore::load(key).isEmpty());
    QVERIFY(SyncTokenStore::save(key, "first-token"));
    reopen();
    QCOMPARE(SyncTokenStore::load(key), QString("first-token"));
    QVERIFY(SyncTokenStore::save(key, "second-tökén"));
    reopen();
    QCOMPARE(SyncTokenStore::load(key), QString("second-tökén"));
    QVERIFY(SyncTokenStore::remove(key));
    reopen();
    QVERIFY(SyncTokenStore::load(key).isEmpty());
    QVERIFY(SyncTokenStore::remove(key));
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

void TestRemoteSyncServer::testSyncEngineTwoDevices()
{
    SKIP_WITHOUT_SERVER();

    const auto params = serverParams();
    QTemporaryDir dir;
    const SyncStateStore stateStore(dir.filePath("state.json"));

    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("a"));

    // The first device creates the database, the second starts from a copy of it
    auto first = QSharedPointer<Database>::create();
    auto kdf = QSharedPointer<AesKdf>::create();
    kdf->setRounds(1);
    first->setKdf(kdf);
    first->setKey(key);
    QVERIFY(first->saveAs(dir.filePath("first.kdbx")));
    QVERIFY(QFile::copy(dir.filePath("first.kdbx"), dir.filePath("second.kdbx")));
    auto second = QSharedPointer<Database>::create();
    QVERIFY(second->open(dir.filePath("second.kdbx"), key));

    const auto makeEngine = [&](const QSharedPointer<Database>& db) {
        auto* engine = new SyncEngine(db, new SyncServerClient(params), stateStore);
        engine->setMergeHandler([db](const QSharedPointer<Database>& remoteDb, const Database* base, QString* error) {
            if (SyncEngine::mergeDatabases(db.data(), remoteDb.data(), base).isEmpty()) {
                return true;
            }
            return db->save(Database::Atomic, {}, error) && remoteDb->save(Database::Atomic, {}, error);
        });
        engine->setSaveHandler([db](QString* error) { return db->save(Database::Atomic, {}, error); });
        return QSharedPointer<SyncEngine>(engine);
    };
    const auto runSync = [](SyncEngine* engine) {
        QSignalSpy finished(engine, &SyncEngine::finished);
        engine->sync();
        if (!finished.wait(10000)) {
            return QString("timed out");
        }
        return finished.first().at(0).toBool() ? QString() : finished.first().at(1).toString();
    };
    const auto addEntry = [](Database* db, const QString& title) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setGroup(db->rootGroup());
    };
    const auto titles = [](Database* db) {
        QStringList result;
        for (const auto* entry : db->rootGroup()->entriesRecursive()) {
            result << entry->title();
        }
        result.sort();
        return result;
    };

    auto firstEngine = makeEngine(first);
    auto secondEngine = makeEngine(second);

    addEntry(first.data(), "From first");
    QCOMPARE(runSync(firstEngine.data()), QString());
    addEntry(second.data(), "From second");
    QCOMPARE(runSync(secondEngine.data()), QString());
    QCOMPARE(runSync(firstEngine.data()), QString());

    QCOMPARE(titles(first.data()), QStringList({"From first", "From second"}));
    QCOMPARE(titles(second.data()), QStringList({"From first", "From second"}));

    // Both devices now agree with the server, so another sync is only a check
    const auto etag = stateStore.load(first->filePath(), params).etag;
    QCOMPARE(runSync(firstEngine.data()), QString());
    QCOMPARE(stateStore.load(first->filePath(), params).etag, etag);
}

void TestRemoteSyncServer::testHistoryAndEvents()
{
    SKIP_WITHOUT_SERVER();

    auto params = serverParams();
    const QString firstDb = QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase.kdbx");
    const QString secondDb = QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase2.kdbx");

    SyncEventStream events(params);
    QSignalSpy connected(&events, &SyncEventStream::connectedChanged);
    QSignalSpy changed(&events, &SyncEventStream::databaseChanged);
    events.start();
    QVERIFY(connected.wait(5000));
    QVERIFY(events.isConnected());

    RemoteHandler handler;
    auto result = handler.upload(firstDb, &params, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    result = handler.upload(secondDb, &params, result.etag);
    QVERIFY2(result.success, qPrintable(result.errorMessage));

    // Both uploads are announced, the last with the version it created
    QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 2, 5000);
    auto last = changed.last().first().value<SyncEventStream::Event>();
    QCOMPARE(last.databaseId, params.serverDatabaseId);
    QCOMPARE(last.etag, result.etag);
    QCOMPARE(last.revision, qint64(2));

    // The server keeps the previous version
    SyncServerClient client(params);
    SyncServerClient::Response history;
    client.history([&](const SyncServerClient::Response& response) { history = response; });
    QTRY_VERIFY_WITH_TIMEOUT(history.status != 0, 5000);
    QCOMPARE(history.status, 200);
    const auto versions = QJsonDocument::fromJson(history.body).object().value("versions").toArray();
    QCOMPARE(versions.size(), 2);
    QCOMPARE(versions[1].toObject().value("revision").toInt(), 1);

    SyncServerClient::Response first;
    client.downloadRevision(1, [&](const SyncServerClient::Response& response) { first = response; });
    QTRY_VERIFY_WITH_TIMEOUT(first.status != 0, 5000);
    QCOMPARE(first.status, 200);
    QCOMPARE(first.body, readFile(firstDb));

    events.stop();
    QVERIFY(!events.isConnected());
}
