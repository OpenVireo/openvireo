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

#include "TestSyncEngine.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Merger.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/remote/SyncEngine.h"
#include "keys/PasswordKey.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

QTEST_GUILESS_MAIN(TestSyncEngine)

namespace
{
    /*
     * An in-memory sync server with the same concurrency rules as sync_server/.
     * beforePut runs before each upload is judged, to let "another device" upload first.
     */
    class FakeSyncServer : public SyncServerClient
    {
    public:
        explicit FakeSyncServer(const RemoteParams& params)
            : SyncServerClient(params)
        {
        }

        void store(const QByteArray& data)
        {
            stored = data;
            etag = etagFor(data);
            ++revision;
        }

        QByteArray stored;
        QString etag;
        qint64 revision = 0;
        QStringList requests;
        int networkErrors = 0;
        std::function<void()> beforePut;

    protected:
        void send(const QByteArray& verb,
                  const QUrl&,
                  const Headers& headers,
                  const QByteArray& body,
                  int,
                  Callback callback) override
        {
            requests << QString::fromLatin1(verb);
            Response response;
            if (networkErrors > 0) {
                --networkErrors;
                response.networkError = "Host unreachable";
                deliverLater(response, std::move(callback));
                return;
            }

            const auto header = [&headers](const QByteArray& name) {
                for (const auto& pair : headers) {
                    if (pair.first == name) {
                        return QString::fromLatin1(pair.second);
                    }
                }
                return QString();
            };

            if (verb == "PUT") {
                if (beforePut) {
                    beforePut();
                }
                const auto ifMatch = header("If-Match");
                const auto ifNoneMatch = header("If-None-Match");
                if ((ifNoneMatch == "*" && !stored.isEmpty()) || (!ifMatch.isEmpty() && ifMatch != etag)) {
                    response.status = 412;
                } else {
                    response.status = stored.isEmpty() ? 201 : 200;
                    store(body);
                }
            } else if (stored.isEmpty()) {
                response.status = 404;
            } else if (verb == "HEAD" && header("If-None-Match") == etag) {
                response.status = 304;
            } else {
                response.status = 200;
                if (verb == "GET") {
                    response.body = stored;
                }
            }
            if (!stored.isEmpty()) {
                response.etag = etag;
                response.revision = revision;
            }
            deliverLater(response, std::move(callback));
        }
    };

    RemoteParams serverParams()
    {
        RemoteParams params;
        params.name = "server";
        params.type = RemoteParams::Type::SyncServer;
        params.serverUrl = "https://sync.example.com";
        params.serverDatabaseId = "personal";
        params.serverToken = "token";
        return params;
    }

    QSharedPointer<CompositeKey> makeKey(const QString& password)
    {
        auto key = QSharedPointer<CompositeKey>::create();
        key->addKey(QSharedPointer<PasswordKey>::create(password));
        return key;
    }

    QSharedPointer<Database> createDatabase(const QString& filePath, const QString& password = "a")
    {
        auto db = QSharedPointer<Database>::create();
        auto kdf = QSharedPointer<AesKdf>::create();
        kdf->setRounds(1);
        db->setKdf(kdf);
        db->setKey(makeKey(password));
        QString error;
        if (!db->saveAs(filePath, Database::Atomic, {}, &error)) {
            qWarning("Failed to save %s: %s", qPrintable(filePath), qPrintable(error));
            return {};
        }
        return db;
    }

    void addEntry(Database* db, const QString& title)
    {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setGroup(db->rootGroup());
    }

    QStringList titles(const Database* db)
    {
        QStringList result;
        for (const auto* entry : db->rootGroup()->entriesRecursive()) {
            result << entry->title();
        }
        result.sort();
        return result;
    }

    QByteArray readFile(const QString& filePath)
    {
        QFile file(filePath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // Open the server's copy as another device would
    QSharedPointer<Database>
    openServerCopy(const FakeSyncServer* server, const QString& filePath, const QString& password = "a")
    {
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(server->stored) != server->stored.size()) {
            return {};
        }
        file.close();
        auto db = QSharedPointer<Database>::create();
        if (!db->open(filePath, makeKey(password))) {
            return {};
        }
        return db;
    }

    // Another device adds an entry and uploads its copy
    bool uploadFromOtherDevice(FakeSyncServer* server, const QString& filePath, const QString& title)
    {
        auto db = openServerCopy(server, filePath);
        if (!db) {
            return false;
        }
        addEntry(db.data(), title);
        if (!db->save(Database::Atomic)) {
            return false;
        }
        server->store(readFile(filePath));
        return true;
    }

    // Same two-way merge as DatabaseWidget::syncWithDatabase
    bool mergeBothWays(Database* local, const QSharedPointer<Database>& remote, QString* error)
    {
        Merger firstMerge(local, remote.data());
        Merger secondMerge(remote.data(), local);
        auto changes = firstMerge.merge() + secondMerge.merge();
        if (changes.isEmpty()) {
            return true;
        }
        return local->save(Database::Atomic, {}, error) && remote->save(Database::Atomic, {}, error);
    }

    struct Fixture
    {
        Fixture()
        {
            local = createDatabase(dir.filePath("local.kdbx"));
            server = new FakeSyncServer(serverParams());
            engine.reset(new SyncEngine(local, server, SyncStateStore(dir.filePath("state.json"))));
            engine->setMergeHandler([this](const QSharedPointer<Database>& remote, QString* error) {
                return mergeBothWays(local.data(), remote, error);
            });
            engine->setSaveHandler([this](QString* error) { return local->save(Database::Atomic, {}, error); });
        }

        // Run one sync to the end; returns the finished() arguments
        QList<QVariant> sync()
        {
            QSignalSpy finished(engine.data(), &SyncEngine::finished);
            engine->sync();
            if (finished.isEmpty() && !finished.wait(5000)) {
                return {};
            }
            return finished.takeFirst();
        }

        QTemporaryDir dir;
        QSharedPointer<Database> local;
        FakeSyncServer* server;
        QScopedPointer<SyncEngine> engine;
    };
} // namespace

void TestSyncEngine::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestSyncEngine::testStateStoreRoundTrip()
{
    QTemporaryDir dir;
    SyncStateStore store(dir.filePath("state.json"));
    auto params = serverParams();
    const auto dbPath = dir.filePath("a.kdbx");

    QVERIFY(store.load(dbPath, params).isEmpty());

    SyncState state;
    state.etag = "\"abc\"";
    state.revision = 7;
    state.lastSync = QDateTime::currentDateTimeUtc();
    QVERIFY(store.save(dbPath, params, state));

    auto loaded = store.load(dbPath, params);
    QCOMPARE(loaded.etag, state.etag);
    QCOMPARE(loaded.revision, qint64(7));
    QCOMPARE(loaded.lastSync, state.lastSync);

    // Another server database for the same file is tracked separately
    auto other = params;
    other.serverDatabaseId = "work";
    QVERIFY(store.load(dbPath, other).isEmpty());

    // The state file doesn't reveal which files or servers are synced
    auto contents = readFile(store.filePath());
    QVERIFY(!contents.contains("sync.example.com"));
    QVERIFY(!contents.contains("a.kdbx"));

    QVERIFY(store.remove(dbPath, params));
    QVERIFY(store.load(dbPath, params).isEmpty());
}

void TestSyncEngine::testFirstSyncUploadsLocalDatabase()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Local entry");

    auto result = f.sync();
    QVERIFY(!result.isEmpty());
    QVERIFY2(result.at(0).toBool(), qPrintable(result.at(1).toString()));
    QCOMPARE(f.server->requests, QStringList({"GET", "PUT"}));
    QVERIFY(!f.local->isModified());
    QCOMPARE(f.server->stored, readFile(f.local->filePath()));

    auto state = SyncStateStore(f.dir.filePath("state.json")).load(f.local->filePath(), serverParams());
    QCOMPARE(state.etag, f.server->etag);
    QCOMPARE(state.revision, qint64(1));
}

void TestSyncEngine::testMergesChangesFromBothSides()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());

    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));
    addEntry(f.local.data(), "Local entry");

    auto result = f.sync();
    QVERIFY2(result.value(0).toBool(), qPrintable(result.value(1).toString()));
    QCOMPARE(titles(f.local.data()), QStringList({"Local entry", "Remote entry"}));

    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(titles(serverCopy.data()), QStringList({"Local entry", "Remote entry"}));
    QCOMPARE(f.server->revision, qint64(3));
}

void TestSyncEngine::testUnchangedSyncOnlyChecks()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    f.server->requests.clear();

    auto result = f.sync();
    QVERIFY2(result.value(0).toBool(), qPrintable(result.value(1).toString()));
    QCOMPARE(f.server->requests, QStringList({"HEAD"}));
    QCOMPARE(f.server->revision, qint64(1));
}

void TestSyncEngine::testDownloadsWhenOnlyServerChanged()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));
    f.server->requests.clear();

    auto result = f.sync();
    QVERIFY2(result.value(0).toBool(), qPrintable(result.value(1).toString()));
    QCOMPARE(f.server->requests.mid(0, 2), QStringList({"HEAD", "GET"}));
    QCOMPARE(titles(f.local.data()), QStringList({"Remote entry"}));
}

void TestSyncEngine::testRetriesAfterConflict()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    addEntry(f.local.data(), "Local entry");

    // Another device uploads between our download and our upload, once
    bool raced = false;
    f.server->beforePut = [&] {
        if (!raced) {
            raced = true;
            QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Concurrent entry"));
        }
    };
    f.server->requests.clear();

    auto result = f.sync();
    QVERIFY2(result.value(0).toBool(), qPrintable(result.value(1).toString()));
    QCOMPARE(f.server->requests, QStringList({"GET", "PUT", "GET", "PUT"}));

    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(titles(serverCopy.data()), QStringList({"Concurrent entry", "Local entry"}));
    QCOMPARE(titles(f.local.data()), QStringList({"Concurrent entry", "Local entry"}));
}

void TestSyncEngine::testGivesUpAfterRepeatedConflicts()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    addEntry(f.local.data(), "Local entry");

    int uploads = 0;
    f.server->beforePut = [&] {
        QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), QString("Other %1").arg(++uploads)));
    };

    auto result = f.sync();
    QVERIFY(!result.value(0).toBool());
    QVERIFY2(result.value(1).toString().contains("another device"), qPrintable(result.value(1).toString()));
    QCOMPARE(uploads, SyncEngine::MaxAttempts);
    QCOMPARE(f.engine->state(), SyncEngine::State::Idle);
    // The local changes are kept
    QVERIFY(titles(f.local.data()).contains("Local entry"));
}

void TestSyncEngine::testAsksToUnlockWhenKeyDiffers()
{
    Fixture f;
    QVERIFY(f.local);
    auto otherDb = createDatabase(f.dir.filePath("other.kdbx"), "different");
    QVERIFY(otherDb);
    f.server->store(readFile(otherDb->filePath()));

    QSignalSpy unlockRequired(f.engine.data(), &SyncEngine::unlockRequired);
    QSignalSpy finished(f.engine.data(), &SyncEngine::finished);
    f.engine->sync();
    QVERIFY(unlockRequired.wait(5000));
    QCOMPARE(f.engine->state(), SyncEngine::State::WaitingForUnlock);
    QVERIFY(QFile::exists(unlockRequired.first().first().toString()));
    QVERIFY(finished.isEmpty());

    f.engine->resumeAfterUnlock(false);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().first().toBool());
    QCOMPARE(f.engine->state(), SyncEngine::State::Idle);
}

void TestSyncEngine::testBacksOffAfterNetworkError()
{
    Fixture f;
    QVERIFY(f.local);
    f.engine->setRetryOnNetworkError(true);
    f.server->networkErrors = 2;

    auto result = f.sync();
    QVERIFY(!result.value(0).toBool());
    QVERIFY2(result.value(1).toString().contains("Host unreachable"), qPrintable(result.value(1).toString()));
    QCOMPARE(f.engine->state(), SyncEngine::State::Backoff);
    QCOMPARE(f.engine->backoffMsec(), SyncEngine::FirstBackoffMsec);
    QVERIFY(!f.engine->isBusy());

    // Each failure doubles the delay
    QVERIFY(!f.sync().value(0).toBool());
    QCOMPARE(f.engine->backoffMsec(), SyncEngine::FirstBackoffMsec * 2);

    // Syncing again runs the retry now, and success resets the delay
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(f.engine->state(), SyncEngine::State::Idle);
    QCOMPARE(f.engine->backoffMsec(), 0);
}

void TestSyncEngine::testCancelStopsTheSync()
{
    Fixture f;
    QVERIFY(f.local);

    QSignalSpy finished(f.engine.data(), &SyncEngine::finished);
    f.engine->sync();
    QVERIFY(f.engine->isBusy());
    f.engine->cancel();
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().first().toBool());
    QCOMPARE(f.engine->state(), SyncEngine::State::Idle);

    // The answer to the cancelled request is dropped
    QTest::qWait(50);
    QCOMPARE(finished.count(), 1);
    QVERIFY(f.server->stored.isEmpty());
}

void TestSyncEngine::testMergeWaitsForTheGate()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));

    bool editing = true;
    f.engine->setMergeGate([&editing] { return !editing; });
    QSignalSpy deferred(f.engine.data(), &SyncEngine::mergeDeferred);
    QSignalSpy finished(f.engine.data(), &SyncEngine::finished);

    f.engine->sync();
    QVERIFY(deferred.wait(5000));
    QCOMPARE(f.engine->state(), SyncEngine::State::WaitingToMerge);
    QVERIFY(f.engine->isBusy());
    QVERIFY(titles(f.local.data()).isEmpty());

    // Still editing: nothing happens
    f.engine->resumeMerge();
    QCOMPARE(f.engine->state(), SyncEngine::State::WaitingToMerge);

    editing = false;
    f.engine->resumeMerge();
    QVERIFY(finished.count() == 1 || finished.wait(5000));
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(titles(f.local.data()), QStringList({"Remote entry"}));
}
