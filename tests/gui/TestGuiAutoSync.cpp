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

#include "TestGuiAutoSync.h"

#include "../util/TemporaryFile.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/Application.h"
#include "gui/DatabaseWidget.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"
#include "keeshare/KeeShare.h"
#include "keys/PasswordKey.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

int main(int argc, char* argv[])
{
    // DatabaseWidget relies on KeePassXC's application object for its theme
    Application app(argc, argv);
    app.setApplicationName("KeePassXC");
    app.setQuitOnLastWindowClosed(false);
    app.applyTheme();
    TestGuiAutoSync tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    /*
     * These tests run against a live server from sync_server/, like testremotesyncserver:
     *   KPXC_SYNC_SERVER_URL=http://127.0.0.1:8787 KPXC_SYNC_SERVER_TOKEN=<token> ./testguiautosync
     */
    RemoteParams serverParams()
    {
        RemoteParams params;
        params.name = "server";
        params.type = RemoteParams::Type::SyncServer;
        params.serverUrl = qEnvironmentVariable("KPXC_SYNC_SERVER_URL");
        params.serverToken = qEnvironmentVariable("KPXC_SYNC_SERVER_TOKEN");
        params.serverDatabaseId = "gui-test-" + QUuid::createUuid().toString(QUuid::Id128);
        params.autoSync = true;
        return params;
    }

    QSharedPointer<CompositeKey> makeKey()
    {
        auto key = QSharedPointer<CompositeKey>::create();
        key->addKey(QSharedPointer<PasswordKey>::create("a"));
        return key;
    }

    // A saved database with automatic sync turned on for `params`
    QSharedPointer<Database> createDatabase(const QString& filePath, const RemoteParams& params)
    {
        auto db = QSharedPointer<Database>::create();
        auto kdf = QSharedPointer<AesKdf>::create();
        kdf->setRounds(1);
        db->setKdf(kdf);
        db->setKey(makeKey());
        RemoteSettings settings(db);
        settings.addRemoteParams(new RemoteParams(params));
        settings.saveSettings();
        if (!db->saveAs(filePath)) {
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

    QStringList titles(Database* db)
    {
        QStringList result;
        for (const auto* entry : db->rootGroup()->entriesRecursive()) {
            result << entry->title();
        }
        result.sort();
        return result;
    }

    // What the server has, as another device would see it
    QStringList serverTitles(const RemoteParams& params, QString* etag = nullptr)
    {
        RemoteHandler handler;
        auto result = handler.download(&params);
        if (!result.success || result.remoteMissing) {
            return {"<nothing on the server>"};
        }
        if (etag) {
            *etag = result.etag;
        }
        Database db;
        auto ok = db.open(result.filePath, makeKey());
        QFile::remove(result.filePath);
        return ok ? titles(&db) : QStringList{"<could not open>"};
    }

    // Another device adds an entry and uploads its copy
    bool uploadFromOtherDevice(const RemoteParams& params, const QString& title)
    {
        RemoteHandler handler;
        auto result = handler.download(&params);
        if (!result.success || result.remoteMissing) {
            return false;
        }
        Database db;
        if (!db.open(result.filePath, makeKey())) {
            return false;
        }
        addEntry(&db, title);
        if (!db.save(Database::Atomic)) {
            return false;
        }
        auto upload = handler.upload(result.filePath, &params, result.etag);
        QFile::remove(result.filePath);
        return upload.success;
    }
} // namespace

#define SKIP_WITHOUT_SERVER()                                                                                          \
    if (qEnvironmentVariableIsEmpty("KPXC_SYNC_SERVER_URL")                                                            \
        || qEnvironmentVariableIsEmpty("KPXC_SYNC_SERVER_TOKEN")) {                                                    \
        QSKIP("Set KPXC_SYNC_SERVER_URL and KPXC_SYNC_SERVER_TOKEN to run automatic sync tests");                      \
    }

void TestGuiAutoSync::initTestCase()
{
    QVERIFY(Crypto::init());
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    Application::bootstrap();
    // Normally done by MainWindow, which these tests don't need
    KeeShare::init(this);
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
}

void TestGuiAutoSync::testSyncsInTheBackgroundAfterSaves()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);

    // The first save turns automatic sync on and publishes the database
    addEntry(db.data(), "First");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QVERIFY(widget.syncStatus().automatic);
    QCOMPARE(serverTitles(params), QStringList({"First"}));

    // Another device's change comes in with the next sync, and ours goes out
    QVERIFY(uploadFromOtherDevice(params, "From another device"));
    addEntry(db.data(), "Second");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(titles(db.data()), QStringList({"First", "From another device", "Second"}), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(serverTitles(params), QStringList({"First", "From another device", "Second"}), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    // The user never saw a dialog or a message for it
    QVERIFY(widget.isEnabled());
}

void TestGuiAutoSync::testWaitsWhileTheDatabaseHasUnsavedChanges()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);

    // A change that isn't saved yet is never merged into or uploaded
    QString etag;
    serverTitles(params, &etag);
    addEntry(db.data(), "Unsaved");
    QTest::qWait(4000);
    QString etagAfter;
    QCOMPARE(serverTitles(params, &etagAfter), QStringList());
    QCOMPARE(etagAfter, etag);
    QVERIFY(db->isModified());

    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(serverTitles(params), QStringList({"Unsaved"}), 10000);
}

void TestGuiAutoSync::testManualSyncReportsToTheUser()
{
    SKIP_WITHOUT_SERVER();

    auto params = serverParams();
    params.autoSync = false;
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);
    QVERIFY(widget.save());
    QTest::qWait(100);
    QVERIFY(!widget.syncStatus().automatic);

    QSignalSpy started(&widget, &DatabaseWidget::databaseSyncInProgress);
    QSignalSpy completed(&widget, &DatabaseWidget::databaseSyncCompleted);
    widget.syncWithRemote(widget.getRemoteParams().first());
    QCOMPARE(started.count(), 1);
    // The window waits for the sync the user asked for
    QVERIFY(!widget.isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
    QVERIFY(widget.isEnabled());
    QCOMPARE(widget.syncStatus().state, DatabaseWidget::SyncStatus::State::Synced);
    QCOMPARE(serverTitles(params), QStringList());
}
