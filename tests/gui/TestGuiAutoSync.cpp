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

#include "../util/FakeAccountServer.h"
#include "../util/TemporaryFile.h"
#include "account/AccountCrypto.h"
#include "account/AccountManager.h"
#include "account/VaultKeyring.h"
#include "account/VaultSharing.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/Application.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/MessageBox.h"
#include "gui/account/AccountController.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncHistoryDialog.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"
#include "keeshare/KeeShare.h"
#include "keys/PasswordKey.h"

#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUuid>

int main(int argc, char* argv[])
{
    // Sync state and base copies go to a test location, not the real KeePassXC data folder
    QStandardPaths::setTestModeEnabled(true);
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
     * These tests run against a live server from keepassxc-sync-server, like testremotesyncserver:
     *   OPENVIREO_SYNC_SERVER_URL=http://127.0.0.1:8787 OPENVIREO_SYNC_SERVER_TOKEN=<token> ./testguiautosync
     */
    RemoteParams serverParams()
    {
        RemoteParams params;
        params.name = "server";
        params.type = RemoteParams::Type::SyncServer;
        params.serverUrl = qEnvironmentVariable("OPENVIREO_SYNC_SERVER_URL");
        params.serverToken = qEnvironmentVariable("OPENVIREO_SYNC_SERVER_TOKEN");
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
    // Delete the database on the server, as an administrator might
    bool deleteOnServer(const RemoteParams& params)
    {
        QNetworkRequest request(QUrl(params.serverUrl + "/api/v1/databases/" + params.serverDatabaseId));
        request.setRawHeader("Authorization", "Bearer " + params.serverToken.toUtf8());
        QNetworkAccessManager nam;
        QScopedPointer<QNetworkReply> reply(nam.deleteResource(request));
        QEventLoop loop;
        QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() / 100 == 2;
    }
} // namespace

#define SKIP_WITHOUT_SERVER()                                                                                          \
    if (qEnvironmentVariableIsEmpty("OPENVIREO_SYNC_SERVER_URL")                                                            \
        || qEnvironmentVariableIsEmpty("OPENVIREO_SYNC_SERVER_TOKEN")) {                                                    \
        QSKIP("Set OPENVIREO_SYNC_SERVER_URL and OPENVIREO_SYNC_SERVER_TOKEN to run automatic sync tests");                      \
    }

void TestGuiAutoSync::initTestCase()
{
    QVERIFY(Crypto::init());
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    Application::bootstrap();
    // Never touch the real Keychain or token file
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
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

void TestGuiAutoSync::testAsksBeforeRecreatingALostDatabase()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);
    addEntry(db.data(), "Kept");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QVERIFY(deleteOnServer(params));

    // The background sync stops instead of quietly uploading again
    addEntry(db.data(), "Later");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::NeedsAttention, 10000);
    QVERIFY(widget.syncStatus().error.contains("no longer has this database"));
    QCOMPARE(serverTitles(params), QStringList({"<nothing on the server>"}));

    // Declining leaves it that way
    MessageBox::setNextAnswer(MessageBox::Cancel);
    widget.syncWithRemote(widget.getRemoteParams().first());
    QTest::qWait(200);
    QCOMPARE(widget.syncStatus().state, State::NeedsAttention);
    QCOMPARE(serverTitles(params), QStringList({"<nothing on the server>"}));

    // Agreeing uploads this device's copy again
    MessageBox::setNextAnswer(MessageBox::Yes);
    widget.syncWithRemote(widget.getRemoteParams().first());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QCOMPARE(serverTitles(params), QStringList({"Kept", "Later"}));
}

void TestGuiAutoSync::testWaitsForThisDevicesToken()
{
    // No server needed: without a token nothing is sent
    using State = DatabaseWidget::SyncStatus::State;
    auto params = serverParams();
    params.serverUrl = "https://sync.invalid";
    params.serverToken.clear();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);

    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::NeedsToken, 5000);
    QVERIFY(widget.syncStatus().error.contains("no access token"));
    QVERIFY(widget.isEnabled());
}

void TestGuiAutoSync::testSyncsWhenTheServerAnnouncesAChange()
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
    // Let the event stream connect and the sync that follows its connecting settle
    QTest::qWait(1500);

    // Nothing is saved here and the next poll is a minute away: only the server's notice can bring this in
    QVERIFY(uploadFromOtherDevice(params, "Announced"));
    QTRY_COMPARE_WITH_TIMEOUT(titles(db.data()), QStringList({"Announced"}), 5000);
}

void TestGuiAutoSync::testOpensAnOlderVersion()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);
    addEntry(db.data(), "In the first version");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QVERIFY(uploadFromOtherDevice(params, "Added later"));

    SyncHistoryDialog dialog(params);
    QSignalSpy opened(&dialog, &SyncHistoryDialog::openRequested);
    auto* versions = dialog.findChild<QTreeWidget*>("syncHistoryVersions");
    QVERIFY(versions);
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 2, 5000);
    QVERIFY(versions->topLevelItem(0)->text(0).contains("current"));

    // Open the first version
    versions->setCurrentItem(versions->topLevelItem(1));
    auto* open = dialog.findChild<QPushButton*>();
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == "Open") {
            open = button;
        }
    }
    QVERIFY(open && open->isEnabled());
    open->click();
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 5000);

    const auto filePath = opened.first().first().toString();
    QVERIFY(filePath.contains("version 1"));
    Database version;
    QVERIFY(version.open(filePath, makeKey()));
    QCOMPARE(titles(&version), QStringList({"In the first version"}));
    QFile::remove(filePath);
}

void TestGuiAutoSync::testOpensAnOlderVersionFromTheVaultCard()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    QWidget window;
    window.resize(1000, 700);
    auto* widget = new DatabaseWidget(db, &window);
    window.show();
    addEntry(db.data(), "In the first version");
    QVERIFY(widget->save());
    QTRY_COMPARE_WITH_TIMEOUT(widget->syncStatus().state, State::Synced, 10000);
    QVERIFY(uploadFromOtherDevice(params, "Added later"));

    QSignalSpy opened(widget, &DatabaseWidget::requestOpenDatabase);
    widget->showServerHistory(&params);
    // A card over the window instead of a dialog of its own, with the buttons in its footer
    auto* card = window.findChild<QWidget*>("vaultSyncHistoryCard");
    QVERIFY(card && card->isVisible());
    auto* dialog = window.findChild<SyncHistoryDialog*>();
    QVERIFY(dialog && !dialog->isWindow());
    QVERIFY(!dialog->buttonBox()->isVisible());
    auto* open = window.findChild<QPushButton*>("vaultSyncHistoryOpen");
    QVERIFY(open && !open->isEnabled());

    auto* versions = dialog->versionList();
    QTRY_COMPARE_WITH_TIMEOUT(versions->topLevelItemCount(), 2, 5000);
    versions->setCurrentItem(versions->topLevelItem(1));
    QVERIFY(open->isEnabled());
    open->click();
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 5000);
    QVERIFY(opened.first().first().toString().contains("version 1"));
    QFile::remove(opened.first().first().toString());

    // Opening the version closes the card
    QTRY_VERIFY(!window.findChild<QWidget*>("vaultSyncHistoryCard"));
}

void TestGuiAutoSync::testCombinesChangesToDifferentFields()
{
    SKIP_WITHOUT_SERVER();

    using State = DatabaseWidget::SyncStatus::State;
    const auto params = serverParams();
    QTemporaryDir dir;
    auto db = createDatabase(dir.filePath("local.kdbx"), params);
    QVERIFY(db);
    DatabaseWidget widget(db);
    addEntry(db.data(), "Bank");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);

    // Another device changes the password...
    {
        RemoteHandler handler;
        auto result = handler.download(&params);
        QVERIFY(result.success);
        Database other;
        QVERIFY(other.open(result.filePath, makeKey()));
        auto* entry = other.rootGroup()->entriesRecursive().first();
        entry->setPassword("changed elsewhere");
        auto timeInfo = entry->timeInfo();
        timeInfo.setLastModificationTime(QDateTime::currentDateTimeUtc().addSecs(5));
        entry->setTimeInfo(timeInfo);
        QVERIFY(other.save(Database::Atomic));
        QVERIFY(handler.upload(result.filePath, &params, result.etag).success);
        QFile::remove(result.filePath);
    }
    // ...and this one the notes, before hearing about it
    const auto uuid = db->rootGroup()->entriesRecursive().first()->uuid();
    db->rootGroup()->findEntryByUuid(uuid)->setNotes("changed here");
    QVERIFY(widget.save());

    // Merging may replace the entry object, so look it up each time
    const auto entry = [&] { return widget.database()->rootGroup()->findEntryByUuid(uuid); };
    QTRY_COMPARE_WITH_TIMEOUT(entry()->password(), QString("changed elsewhere"), 10000);
    QCOMPARE(entry()->notes(), QString("changed here"));
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
}

namespace
{
    // An in-memory sync server for account vaults that records the path and token of every request
    struct VaultServer
    {
        QByteArray stored;
        QString etag;
        qint64 revision = 0;
        QStringList requests;
        QStringList tokens;
    };

    class FakeVaultClient : public SyncServerClient
    {
    public:
        FakeVaultClient(const RemoteParams& params, QSharedPointer<VaultServer> server)
            : SyncServerClient(params)
            , m_server(std::move(server))
        {
        }

    protected:
        void send(const QByteArray& verb,
                  const QUrl& url,
                  const Headers& headers,
                  const QByteArray& body,
                  int,
                  Callback callback) override
        {
            auto& server = *m_server;
            server.requests << QString::fromLatin1(verb) + " " + url.path();
            QString ifMatch, ifNoneMatch, token;
            for (const auto& header : headers) {
                if (header.first == "Authorization") {
                    token = QString::fromLatin1(header.second);
                } else if (header.first == "If-Match") {
                    ifMatch = QString::fromLatin1(header.second);
                } else if (header.first == "If-None-Match") {
                    ifNoneMatch = QString::fromLatin1(header.second);
                }
            }
            server.tokens << token;
            Response response;
            if (token.isEmpty()) {
                response.status = 401;
            } else if (verb == "PUT") {
                if (ifMatch.isEmpty() && ifNoneMatch.isEmpty()) {
                    response.status = 428;
                } else if ((ifNoneMatch == "*" && !server.stored.isEmpty())
                           || (!ifMatch.isEmpty() && ifMatch != server.etag)) {
                    response.status = 412;
                } else {
                    response.status = server.stored.isEmpty() ? 201 : 200;
                    server.stored = body;
                    server.etag = etagFor(body);
                    ++server.revision;
                }
            } else if (server.stored.isEmpty()) {
                response.status = 404;
            } else if (verb == "HEAD" && ifNoneMatch == server.etag) {
                response.status = 304;
            } else {
                response.status = 200;
                if (verb == "GET") {
                    response.body = server.stored;
                }
            }
            if (!server.stored.isEmpty()) {
                response.etag = server.etag;
                response.revision = server.revision;
            }
            deliverLater(response, std::move(callback));
        }

    private:
        QSharedPointer<VaultServer> m_server;
    };
} // namespace

void TestGuiAutoSync::testSyncsAnAccountVaultWithTheAccountsToken()
{
    // No server needed: the sync server client is replaced by an in-memory one
    using State = DatabaseWidget::SyncStatus::State;
    auto server = QSharedPointer<VaultServer>::create();
    SyncServerClient::setFactory(
        [server](const RemoteParams& params) -> SyncServerClient* { return new FakeVaultClient(params, server); });
    const auto restoreFactory = qScopeGuard([] { SyncServerClient::setFactory(nullptr); });

    const auto accountId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    RemoteParams params;
    params.name = "KeePassXC account";
    params.type = RemoteParams::Type::AccountVault;
    // Nothing listens here; the event stream just keeps retrying
    params.serverUrl = "http://127.0.0.1:9/api/v1";
    params.accountId = accountId;
    params.vaultId = vaultId;
    params.autoSync = true;

    // An account vault: its key is the vault key as a key file, and there is no password
    const auto vaultKey = AccountCrypto::generateVaultKey();
    QTemporaryDir dir;
    auto db = VaultKeyring::createVaultDatabase("Personal", vaultKey);
    {
        RemoteSettings settings(db);
        settings.addRemoteParams(new RemoteParams(params));
        settings.saveSettings();
    }
    QVERIFY(db->saveAs(dir.filePath("vault.kdbx")));
    // The database carries no token for the vault
    QVERIFY(!db->metadata()->customData()->value(CustomData::RemoteProgramSettings).contains("Bearer"));

    // Signed out: nothing is sent
    DatabaseWidget widget(db);
    addEntry(db.data(), "First");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::NeedsToken, 5000);
    QVERIFY(widget.syncStatus().error.contains("Sign in"));
    QVERIFY(server->requests.isEmpty());

    // The account signs in: its provider hands out the current access token
    QString accessToken = "jwt-1";
    SyncServerClient::setAccountTokenProvider(
        accountId, [&accessToken](std::function<void(const QString&)> done) { done(accessToken); });
    const auto signOut = qScopeGuard([accountId] { SyncServerClient::setAccountTokenProvider(accountId, nullptr); });
    addEntry(db.data(), "Second");
    QVERIFY(widget.save());
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QVERIFY(server->requests.contains("PUT /api/v1/vaults/" + vaultId));
    for (const auto& request : server->requests) {
        QVERIFY2(request.contains("/api/v1/vaults/" + vaultId), qPrintable(request));
    }
    QVERIFY(!server->tokens.isEmpty());
    for (const auto& token : server->tokens) {
        QCOMPARE(token, QString("Bearer jwt-1"));
    }
    auto uploaded = VaultKeyring::readVault(server->stored, VaultKeyring::compositeKeyFor(vaultKey));
    QVERIFY(uploaded);
    QCOMPARE(titles(uploaded.data()), QStringList({"First", "Second"}));

    // After a refresh the next sync goes out with the new token
    accessToken = "jwt-2";
    server->tokens.clear();
    addEntry(db.data(), "Third");
    QVERIFY(widget.save());
    QTRY_VERIFY_WITH_TIMEOUT(server->tokens.contains("Bearer jwt-2"), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(widget.syncStatus().state, State::Synced, 10000);
    QVERIFY(!server->tokens.contains("Bearer jwt-1"));
    uploaded = VaultKeyring::readVault(server->stored, VaultKeyring::compositeKeyFor(vaultKey));
    QVERIFY(uploaded);
    QCOMPARE(titles(uploaded.data()), QStringList({"First", "Second", "Third"}));
}

namespace
{
    // An account signed up on the fake account server, unlocked
    AccountManager* signUp(const QSharedPointer<FakeAccount::Server>& server, const QString& email)
    {
        auto* manager = FakeAccount::createManager(server);
        manager->setServer(FakeAccount::serverConfig());
        manager->registerAccount(email, "correct horse battery staple");
        QSignalSpy vaults(manager, &AccountManager::vaultsChanged);
        if (!QTest::qWaitFor([manager] { return manager->state() == AccountManager::State::Unlocked; }, 10000)
            || !vaults.wait(5000)) {
            delete manager;
            return nullptr;
        }
        return manager;
    }

    QStringList vaultTitles(const QSharedPointer<FakeAccount::Server>& server,
                            const QString& vaultId,
                            const AccountCrypto::SecureBytes& key)
    {
        auto db = VaultKeyring::readVault(server->blobs.value(vaultId).data, VaultKeyring::compositeKeyFor(key));
        return db ? titles(db.data()) : QStringList{"<could not open>"};
    }
} // namespace

void TestGuiAutoSync::testAccountVaultFollowsAKeyRotation()
{
    using State = DatabaseWidget::SyncStatus::State;
    auto server = QSharedPointer<FakeAccount::Server>::create();
    FakeAccount::installSyncFactory(server);
    const auto restoreFactory = qScopeGuard([] { FakeAccount::restoreSyncFactory(); });

    DatabaseTabWidget tabs;
    auto* account = signUp(server, "alice@example.com");
    QVERIFY(account);
    AccountController controller(&tabs, account);
    controller.createVault("Family");
    QTRY_COMPARE_WITH_TIMEOUT(tabs.count(), 1, 10000);
    QTRY_VERIFY(!account->keyring().isEmpty());
    const auto vaultId = account->keyring().vaults().first().id;
    auto* widget = controller.vaultWidget(vaultId);
    QVERIFY(widget);
    addEntry(widget->database().data(), "Before");
    QVERIFY(widget->save());
    QTRY_COMPARE_WITH_TIMEOUT(widget->syncStatus().state, State::Synced, 10000);
    const auto oldKey = account->keyring().vaultKey(vaultId);
    QCOMPARE(vaultTitles(server, vaultId, oldKey), QStringList{"Before"});

    // The key rotates (a member left) while the vault is open on this device
    bool rotated = false;
    controller.sharing()->rotate(vaultId, {}, [&](bool ok, const QString& error) {
        QVERIFY2(ok, qPrintable(error));
        rotated = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(rotated, 10000);
    QTRY_COMPARE(account->keyring().vault(vaultId).currentKeyVersion, 2);
    const auto newKey = account->keyring().vaultKey(vaultId);
    QVERIFY(newKey != oldKey);

    // The open vault takes the new key, so its next sync can open the re-keyed blob and merge
    QTRY_VERIFY(widget->database()->key()->rawKey() == VaultKeyring::compositeKeyFor(newKey)->rawKey());
    addEntry(widget->database().data(), "After");
    QVERIFY(widget->save());
    QTRY_COMPARE_WITH_TIMEOUT(vaultTitles(server, vaultId, newKey), QStringList({"After", "Before"}), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(widget->syncStatus().state, State::Synced, 10000);
    QCOMPARE(vaultTitles(server, vaultId, oldKey), QStringList{"<could not open>"});
    // The local copy is under the new key too
    Database local;
    QVERIFY(local.open(controller.vaultFilePath(vaultId), VaultKeyring::compositeKeyFor(newKey)));
    controller.lockAccount();
}

void TestGuiAutoSync::testSharedVaultSyncsWithThisDevicesAccount()
{
    using State = DatabaseWidget::SyncStatus::State;
    auto server = QSharedPointer<FakeAccount::Server>::create();
    FakeAccount::installSyncFactory(server);
    const auto restoreFactory = qScopeGuard([] { FakeAccount::restoreSyncFactory(); });

    // Alice creates a vault and shares it with Bob
    QScopedPointer<AccountManager> alice(signUp(server, "alice@example.com"));
    QVERIFY(alice);
    VaultSharing sharing(alice.data());
    QString vaultId;
    sharing.createVault("Shared",
                        [&](const QString& id, const QSharedPointer<Database>&, const QString&) { vaultId = id; });
    QTRY_VERIFY_WITH_TIMEOUT(!vaultId.isEmpty(), 10000);
    auto* bob = signUp(server, "bob@example.com");
    QVERIFY(bob);
    bool shared = false;
    sharing.lookupRecipient("bob@example.com", [&](const VaultSharing::Recipient& recipient, const QString&) {
        sharing.grant(vaultId, recipient, "write", [&](bool ok, const QString& error) {
            QVERIFY2(ok, qPrintable(error));
            shared = true;
        });
    });
    QTRY_VERIFY(shared);

    // Bob opens it on his device; its settings still name Alice's account
    DatabaseTabWidget tabs;
    AccountController controller(&tabs, bob);
    QSignalSpy reloaded(bob, &AccountManager::vaultsChanged);
    // The access event that would announce the grant: a fresh token for the new vlt claim, and the vault list
    bob->refreshAccessToken();
    bob->reloadVaults();
    QVERIFY(reloaded.wait());
    controller.openAllVaults();
    QTRY_COMPARE_WITH_TIMEOUT(tabs.count(), 1, 10000);
    auto* widget = controller.vaultWidget(vaultId);
    QVERIFY(widget);
    RemoteSettings settings(widget->database());
    QCOMPARE(settings.getAllRemoteParams().size(), 1);
    QCOMPARE(settings.getAllRemoteParams().first()->accountId, alice->accountId());

    // Bob's change goes out with Bob's token
    server->requests.clear();
    server->syncPrincipals.clear();
    addEntry(widget->database().data(), "From Bob");
    QVERIFY(widget->save());
    QTRY_COMPARE_WITH_TIMEOUT(
        vaultTitles(server, vaultId, alice->keyring().vaultKey(vaultId)), QStringList{"From Bob"}, 10000);
    QVERIFY(server->requests.contains("sync PUT vaults/" + vaultId));
    // Not Alice's, although her account is signed in on this device too and the settings name it
    QVERIFY(server->syncPrincipals.contains(bob->principal()));
    QVERIFY(!server->syncPrincipals.contains(alice->principal()));
    QTRY_COMPARE_WITH_TIMEOUT(widget->syncStatus().state, State::Synced, 10000);
    controller.lockAccount();
}
