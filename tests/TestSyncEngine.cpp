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
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/remote/SyncEngine.h"
#include "keys/PasswordKey.h"
#include "mock/MockClock.h"

#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <functional>

QTEST_GUILESS_MAIN(TestSyncEngine)

namespace
{
    /*
     * An in-memory sync server with the same concurrency rules as sync_server/.
     * beforePut runs before each upload is judged, to let "another device" upload first.
     */
    struct ServerData
    {
        QByteArray stored;
        QString etag;
        qint64 revision = 0;
    };

    class FakeSyncServer : public SyncServerClient
    {
    public:
        // Clients created with the same data talk to the same server
        explicit FakeSyncServer(const RemoteParams& params,
                                QSharedPointer<ServerData> serverData = QSharedPointer<ServerData>::create())
            : SyncServerClient(params)
            , data(std::move(serverData))
            , stored(data->stored)
            , etag(data->etag)
            , revision(data->revision)
        {
        }

        void store(const QByteArray& content)
        {
            stored = content;
            etag = etagFor(content);
            ++revision;
        }

        QSharedPointer<ServerData> data;
        QByteArray& stored;
        QString& etag;
        qint64& revision;
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

    // Same merge and saves as DatabaseWidget::syncWithDatabase
    bool mergeBothWays(Database* local, const QSharedPointer<Database>& remote, const Database* base, QString* error)
    {
        if (SyncEngine::mergeDatabases(local, remote.data(), base).isEmpty()) {
            return true;
        }
        return local->save(Database::Atomic, {}, error) && remote->save(Database::Atomic, {}, error);
    }

    Entry* findEntry(Database* db, const QString& title)
    {
        for (auto* entry : db->rootGroup()->entriesRecursive()) {
            if (entry->title() == title) {
                return entry;
            }
        }
        return nullptr;
    }

    // Edit an entry as of a given time, so tests don't depend on the clock
    void editEntry(Entry* entry, const std::function<void(Entry*)>& change, const QDateTime& when)
    {
        change(entry);
        auto timeInfo = entry->timeInfo();
        timeInfo.setLastModificationTime(when);
        entry->setTimeInfo(timeInfo);
    }

    void editEntry(Entry* entry, const QString& notes, const QDateTime& when)
    {
        editEntry(entry, [&notes](Entry* e) { e->setNotes(notes); }, when);
    }

    // Another device changes an entry of the server copy and uploads it
    bool editOnOtherDevice(FakeSyncServer* server,
                           const QString& filePath,
                           const QString& title,
                           const std::function<void(Entry*)>& change,
                           const QDateTime& when)
    {
        auto other = openServerCopy(server, filePath);
        if (!other || !findEntry(other.data(), title)) {
            return false;
        }
        editEntry(findEntry(other.data(), title), change, when);
        if (!other->save(Database::Atomic)) {
            return false;
        }
        server->store(readFile(filePath));
        return true;
    }

    bool historyHas(const Entry* entry, const std::function<bool(const Entry*)>& match)
    {
        for (const auto* item : entry->historyItems()) {
            if (match(item)) {
                return true;
            }
        }
        return false;
    }

    struct Fixture
    {
        // A second fixture can share the first one's server and act as another device
        explicit Fixture(const QString& name = "local", Fixture* sameServerAs = nullptr)
        {
            local = createDatabase(dir.filePath(name + ".kdbx"));
            if (sameServerAs) {
                // Start from the same database, as a device set up from the server would
                local.reset();
                QFile::copy(sameServerAs->local->filePath(), dir.filePath(name + ".kdbx"));
                local = QSharedPointer<Database>::create();
                local->open(dir.filePath(name + ".kdbx"), makeKey("a"));
            }
            server = sameServerAs ? new FakeSyncServer(serverParams(), sameServerAs->server->data)
                                  : new FakeSyncServer(serverParams());
            engine.reset(new SyncEngine(local, server, SyncStateStore(dir.filePath("state.json"))));
            engine->setMergeHandler(
                [this](const QSharedPointer<Database>& remote, const Database* base, QString* error) {
                    return mergeBothWays(local.data(), remote, base, error);
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
    state.generation = 4;
    const auto entryUuid = QUuid::createUuid();
    state.entries.insert(entryUuid, QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone::utc()));
    QVERIFY(store.save(dbPath, params, state));

    auto loaded = store.load(dbPath, params);
    QCOMPARE(loaded.etag, state.etag);
    QCOMPARE(loaded.revision, qint64(7));
    QCOMPARE(loaded.lastSync, state.lastSync);
    QCOMPARE(loaded.generation, qint64(4));
    QCOMPARE(loaded.entries, state.entries);

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

void TestSyncEngine::testGenerationRisesOnlyWithChanges()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(SyncEngine::generationOf(f.local.data()), qint64(0));

    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));
    addEntry(f.local.data(), "Local entry");
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(SyncEngine::generationOf(f.local.data()), qint64(1));
    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(SyncEngine::generationOf(serverCopy.data()), qint64(1));

    // Nothing changed: no new generation and nothing uploaded
    const auto revision = f.server->revision;
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(SyncEngine::generationOf(f.local.data()), qint64(1));
    QCOMPARE(f.server->revision, revision);
}

void TestSyncEngine::testTwoDevicesSettle()
{
    // Each step a minute apart, as on real devices, so newer custom data really is newer
    auto* clock = new MockClock();
    MockClock::setup(clock);
    auto teardown = qScopeGuard([] { MockClock::teardown(); });

    Fixture first("first");
    QVERIFY(first.local);
    QVERIFY(first.sync().value(0).toBool());
    Fixture second("second", &first);
    QVERIFY(second.local);

    clock->advanceMinute(1);
    addEntry(first.local.data(), "From first");
    QVERIFY(first.sync().value(0).toBool());
    clock->advanceMinute(1);
    addEntry(second.local.data(), "From second");
    QVERIFY(second.sync().value(0).toBool());
    clock->advanceMinute(1);
    QVERIFY(first.sync().value(0).toBool());
    QCOMPARE(titles(first.local.data()), QStringList({"From first", "From second"}));
    QCOMPARE(titles(second.local.data()), QStringList({"From first", "From second"}));

    // Once both have everything, syncing back and forth changes nothing on the server
    const auto revision = first.server->revision;
    for (int i = 0; i < 3; ++i) {
        clock->advanceMinute(1);
        QVERIFY(first.sync().value(0).toBool());
        clock->advanceMinute(1);
        QVERIFY(second.sync().value(0).toBool());
    }
    QCOMPARE(first.server->revision, revision);
}

void TestSyncEngine::testDetectsServerRevisionGoingBack()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    const auto oldCopy = f.server->stored;
    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));
    QVERIFY(f.sync().value(0).toBool());

    // The server is restored from a backup: the old file and an old revision number
    f.server->stored = oldCopy;
    f.server->etag = SyncServerClient::etagFor(oldCopy);
    f.server->revision = 1;
    f.server->requests.clear();

    auto result = f.sync();
    QVERIFY(!result.value(0).toBool());
    QVERIFY2(result.value(1).toString().contains("older version"), qPrintable(result.value(1).toString()));
    QCOMPARE(f.engine->state(), SyncEngine::State::NeedsAttention);
    QCOMPARE(f.engine->attention(), SyncEngine::Attention::ServerRolledBack);
    QVERIFY(!f.engine->isBusy());
    QVERIFY(!f.server->requests.contains("PUT"));
    QCOMPARE(f.server->stored, oldCopy);

    // Asking again without accepting stops again
    QVERIFY(!f.sync().value(0).toBool());
    QCOMPARE(f.engine->state(), SyncEngine::State::NeedsAttention);

    // The user accepts: the server gets everything back, and later syncs compare against the new state
    QSignalSpy finished(f.engine.data(), &SyncEngine::finished);
    f.engine->syncAcceptingServerState();
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(f.engine->attention(), SyncEngine::Attention::None);
    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(titles(serverCopy.data()), QStringList({"Remote entry"}));
    QVERIFY(f.sync().value(0).toBool());
}

void TestSyncEngine::testDetectsOlderCopyBehindANewRevision()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    const auto oldCopy = f.server->stored;
    QVERIFY(uploadFromOtherDevice(f.server, f.dir.filePath("other.kdbx"), "Remote entry"));
    addEntry(f.local.data(), "Local entry");
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(SyncEngine::generationOf(f.local.data()), qint64(1));

    // A server that lies: a higher revision number, but the content of an older version
    f.server->store(oldCopy);
    auto result = f.sync();
    QVERIFY(!result.value(0).toBool());
    QVERIFY2(result.value(1).toString().contains("sync generation"), qPrintable(result.value(1).toString()));
    QCOMPARE(f.engine->attention(), SyncEngine::Attention::ServerRolledBack);
    // Nothing was merged into the local database
    QCOMPARE(titles(f.local.data()), QStringList({"Local entry", "Remote entry"}));
    QVERIFY(!f.local->isModified());
}

void TestSyncEngine::testDetectsLostDatabase()
{
    Fixture f;
    QVERIFY(f.local);
    QVERIFY(f.sync().value(0).toBool());
    f.server->stored.clear();
    f.server->etag.clear();

    auto result = f.sync();
    QVERIFY(!result.value(0).toBool());
    QCOMPARE(f.engine->attention(), SyncEngine::Attention::ServerLostDatabase);
    QVERIFY(f.server->stored.isEmpty());

    QSignalSpy finished(f.engine.data(), &SyncEngine::finished);
    f.engine->syncAcceptingServerState();
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(f.server->stored, readFile(f.local->filePath()));
}

void TestSyncEngine::testReportsEntriesChangedOnBothSides()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    addEntry(f.local.data(), "Only here");
    QVERIFY(f.sync().value(0).toBool());

    const auto base = QDateTime::currentDateTimeUtc();
    // Another device edits "Shared" and uploads
    {
        auto other = openServerCopy(f.server, f.dir.filePath("other.kdbx"));
        QVERIFY(other);
        editEntry(findEntry(other.data(), "Shared"), "edited elsewhere", base.addSecs(20));
        QVERIFY(other->save(Database::Atomic));
        f.server->store(readFile(other->filePath()));
    }
    // This device edits "Shared" too, and "Only here" which nobody else touched
    editEntry(findEntry(f.local.data(), "Shared"), "edited here", base.addSecs(10));
    editEntry(findEntry(f.local.data(), "Only here"), "edited here", base.addSecs(10));
    const auto sharedUuid = findEntry(f.local.data(), "Shared")->uuid();

    QSignalSpy conflicts(f.engine.data(), &SyncEngine::conflictsDetected);
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(conflicts.count(), 1);
    const auto list = conflicts.first().first().value<QList<SyncConflict>>();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.first().uuid, sharedUuid);
    QCOMPARE(list.first().title, QString("Shared"));
    QCOMPARE(list.first().kind, SyncConflict::Kind::EditedOnBoth);
    // Both changed the notes, so that field is named
    QCOMPARE(list.first().fields, QStringList({"Notes"}));
    // The newer edit won, and the older one is in the entry's history
    auto* shared = findEntry(f.local.data(), "Shared");
    QCOMPARE(shared->notes(), QString("edited elsewhere"));
    bool inHistory = false;
    for (const auto* item : shared->historyItems()) {
        inHistory |= item->notes() == "edited here";
    }
    QVERIFY(inHistory);

    // The next sync starts from the merged state, so there is nothing to report
    conflicts.clear();
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(conflicts.count(), 0);
}

void TestSyncEngine::testFindsDeletedAndEditedEntries()
{
    QTemporaryDir dir;
    auto local = createDatabase(dir.filePath("local.kdbx"));
    QVERIFY(local);
    addEntry(local.data(), "Deleted here");
    addEntry(local.data(), "Deleted there");
    addEntry(local.data(), "Untouched");
    const auto base = QDateTime::currentDateTimeUtc().addSecs(-100);
    for (auto* entry : local->rootGroup()->entriesRecursive()) {
        editEntry(entry, QString(), base);
    }
    QVERIFY(local->save(Database::Atomic));
    const auto snapshot = SyncEngine::snapshotOf(local.data());
    QVERIFY(QFile::copy(local->filePath(), dir.filePath("remote.kdbx")));
    auto remote = QSharedPointer<Database>::create();
    QVERIFY(remote->open(dir.filePath("remote.kdbx"), makeKey("a")));

    const auto deletedHere = findEntry(local.data(), "Deleted here")->uuid();
    const auto deletedThere = findEntry(local.data(), "Deleted there")->uuid();
    delete findEntry(local.data(), "Deleted here");
    editEntry(findEntry(remote.data(), "Deleted here"), "edited there", base.addSecs(10));
    delete findEntry(remote.data(), "Deleted there");
    editEntry(findEntry(local.data(), "Deleted there"), "edited here", base.addSecs(10));

    auto conflicts = SyncEngine::findConflicts(local.data(), remote.data(), snapshot);
    QCOMPARE(conflicts.size(), 2);
    for (const auto& conflict : conflicts) {
        if (conflict.uuid == deletedHere) {
            QCOMPARE(conflict.kind, SyncConflict::Kind::DeletedHereEditedThere);
        } else {
            QCOMPARE(conflict.uuid, deletedThere);
            QCOMPARE(conflict.kind, SyncConflict::Kind::EditedHereDeletedThere);
        }
    }

    // Without a snapshot there is no common starting point, so nothing is reported
    QVERIFY(SyncEngine::findConflicts(local.data(), remote.data(), {}).isEmpty());
}

void TestSyncEngine::testMergerIgnoresTheGeneration()
{
    // If Merger copied the generation like other custom data, two devices would keep "changing" it for each
    // other and sync forever
    auto* clock = new MockClock();
    MockClock::setup(clock);
    auto teardown = qScopeGuard([] { MockClock::teardown(); });

    QTemporaryDir dir;
    auto first = createDatabase(dir.filePath("first.kdbx"));
    QVERIFY(first);
    QVERIFY(QFile::copy(first->filePath(), dir.filePath("second.kdbx")));
    auto second = QSharedPointer<Database>::create();
    QVERIFY(second->open(dir.filePath("second.kdbx"), makeKey("a")));

    // Different times, so Merger takes the newer side's custom data
    first->metadata()->customData()->set(CustomData::SyncGeneration, "5");
    clock->advanceMinute(1);
    second->metadata()->customData()->set(CustomData::SyncGeneration, "2");

    QVERIFY(SyncEngine::mergeDatabases(first.data(), second.data()).isEmpty());
    QCOMPARE(SyncEngine::generationOf(first.data()), qint64(5));
    QCOMPARE(SyncEngine::generationOf(second.data()), qint64(2));

    // Without the key on one side, the other side keeps it
    second->metadata()->customData()->remove(CustomData::SyncGeneration);
    Merger(second.data(), first.data()).merge();
    QCOMPARE(SyncEngine::generationOf(first.data()), qint64(5));
}

void TestSyncEngine::testCombinesDifferentFieldsChangedOnBothSides()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    findEntry(f.local.data(), "Shared")->setPassword("old password");
    QVERIFY(f.sync().value(0).toBool());

    const auto base = QDateTime::currentDateTimeUtc();
    // Another device changes the password, this one the notes, a bit earlier
    QVERIFY(editOnOtherDevice(
        f.server,
        f.dir.filePath("other.kdbx"),
        "Shared",
        [](Entry* e) { e->setPassword("new password"); },
        base.addSecs(20)));
    editEntry(findEntry(f.local.data(), "Shared"), "notes from here", base.addSecs(10));

    QSignalSpy conflicts(f.engine.data(), &SyncEngine::conflictsDetected);
    QVERIFY(f.sync().value(0).toBool());
    // Nothing to report: both changes are kept
    QCOMPARE(conflicts.count(), 0);

    auto* shared = findEntry(f.local.data(), "Shared");
    QCOMPARE(shared->password(), QString("new password"));
    QCOMPARE(shared->notes(), QString("notes from here"));
    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    auto* serverShared = findEntry(serverCopy.data(), "Shared");
    QCOMPARE(serverShared->password(), QString("new password"));
    QCOMPARE(serverShared->notes(), QString("notes from here"));

    // Each side's version before combining is in the history
    QVERIFY(historyHas(
        shared, [](const Entry* e) { return e->notes() == "notes from here" && e->password() == "old password"; }));
    QVERIFY(historyHas(shared, [](const Entry* e) { return e->notes().isEmpty() && e->password() == "new password"; }));

    // Syncing again changes nothing
    const auto revision = f.server->revision;
    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(f.server->revision, revision);
}

void TestSyncEngine::testKeepsAChangeFromASlowClock()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    QVERIFY(f.sync().value(0).toBool());

    // Another device, its clock an hour behind, changes the entry; this one doesn't touch it.
    // Comparing times alone would keep the unchanged copy here, which looks newer.
    QVERIFY(editOnOtherDevice(
        f.server,
        f.dir.filePath("other.kdbx"),
        "Shared",
        [](Entry* e) { e->setNotes("changed elsewhere"); },
        QDateTime::currentDateTimeUtc().addSecs(-3600)));
    // Something else changes here, so this device has to merge
    addEntry(f.local.data(), "Unrelated");

    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(findEntry(f.local.data(), "Shared")->notes(), QString("changed elsewhere"));
    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(findEntry(serverCopy.data(), "Shared")->notes(), QString("changed elsewhere"));
}

void TestSyncEngine::testMergesEveryKindOfField()
{
    QTemporaryDir dir;
    auto base = createDatabase(dir.filePath("base.kdbx"));
    QVERIFY(base);
    addEntry(base.data(), "Shared");
    const auto then = QDateTime::currentDateTimeUtc().addSecs(-100);
    editEntry(
        findEntry(base.data(), "Shared"),
        [](Entry* e) {
            e->attributes()->set("PIN", "1234", true);
            e->attributes()->set("Old", "remove me");
            e->attachments()->set("a.txt", "attachment");
            e->setTags("work");
        },
        then);
    QVERIFY(base->save(Database::Atomic));
    QVERIFY(QFile::copy(base->filePath(), dir.filePath("local.kdbx")));
    QVERIFY(QFile::copy(base->filePath(), dir.filePath("remote.kdbx")));
    auto local = QSharedPointer<Database>::create();
    QVERIFY(local->open(dir.filePath("local.kdbx"), makeKey("a")));
    auto remote = QSharedPointer<Database>::create();
    QVERIFY(remote->open(dir.filePath("remote.kdbx"), makeKey("a")));

    editEntry(
        findEntry(local.data(), "Shared"),
        [](Entry* e) {
            e->attributes()->set("PIN", "5678", true);
            e->attachments()->remove("a.txt");
            e->attributes()->remove("Old");
        },
        then.addSecs(10));
    editEntry(
        findEntry(remote.data(), "Shared"),
        [](Entry* e) {
            e->setTags("work;bank");
            e->attributes()->set("Recovery", "words", true);
            e->setUsername("me@example.com");
        },
        then.addSecs(20));

    QVERIFY(!SyncEngine::mergeDatabases(local.data(), remote.data(), base.data()).isEmpty());
    for (auto* db : {local.data(), remote.data()}) {
        auto* shared = findEntry(db, "Shared");
        QCOMPARE(shared->attributes()->value("PIN"), QString("5678"));
        QVERIFY(shared->attributes()->isProtected("PIN"));
        QVERIFY(!shared->attributes()->hasKey("Old"));
        QVERIFY(!shared->attachments()->hasKey("a.txt"));
        QCOMPARE(shared->tags(), QString("bank,work"));
        QCOMPARE(shared->attributes()->value("Recovery"), QString("words"));
        QVERIFY(shared->attributes()->isProtected("Recovery"));
        QCOMPARE(shared->username(), QString("me@example.com"));
    }
    // Both copies ended up as the same version
    QVERIFY(findEntry(local.data(), "Shared")
                ->equals(findEntry(remote.data(), "Shared"),
                         CompareItemIgnoreHistory | CompareItemIgnoreMilliseconds | CompareItemIgnoreLocation));
}

void TestSyncEngine::testMergesWithoutABase()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    QVERIFY(f.sync().value(0).toBool());
    // E.g. the base was deleted, or comes from before a password change
    QVERIFY(QFile::remove(SyncStateStore(f.dir.filePath("state.json")).basePath(f.local->filePath(), serverParams())));

    const auto base = QDateTime::currentDateTimeUtc();
    QVERIFY(editOnOtherDevice(
        f.server,
        f.dir.filePath("other.kdbx"),
        "Shared",
        [](Entry* e) { e->setPassword("new password"); },
        base.addSecs(20)));
    editEntry(findEntry(f.local.data(), "Shared"), "notes from here", base.addSecs(10));

    QSignalSpy conflicts(f.engine.data(), &SyncEngine::conflictsDetected);
    QVERIFY(f.sync().value(0).toBool());
    // Without the base the newer entry wins as before, and the conflict is reported without fields
    QCOMPARE(conflicts.count(), 1);
    const auto list = conflicts.first().first().value<QList<SyncConflict>>();
    QCOMPARE(list.size(), 1);
    QVERIFY(list.first().fields.isEmpty());
    auto* shared = findEntry(f.local.data(), "Shared");
    QCOMPARE(shared->password(), QString("new password"));
    QVERIFY(historyHas(shared, [](const Entry* e) { return e->notes() == "notes from here"; }));
}

namespace
{
    // Another time in the same second, which the file can't tell apart
    QDateTime sameSecond(const QDateTime& time)
    {
        const auto msec = time.time().msec();
        return QDateTime::fromMSecsSinceEpoch(time.toSecsSinceEpoch() * 1000 + (msec >= 500 ? msec - 400 : msec + 400),
                                              QTimeZone::utc());
    }
} // namespace

void TestSyncEngine::testKeepsAnEditFromTheSameSecondAsTheSync()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    QVERIFY(f.sync().value(0).toBool());
    auto* shared = findEntry(f.local.data(), "Shared");
    const auto synced = shared->timeInfo().lastModificationTime();

    // Edited here right after the sync, within the same second: by whole seconds it looks unchanged
    editEntry(shared, "changed here", sameSecond(synced));
    QVERIFY(editOnOtherDevice(
        f.server,
        f.dir.filePath("other.kdbx"),
        "Shared",
        [](Entry* e) { e->setPassword("changed elsewhere"); },
        synced.addSecs(5)));

    QVERIFY(f.sync().value(0).toBool());
    shared = findEntry(f.local.data(), "Shared");
    QCOMPARE(shared->notes(), QString("changed here"));
    QCOMPARE(shared->password(), QString("changed elsewhere"));
}

void TestSyncEngine::testTakesAnotherDevicesEditFromTheSameSecond()
{
    Fixture f;
    QVERIFY(f.local);
    addEntry(f.local.data(), "Shared");
    QVERIFY(f.sync().value(0).toBool());
    const auto synced = findEntry(f.local.data(), "Shared")->timeInfo().lastModificationTime();

    // Another device edits within the same second as this device's sync; nothing changes here
    QVERIFY(editOnOtherDevice(
        f.server,
        f.dir.filePath("other.kdbx"),
        "Shared",
        [](Entry* e) { e->setNotes("changed elsewhere"); },
        sameSecond(synced)));

    QVERIFY(f.sync().value(0).toBool());
    QCOMPARE(findEntry(f.local.data(), "Shared")->notes(), QString("changed elsewhere"));
    auto serverCopy = openServerCopy(f.server, f.dir.filePath("check.kdbx"));
    QVERIFY(serverCopy);
    QCOMPARE(findEntry(serverCopy.data(), "Shared")->notes(), QString("changed elsewhere"));
}
