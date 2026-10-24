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

#include "TestGuiAccounts.h"

#include "../util/FakeAccountServer.h"
#include "../util/TemporaryFile.h"

#include "account/AccountManager.h"
#include "account/ServiceAccountAdmin.h"
#include "account/VaultKeyring.h"
#include "account/VaultSharing.h"
#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "crypto/kdf/AesKdf.h"
#include "gui/Application.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/MainWindow.h"
#include "gui/MessageBox.h"
#include "gui/account/AccountController.h"
#include "gui/account/AccountStartWidget.h"
#include "gui/account/EmergencyKit.h"
#include "gui/account/ServiceAccountsDialog.h"
#include "gui/account/ShareVaultDialog.h"
#include "gui/account/SignUpWizard.h"
#include "gui/remote/DatabaseSettingsWidgetRemote.h"
#include "gui/remote/RemoteSettings.h"
#include "gui/remote/SyncServerClient.h"
#include "gui/remote/SyncTokenStore.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/AccountVaultsPane.h"
#include "keys/PasswordKey.h"
#include "quickunlock/QuickUnlockInterface.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

using namespace FakeAccount;

int main(int argc, char* argv[])
{
    QStandardPaths::setTestModeEnabled(true);
    Application app(argc, argv);
    app.setApplicationName("KeePassXC");
    app.setQuitOnLastWindowClosed(false);
    app.applyTheme();
    TestGuiAccounts tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    const QString Email = QStringLiteral("alice@example.com");
    const QString Password = QStringLiteral("correct horse battery staple");

    template <typename T> T* child(QObject* parent, const QString& name)
    {
        auto* widget = parent ? parent->findChild<T*>(name) : nullptr;
        if (!widget) {
            qWarning("No %s", qPrintable(name));
        }
        return widget;
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

    void addEntry(Database* db, const QString& title)
    {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setGroup(db->rootGroup());
    }
} // namespace

void TestGuiAccounts::initTestCase()
{
    QVERIFY(Crypto::init());
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    Application::bootstrap();
    FluentTheme::setVaultLayoutEnabledForTests(false);
}

void TestGuiAccounts::init()
{
    config()->resetToDefaults();
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    config()->set(Config::OpenPreviousDatabasesOnStartup, false);
    config()->set(Config::Account_ServerUrl, Issuer);
    // A fresh device and a fresh server for every test
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    // The window lives as long as the test case (KeePassXC doesn't support a second main window), so the
    // server it talks to is emptied instead of replaced
    if (!m_server) {
        m_server = QSharedPointer<Server>::create();
    } else {
        *m_server = Server();
    }
    installSyncFactory(m_server);
    const auto server = m_server;
    AccountController::setAccountManagerFactory([server] { return createManager(server); });
    QDir(AccountController::vaultDirectory({}).section('/', 0, -2)).removeRecursively();
}

void TestGuiAccounts::cleanup()
{
    if (m_mainWindow) {
        auto* controller = m_mainWindow->accountController();
        controller->lockAccount();
        controller->account()->signOut();
        for (auto* dialog : m_mainWindow->findChildren<QDialog*>()) {
            dialog->close();
        }
        auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->databaseWidgetFromIndex(i)->database()->markAsClean();
        }
        tabs->closeAllDatabaseTabs();
    }
    restoreSyncFactory();
}

void TestGuiAccounts::cleanupTestCase()
{
    AccountController::setAccountManagerFactory(nullptr);
}

void TestGuiAccounts::createWindow(const QString& mode)
{
    if (mode.isEmpty()) {
        config()->remove(Config::Account_StartMode);
    } else {
        config()->set(Config::Account_StartMode, mode);
    }
    if (!m_mainWindow) {
        m_mainWindow.reset(new MainWindow());
        m_mainWindow->resize(1024, 768);
        m_mainWindow->show();
    } else {
        // What a launch with this mode does
        auto* controller = m_mainWindow->accountController();
        controller->setMode(AccountController::savedMode());
        controller->restore();
        if (controller->needsStartScreen()) {
            m_mainWindow->showStartScreen();
        } else {
            QMetaObject::invokeMethod(m_mainWindow.data(), "finishStartScreen");
        }
    }
    QApplication::processEvents();
}

QString TestGuiAccounts::registerElsewhere(const QString& email, bool mfa)
{
    QScopedPointer<AccountManager> other(createManager(m_server));
    other->setServer(serverConfig());
    QString secretKey;
    connect(other.data(), &AccountManager::registered, this, [&](const QString& key) { secretKey = key; });
    other->registerAccount(email, Password);
    if (!QTest::qWaitFor([&] { return other->state() == AccountManager::State::Unlocked; }, 10000)) {
        return {};
    }
    m_server->users[email.toLower()].mfa = mfa;
    other->signOut();
    // This device never saw the account
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    return secretKey;
}

void TestGuiAccounts::signIn(const QString& email, const QString& secretKey)
{
    auto* start = m_mainWindow->startScreen();
    child<QLineEdit>(start, "emailEdit")->setText(email);
    child<QLineEdit>(start, "passwordEdit")->setText(Password);
    child<QLineEdit>(start, "secretKeyEdit")->setText(secretKey);
    QTest::mouseClick(child<QPushButton>(start, "signInButton"), Qt::LeftButton);
}

void TestGuiAccounts::testStartScreenShowsUntilAModeIsChosen()
{
    createWindow({});
    QVERIFY(m_mainWindow->isStartScreenShown());
    auto* welcome = m_mainWindow->findChild<QWidget*>("welcomeWidget");
    QVERIFY(welcome->isHidden());
    QCOMPARE(m_mainWindow->startScreen()->currentPage(), AccountStartWidget::Page::SignIn);
    QVERIFY(m_mainWindow->accountVaultsPane()->isHidden());
    // The hosted server comes from the build (KPXC_DEFAULT_ACCOUNT_SERVER) unless changed
    QCOMPARE(child<QLineEdit>(m_mainWindow->startScreen(), "serverEdit")->text(), Issuer);

    QTest::mouseClick(child<QPushButton>(m_mainWindow->startScreen(), "localButton"), Qt::LeftButton);
    QVERIFY(!m_mainWindow->isStartScreenShown());
    QVERIFY(!welcome->isHidden());
    QCOMPARE(config()->get(Config::Account_StartMode).toString(), QString("local"));

    // The choice is remembered: the next launch goes straight to the welcome screen
    DatabaseTabWidget tabs;
    AccountController nextLaunch(&tabs);
    QCOMPARE(nextLaunch.mode(), AccountController::Mode::Local);
    QVERIFY(!nextLaunch.needsStartScreen());
    createWindow("local");
    QVERIFY(!m_mainWindow->isStartScreenShown());

    // In account mode without a session the start screen comes back
    createWindow("account");
    QVERIFY(m_mainWindow->isStartScreenShown());
    createWindow("selfhosted");
    QVERIFY(!m_mainWindow->isStartScreenShown());
}

void TestGuiAccounts::testUseWithoutAnAccountMakesNoRequests()
{
    createWindow({});
    QTest::mouseClick(child<QPushButton>(m_mainWindow->startScreen(), "localButton"), Qt::LeftButton);

    // Today's local experience: create, save and reopen a database with a master password
    QTemporaryDir dir;
    auto db = QSharedPointer<Database>::create();
    auto kdf = QSharedPointer<AesKdf>::create();
    kdf->setRounds(1);
    db->setKdf(kdf);
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("a"));
    db->setKey(key);
    addEntry(db.data(), "Local");
    QVERIFY(db->saveAs(dir.filePath("local.kdbx")));
    m_mainWindow->openDatabase(dir.filePath("local.kdbx"), "a");
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY(!tabs->currentDatabaseWidget()->isLocked());
    QCOMPARE(titles(tabs->currentDatabaseWidget()->database().data()), QStringList{"Local"});
    QVERIFY(!m_mainWindow->accountController()->isAccountVault(tabs->currentDatabaseWidget()));

    // Nothing went to an account server or a sync server
    QVERIFY2(m_server->requests.isEmpty(), qPrintable(m_server->requests.join(", ")));
}

void TestGuiAccounts::testSignsInOnANewDeviceWithMfa()
{
    const auto secretKey = registerElsewhere(Email, true);
    QVERIFY(!secretKey.isEmpty());
    createWindow({});
    auto* start = m_mainWindow->startScreen();

    // A device that hasn't signed in before asks for the Secret Key
    child<QLineEdit>(start, "emailEdit")->setText(Email);
    QVERIFY(!child<QLineEdit>(start, "secretKeyEdit")->isHidden());
    signIn(Email, secretKey);
    QTRY_COMPARE_WITH_TIMEOUT(start->currentPage(), AccountStartWidget::Page::Mfa, 10000);
    // The sync server came from the account server's discovery document
    QVERIFY(m_server->requests.contains("GET .well-known/openvireo-configuration"));
    QCOMPARE(m_mainWindow->accountController()->account()->server().sync, SyncUrl);

    // A wrong code keeps the prompt
    child<QLineEdit>(start, "mfaCodeEdit")->setText("000000");
    QTest::mouseClick(child<QPushButton>(start, "mfaVerifyButton"), Qt::LeftButton);
    QTRY_VERIFY(m_server->requests.filter("auth/login/mfa").size() == 1);
    QTRY_COMPARE(m_mainWindow->accountController()->account()->state(), AccountManager::State::NeedsMfa);
    QCOMPARE(start->currentPage(), AccountStartWidget::Page::Mfa);
    QVERIFY(child<MessageWidget>(start, "messageWidget")->text().contains("wrong"));

    child<QLineEdit>(start, "mfaCodeEdit")->setText(FakeAccount::Totp);
    QTest::mouseClick(child<QPushButton>(start, "mfaVerifyButton"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    QCOMPARE(controller->account()->state(), AccountManager::State::Unlocked);
    QCOMPARE(controller->mode(), AccountController::Mode::Account);
    QCOMPARE(config()->get(Config::Account_StartMode).toString(), QString("account"));
    QVERIFY(!m_mainWindow->accountVaultsPane()->isHidden());
    // The device is trusted now: the next sign-in doesn't ask for the Secret Key
    QVERIFY(!AccountManager::storedSecretKey(serverConfig().serverId(), Email).isEmpty());
    m_mainWindow->showStartScreen();
    child<QLineEdit>(start, "emailEdit")->setText(Email);
    QVERIFY(child<QLineEdit>(start, "secretKeyEdit")->isHidden());
}

void TestGuiAccounts::testOtherMfaErrorsReturnToSignIn()
{
    const auto secretKey = registerElsewhere(Email, true);
    createWindow({});
    auto* start = m_mainWindow->startScreen();
    signIn(Email, secretKey);
    QTRY_COMPARE_WITH_TIMEOUT(start->currentPage(), AccountStartWidget::Page::Mfa, 10000);

    // The server dropped the MFA token (expired, or five wrong codes): that's a plain 401
    m_server->mfaTokens.clear();
    child<QLineEdit>(start, "mfaCodeEdit")->setText(FakeAccount::Totp);
    QTest::mouseClick(child<QPushButton>(start, "mfaVerifyButton"), Qt::LeftButton);
    QTRY_COMPARE(start->currentPage(), AccountStartWidget::Page::SignIn);
    QCOMPARE(m_mainWindow->accountController()->account()->state(), AccountManager::State::SignedOut);
    QVERIFY(m_mainWindow->isStartScreenShown());
    QVERIFY(child<QLineEdit>(start, "passwordEdit")->text().isEmpty());

    // A wrong password stays on the sign-in page too
    child<QLineEdit>(start, "passwordEdit")->setText("wrong");
    child<QLineEdit>(start, "secretKeyEdit")->setText(secretKey);
    QTest::mouseClick(child<QPushButton>(start, "signInButton"), Qt::LeftButton);
    QTRY_VERIFY(child<MessageWidget>(start, "messageWidget")->text().contains("wrong"));
    QCOMPARE(start->currentPage(), AccountStartWidget::Page::SignIn);
}

void TestGuiAccounts::testPairsWithASelfHostedServer()
{
    createWindow({});
    auto* start = m_mainWindow->startScreen();
    QTest::mouseClick(child<QPushButton>(start, "selfHostedButton"), Qt::LeftButton);
    QCOMPARE(start->currentPage(), AccountStartWidget::Page::SelfHosted);

    // A wrong code is refused and nothing is remembered
    child<QLineEdit>(start, "selfHostedUrlEdit")->setText(SelfHostedUrl);
    child<QLineEdit>(start, "pairingCodeEdit")->setText("NOPE1234");
    QTest::mouseClick(child<QPushButton>(start, "pairButton"), Qt::LeftButton);
    QTRY_VERIFY(child<MessageWidget>(start, "messageWidget")->text().contains("pair"));
    QVERIFY(m_mainWindow->isStartScreenShown());
    QCOMPARE(m_mainWindow->accountController()->mode(), AccountController::Mode::Unset);

    // Plain http works too, e.g. on a home network
    const QString httpUrl = "http://sync.home.example:8787";
    const auto code = m_server->addPairingCode();
    const auto token = m_server->pairingCodes.value(code);
    child<QLineEdit>(start, "selfHostedUrlEdit")->setText(httpUrl);
    child<QLineEdit>(start, "pairingCodeEdit")->setText(code);
    QTest::mouseClick(child<QPushButton>(start, "pairButton"), Qt::LeftButton);
    QTRY_VERIFY(!m_mainWindow->isStartScreenShown());
    QCOMPARE(m_mainWindow->accountController()->mode(), AccountController::Mode::SelfHosted);
    QVERIFY(m_server->requests.contains("sync GET health"));
    QVERIFY(m_server->requests.contains("sync POST devices/pairing/claim"));
    QVERIFY(m_mainWindow->accountVaultsPane()->isHidden());
    QString url, deviceToken;
    QVERIFY(AccountController::selfHostedServer(&url, &deviceToken));
    QCOMPARE(url, httpUrl);
    QCOMPARE(deviceToken, token);

    // New Remote Sync settings start with this server and this device's token
    auto db = QSharedPointer<Database>::create();
    DatabaseSettingsWidgetRemote remote;
    remote.loadSettings(db);
    child<QComboBox>(&remote, "typeComboBox")->setCurrentIndex(1);
    QCOMPARE(child<QLineEdit>(&remote, "serverUrlEdit")->text(), httpUrl);
    QCOMPARE(child<QLineEdit>(&remote, "serverTokenEdit")->text(), token);
    QCOMPARE(child<QLineEdit>(&remote, "serverDatabaseIdEdit")->text(),
             db->rootGroup()->uuid().toString(QUuid::WithoutBraces));
}

void TestGuiAccounts::testSignUpShowsTheSecretKeyOnce()
{
    createWindow({});
    QTest::mouseClick(child<QPushButton>(m_mainWindow->startScreen(), "createAccountButton"), Qt::LeftButton);
    SignUpWizard* wizard = nullptr;
    QTRY_VERIFY(wizard = m_mainWindow->findChild<SignUpWizard*>());
    child<QLineEdit>(wizard, "emailEdit")->setText(Email);
    child<QLineEdit>(wizard, "passwordEdit")->setText(Password);
    child<QLineEdit>(wizard, "confirmEdit")->setText("something else");
    auto* next = child<QPushButton>(wizard, "signUpNext");
    QTest::mouseClick(next, Qt::LeftButton);
    QVERIFY(child<MessageWidget>(wizard, "messageWidget")->text().contains("match"));
    QVERIFY(m_server->users.isEmpty());

    child<QLineEdit>(wizard, "confirmEdit")->setText(Password);
    QTest::mouseClick(next, Qt::LeftButton);
    auto* pages = child<QStackedWidget>(wizard, "pages");
    QTRY_COMPARE_WITH_TIMEOUT(pages->currentIndex(), 1, 10000);
    const auto secretKey = wizard->secretKey();
    QVERIFY(secretKey.startsWith("K1-"));
    QCOMPARE(child<QLabel>(wizard, "secretKeyLabel")->text(), secretKey);
    QVERIFY(m_server->users.contains(Email));
    // Finishing waits for the kit
    QVERIFY(!next->isEnabled());

    QTemporaryDir dir;
    const auto kitPath = dir.filePath("kit.pdf");
    fileDialog()->setNextFileName(kitPath);
    QTest::mouseClick(child<QPushButton>(wizard, "saveKitButton"), Qt::LeftButton);
    QFile kit(kitPath);
    QVERIFY(kit.open(QIODevice::ReadOnly));
    QVERIFY(kit.read(5) == "%PDF-");
    QVERIFY(EmergencyKit(Email, Issuer, secretKey).html().contains(secretKey));
    QVERIFY(child<QCheckBox>(wizard, "savedCheckBox")->isChecked());
    QVERIFY(next->isEnabled());

    QPointer<SignUpWizard> guard(wizard);
    QTest::mouseClick(next, Qt::LeftButton);
    QTRY_VERIFY(!guard || guard->isHidden());
    // Shown once: gone from the wizard, kept only in this device's keychain
    if (guard) {
        QVERIFY(guard->secretKey().isEmpty());
        QVERIFY(child<QLabel>(guard, "secretKeyLabel")->text().isEmpty());
    }
    QCOMPARE(AccountManager::storedSecretKey(serverConfig().serverId(), Email), secretKey);
    QVERIFY(!m_mainWindow->isStartScreenShown());
    QCOMPARE(m_mainWindow->accountController()->mode(), AccountController::Mode::Account);
    QCOMPARE(m_mainWindow->accountController()->account()->state(), AccountManager::State::Unlocked);
}

void TestGuiAccounts::testUnlockOpensEveryVaultAndLockClearsKeys()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* account = controller->account();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");

    for (const auto& name : {QString("Personal"), QString("Work")}) {
        const auto before = tabs->count();
        controller->createVault(name);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), before + 1, 10000);
    }
    QTRY_COMPARE(account->keyring().vaults().size(), 2);
    auto* pane = m_mainWindow->accountVaultsPane();
    QTRY_COMPARE(pane->vaultList()->count(), 2);
    QCOMPARE(pane->vaultList()->item(0)->text(), QString("Personal"));
    // Vault copies are kept out of the recent databases
    QVERIFY(config()->get(Config::LastDatabases).toStringList().isEmpty());

    // Locking closes the vaults and wipes the keys
    QTest::mouseClick(child<QPushButton>(pane, "accountVaultsLock"), Qt::LeftButton);
    QCOMPARE(account->state(), AccountManager::State::Locked);
    QTRY_COMPARE(tabs->count(), 0);
    QVERIFY(account->keyring().isEmpty());
    QVERIFY(account->keyset().isNull());
    QCOMPARE(pane->vaultList()->count(), 0);
    QVERIFY(!child<QWidget>(pane, "accountVaultsPassword")->isHidden());

    // One password unlocks the account and opens every vault, with no per-vault password
    const auto vaultRequests = m_server->requests.filter("sync GET vaults/").size();
    child<QLineEdit>(pane, "accountVaultsPassword")->setText(Password);
    QTest::mouseClick(child<QPushButton>(pane, "accountVaultsUnlock"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(account->state(), AccountManager::State::Unlocked, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 10000);
    for (int i = 0; i < tabs->count(); ++i) {
        auto* widget = tabs->databaseWidgetFromIndex(i);
        QVERIFY(!widget->isLocked());
        QVERIFY(controller->isAccountVault(widget));
    }
    // Both opened from their local copies
    QCOMPARE(m_server->requests.filter("sync GET vaults/").size(), vaultRequests);

    // Locking one vault (e.g. the inactivity timer) locks the account
    tabs->databaseWidgetFromIndex(0)->lock();
    QTRY_COMPARE(account->state(), AccountManager::State::Locked);
    QTRY_COMPARE(tabs->count(), 0);

    // A vault this device hasn't seen yet is downloaded once
    for (const auto& vault : m_server->vaults) {
        QFile::remove(AccountController::vaultDirectory(account->accountId()) + "/" + vault.id + ".kdbx");
    }
    controller->unlockAccount(Password);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 10000);
    QCOMPARE(m_server->requests.filter("sync GET vaults/").size(), vaultRequests + 2);
}

void TestGuiAccounts::testShareDialogRevokeRotatesTheKey()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    controller->createVault("Family");
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
    QTRY_COMPARE(controller->account()->keyring().vaults().size(), 1);
    const auto vaultId = controller->account()->keyring().vaults().first().id;
    QString bob;
    const auto bobKeys = m_server->addUser("bob@example.com", &bob);

    m_mainWindow->showShareVaultDialog(vaultId);
    ShareVaultDialog* dialog = nullptr;
    QTRY_VERIFY(dialog = m_mainWindow->findChild<ShareVaultDialog*>());
    auto* members = child<QTreeWidget>(dialog, "membersTree");
    QTRY_COMPARE(members->topLevelItemCount(), 1);
    QVERIFY(members->topLevelItem(0)->text(0).contains("(you)"));
    QCOMPARE(members->topLevelItem(0)->text(2), AccountCrypto::fingerprint(controller->account()->keyset().publicKeys));

    // Look the recipient up, compare fingerprints, share
    child<QLineEdit>(dialog, "emailEdit")->setText("bob@example.com");
    auto* share = child<QPushButton>(dialog, "shareButton");
    QVERIFY(!share->isEnabled());
    QTest::mouseClick(child<QPushButton>(dialog, "lookupButton"), Qt::LeftButton);
    QTRY_VERIFY(share->isEnabled());
    QVERIFY(child<QLabel>(dialog, "recipientLabel")->text().contains(AccountCrypto::fingerprint(bobKeys.publicKeys)));
    child<QComboBox>(dialog, "addRoleCombo")->setCurrentIndex(1);
    QTest::mouseClick(share, Qt::LeftButton);
    QTRY_COMPARE(members->topLevelItemCount(), 2);
    QCOMPARE(m_server->roleOf(vaultId, bob), QString("write"));
    QCOMPARE(members->topLevelItem(1)->text(2), AccountCrypto::fingerprint(bobKeys.publicKeys));

    // Removing Bob rotates the key in the same step
    members->topLevelItem(1)->setSelected(true);
    MessageBox::setNextAnswer(MessageBox::Remove);
    QTest::mouseClick(child<QPushButton>(dialog, "removeButton"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->isBusy() && members->topLevelItemCount() == 1, 10000);
    QVERIFY(m_server->roleOf(vaultId, bob).isEmpty());
    QCOMPARE(m_server->vaults.value(vaultId).current, 2);
    const auto newKey = controller->account()->keyring().vaultKey(vaultId);
    QVERIFY(newKey != m_server->vaultKeyFor(vaultId, bob, bobKeys.encSk, 1));
    QVERIFY(VaultKeyring::readVault(m_server->blobs.value(vaultId).data, VaultKeyring::compositeKeyFor(newKey)));

    // The open vault follows the new key and keeps syncing
    auto* widget = controller->vaultWidget(vaultId);
    QVERIFY(widget);
    QTRY_VERIFY(widget->database()->key()->rawKey() == VaultKeyring::compositeKeyFor(newKey)->rawKey());
    addEntry(widget->database().data(), "After rotation");
    QVERIFY(widget->save());
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            auto db =
                VaultKeyring::readVault(m_server->blobs.value(vaultId).data, VaultKeyring::compositeKeyFor(newKey));
            return db && titles(db.data()).contains("After rotation");
        }(),
        10000);
    dialog->close();
}

void TestGuiAccounts::testMovesADatabaseToTheAccount()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");

    QTemporaryDir dir;
    const auto path = dir.filePath("local.kdbx");
    {
        auto db = QSharedPointer<Database>::create();
        auto kdf = QSharedPointer<AesKdf>::create();
        kdf->setRounds(1);
        db->setKdf(kdf);
        auto key = QSharedPointer<CompositeKey>::create();
        key->addKey(QSharedPointer<PasswordKey>::create("a"));
        db->setKey(key);
        addEntry(db.data(), "Bank");
        QVERIFY(db->saveAs(path));
    }
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto originalBytes = original.readAll();
    original.close();
    m_mainWindow->openDatabase(path, "a");
    QTRY_COMPARE(tabs->count(), 1);
    QTRY_VERIFY(!tabs->currentDatabaseWidget()->isLocked());

    // Closing the database's tab asks about unsaved changes the way it always does; keep the file as it was
    MessageBox::setNextAnswer(MessageBox::Discard);
    QSignalSpy moved(controller, &AccountController::moveFinished);
    controller->moveToAccount(tabs->currentDatabaseWidget(), "Banking");
    QVERIFY(moved.wait(10000));
    QVERIFY2(moved.first().at(0).toBool(), qPrintable(moved.first().at(1).toString()));

    // The vault replaced the database's tab
    QTRY_COMPARE(tabs->count(), 1);
    auto* vaultWidget = tabs->currentDatabaseWidget();
    QVERIFY(controller->isAccountVault(vaultWidget));
    QCOMPARE(titles(vaultWidget->database().data()), QStringList{"Bank"});
    const auto vaultId = controller->vaultIdOf(vaultWidget);
    QCOMPARE(m_server->vaults.value(vaultId).current, 1);
    QCOMPARE(m_server->vaults.value(vaultId).access.first().principal, controller->account()->principal());
    QVERIFY(VaultKeyring::readVault(m_server->blobs.value(vaultId).data,
                                    controller->account()->keyring().compositeKey(vaultId)));
    // The original file is unchanged
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);
}

void TestGuiAccounts::testServiceAccountsDialog()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    controller->createVault("Deploy keys");
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
    QTRY_COMPARE(controller->account()->keyring().vaults().size(), 1);
    const auto vaultId = controller->account()->keyring().vaults().first().id;
    const auto orgId = m_server->addOrg("Acme", Email);

    m_mainWindow->showServiceAccounts();
    ServiceAccountsDialog* dialog = nullptr;
    QTRY_VERIFY(dialog = m_mainWindow->findChild<ServiceAccountsDialog*>());
    QTRY_COMPARE(dialog->currentOrg(), orgId);

    dialog->createServiceAccount("CI deploy");
    QTRY_VERIFY(dialog->tokenDialog());
    auto* tokenField = child<QLineEdit>(dialog->tokenDialog(), "serviceAccountToken");
    const auto token = tokenField->text();
    QVERIFY(token.startsWith("ov_sa_"));
    QTRY_COMPARE(dialog->currentServiceAccount().isEmpty(), false);
    const auto saId = dialog->currentServiceAccount();
    // Shown once: closing the window forgets it
    dialog->tokenDialog()->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close)->click();
    QTRY_VERIFY(!dialog->tokenDialog());

    // Grant the vault read-only
    QTRY_VERIFY(!dialog->isBusy());
    child<QComboBox>(dialog, "vaultCombo")->setCurrentIndex(0);
    child<QComboBox>(dialog, "vaultRoleCombo")->setCurrentIndex(0);
    dialog->grantSelectedVault();
    QTRY_COMPARE(m_server->roleOf(vaultId, "sa:" + saId), QString("read"));

    // Another secret, with an expiry
    const auto expires = QDateTime::currentDateTimeUtc().addDays(30);
    dialog->issueSecret("staging", expires);
    QTRY_VERIFY(dialog->tokenDialog());
    QVERIFY(child<QLineEdit>(dialog->tokenDialog(), "serviceAccountToken")->text() != token);
    QTRY_COMPARE(child<QTreeWidget>(dialog, "secretsTree")->topLevelItemCount(), 2);
    dialog->tokenDialog()->close();

    // Revoke the first one
    auto* secrets = child<QTreeWidget>(dialog, "secretsTree");
    for (int i = 0; i < secrets->topLevelItemCount(); ++i) {
        if (secrets->topLevelItem(i)->text(0) != "staging") {
            secrets->topLevelItem(i)->setSelected(true);
        }
    }
    MessageBox::setNextAnswer(MessageBox::Remove);
    dialog->revokeSelectedSecret();
    QTRY_COMPARE(secrets->topLevelItemCount(), 1);
    QCOMPARE(m_server->secrets.size(), 1);

    // Change the remaining secret's expiry from the dialog
    QCOMPARE(secrets->topLevelItem(0)->text(0), QString("staging"));
    secrets->topLevelItem(0)->setSelected(true);
    QTRY_VERIFY(child<QPushButton>(dialog, "secretExpiryButton")->isEnabled());
    const auto later = QDateTime::currentDateTimeUtc().addDays(60);
    dialog->setSelectedSecretExpiry(later);
    QTRY_COMPARE(m_server->secrets.constBegin()->expiresAt.toString(), later.toString(Qt::ISODate));
    QTRY_VERIFY(!dialog->isBusy());
    secrets->topLevelItem(0)->setSelected(true);
    dialog->setSelectedSecretExpiry({});
    QTRY_VERIFY(m_server->secrets.constBegin()->expiresAt.isNull());
    QTRY_COMPARE(secrets->topLevelItem(0)->text(3), QString("never"));
    QTRY_VERIFY(!dialog->isBusy());

    // Expire the whole service account
    child<QCheckBox>(dialog, "neverExpiresCheckBox")->setChecked(false);
    dialog->applyExpiry();
    QTRY_VERIFY(m_server->serviceAccounts.value(saId).json.value("expires_at").isString());

    // The audit log shows what happened, newest first
    auto* audit = child<QTreeWidget>(dialog, "auditTree");
    QTRY_VERIFY(audit->topLevelItemCount() >= 4);
    QCOMPARE(audit->topLevelItem(0)->text(2), QString("sa.secret.update"));

    // Confirm an accepted member with the vault picked for them
    QString bob;
    m_server->addUser("bob@example.com", &bob);
    m_server->addOrgMember(orgId, "bob@example.com", "member", "accepted");
    m_server->vaults[vaultId].orgId = orgId;
    QSignalSpy reloaded(controller->account(), &AccountManager::vaultsChanged);
    controller->account()->reloadVaults();
    QVERIFY(reloaded.wait());
    dialog->loadMembers();
    QTRY_COMPARE(child<QTreeWidget>(dialog, "membersTree")->topLevelItemCount(), 2);
    dialog->confirmMember(bob.mid(5), {{vaultId, "write"}});
    QTRY_COMPARE(m_server->roleOf(vaultId, bob), QString("write"));
    dialog->close();
}

void TestGuiAccounts::testSignOutReturnsToTheStartScreen()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    controller->createVault("Personal");
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);

    // A restart picks the session up locked from the keychain, without touching the network
    controller->lockAccount();
    QTRY_COMPARE(tabs->count(), 0);
    {
        m_server->requests.clear();
        DatabaseTabWidget otherTabs;
        AccountController nextLaunch(&otherTabs);
        QVERIFY(!nextLaunch.needsStartScreen() || nextLaunch.account()->state() == AccountManager::State::SignedOut);
        nextLaunch.restore();
        QCOMPARE(nextLaunch.account()->state(), AccountManager::State::Locked);
        QVERIFY(!nextLaunch.needsStartScreen());
        QVERIFY(m_server->requests.isEmpty());
        nextLaunch.unlockAccount(Password);
        QTRY_COMPARE_WITH_TIMEOUT(otherTabs.count(), 1, 10000);
        nextLaunch.lockAccount();
        // The session this launch picked up is the same one, so don't end it on the server
        SyncServerClient::setAccountTokenProvider(controller->account()->accountId(),
                                                  controller->account()->tokenProvider());
    }
    controller->unlockAccount(Password);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);

    QTest::mouseClick(child<QPushButton>(m_mainWindow->accountVaultsPane(), "accountVaultsSignOut"), Qt::LeftButton);
    QCOMPARE(controller->account()->state(), AccountManager::State::SignedOut);
    QTRY_COMPARE(tabs->count(), 0);
    QVERIFY(m_mainWindow->isStartScreenShown());
    QCOMPARE(controller->mode(), AccountController::Mode::Unset);
    QVERIFY(m_mainWindow->accountVaultsPane()->isHidden());
}

namespace
{
    // The vaults and headings the sidebar shows, in order
    QStringList sidebarRows(AccountVaultsPane* pane)
    {
        QStringList rows;
        for (int i = 0; i < pane->vaultList()->count(); ++i) {
            rows << pane->vaultList()->item(i)->text();
        }
        return rows;
    }

    ServiceAccountsDialog* openOrgDialog(MainWindow* window)
    {
        window->showServiceAccounts();
        ServiceAccountsDialog* dialog = nullptr;
        std::ignore = QTest::qWaitFor([&] { return (dialog = window->findChild<ServiceAccountsDialog*>()); }, 5000);
        return dialog;
    }

    QTreeWidgetItem* row(QTreeWidget* tree, const QString& text)
    {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (tree->topLevelItem(i)->text(0) == text) {
                return tree->topLevelItem(i);
            }
        }
        return nullptr;
    }
} // namespace

void TestGuiAccounts::testCreatesAnOrgAndOrgVaults()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* account = controller->account();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    controller->createVault("Mine");
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);

    // Create the org from the admin dialog; it is selected right away
    auto* dialog = openOrgDialog(m_mainWindow.data());
    QVERIFY(dialog);
    QTRY_VERIFY(!dialog->isBusy());
    QVERIFY(child<QPushButton>(dialog, "newOrgButton")->isEnabled());
    dialog->createOrg("Acme");
    QTRY_COMPARE(m_server->orgs.size(), 1);
    const auto orgId = m_server->orgs.constBegin()->id;
    QTRY_COMPARE(dialog->currentOrg(), orgId);
    QCOMPARE(dialog->currentRole(), QString("owner"));
    QTRY_COMPARE(child<QTreeWidget>(dialog, "membersTree")->topLevelItemCount(), 1);
    dialog->close();
    QTRY_VERIFY(account->org(orgId).canAdminister());

    // New vault: the dialog offers the org, and the vault is sealed to the org's other admin too
    QString carol;
    const auto carolKeys = m_server->addUser("carol@example.com", &carol);
    m_server->addOrgMember(orgId, "carol@example.com", "admin");
    QTimer::singleShot(0, this, [this] {
        auto* newVault = m_mainWindow->findChild<QDialog*>("newAccountVaultDialog");
        QVERIFY(newVault);
        child<QLineEdit>(newVault, "newVaultNameEdit")->setText("Deploy");
        auto* owner = child<QComboBox>(newVault, "newVaultOwnerCombo");
        QCOMPARE(owner->count(), 2);
        owner->setCurrentIndex(owner->findData(m_server->orgs.constBegin()->id));
        newVault->accept();
    });
    m_mainWindow->createAccountVault();
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 2, 10000);
    QTRY_COMPARE(account->keyring().vaults().size(), 2);
    QString vaultId;
    for (const auto& vault : m_server->vaults) {
        if (vault.orgId == orgId) {
            vaultId = vault.id;
        }
    }
    QVERIFY(!vaultId.isEmpty());
    QCOMPARE(m_server->roleOf(vaultId, carol), QString("manage"));
    QVERIFY(m_server->vaultKeyFor(vaultId, carol, carolKeys.encSk) == account->keyring().vaultKey(vaultId));

    // The sidebar groups the vaults: personal ones first, then each org's under its name
    auto* pane = m_mainWindow->accountVaultsPane();
    QTRY_COMPARE(sidebarRows(pane), QStringList({"Personal", "Mine", "Acme", "Deploy"}));
    QVERIFY(!(pane->vaultList()->item(2)->flags() & Qt::ItemIsSelectable));
    QCOMPARE(pane->vaultList()->item(3)->data(AccountVaultsPane::OrgIdRole).toString(), orgId);

    // A confirmed admin the client can't seal to: the server answers 400, nothing is created
    QString erin;
    m_server->addUser("erin@example.com", &erin);
    m_server->directory.remove(erin);
    m_server->addOrgMember(orgId, "erin@example.com", "admin");
    QSignalSpy messages(controller, &AccountController::message);
    controller->createVault("Finance", orgId);
    QTRY_VERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(1).value<MessageWidget::MessageType>(), MessageWidget::Error);
    QVERIFY2(messages.last().at(0).toString().contains("missing: " + erin),
             qPrintable(messages.last().at(0).toString()));
    QCOMPARE(m_server->vaults.size(), 2);
    QCOMPARE(tabs->count(), 2);
}

void TestGuiAccounts::testAcceptsAnInvitation()
{
    // Olivia owns an org on another device and invites Alice by email
    QScopedPointer<AccountManager> olivia(createManager(m_server));
    olivia->setServer(serverConfig());
    olivia->registerAccount("olivia@example.com", Password);
    QTRY_COMPARE_WITH_TIMEOUT(olivia->state(), AccountManager::State::Unlocked, 10000);
    const auto orgId = m_server->addOrg("Acme", "olivia@example.com");
    VaultSharing oliviaSharing(olivia.data());
    QString vaultId;
    oliviaSharing.createVault("Deploy", orgId, [&](const QString& id, const QSharedPointer<Database>&, const QString&) {
        vaultId = id;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!vaultId.isEmpty() && olivia->keyring().contains(vaultId), 10000);
    ServiceAccountAdmin oliviaAdmin(olivia.data());
    bool invited = false;
    oliviaAdmin.invite(orgId, Email, "member", [&](bool ok, const QString&) { invited = ok; });
    QTRY_VERIFY(invited);
    const auto link = m_server->sentInvitations.last().second;

    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* account = controller->account();
    auto* pane = m_mainWindow->accountVaultsPane();

    // A pasted code that isn't one is refused before anything is sent
    QSignalSpy messages(controller, &AccountController::message);
    controller->acceptInvitation("https://app.openvireo.example/invitations");
    QTRY_COMPARE(messages.size(), 1);
    QCOMPARE(messages.last().at(1).value<MessageWidget::MessageType>(), MessageWidget::Error);
    QVERIFY(m_server->requests.filter("invitations/accept").isEmpty());

    // The link from the email, pasted into the Accept Invitation box
    QTimer::singleShot(0, this, [this, link] {
        auto* input = m_mainWindow->findChild<QInputDialog*>();
        QVERIFY(input);
        input->setTextValue(link);
        input->accept();
    });
    m_mainWindow->acceptAccountInvitation();
    QTRY_COMPARE(messages.size(), 2);
    QCOMPARE(messages.last().at(1).value<MessageWidget::MessageType>(), MessageWidget::Positive);
    QCOMPARE(m_server->orgMember(orgId, account->accountId())->status, QString("accepted"));
    // Alice waits for an admin: the org shows, without vaults
    QTRY_COMPARE(sidebarRows(pane), QStringList({"Acme (waiting for confirmation)"}));
    QVERIFY(!account->keyring().contains(vaultId));

    // Olivia confirms her with the vault; the access event makes this device reload
    ServiceAccountAdmin::Member alice;
    bool listed = false;
    oliviaAdmin.listMembers(orgId, [&](const QList<ServiceAccountAdmin::Member>& members, const QString&) {
        for (const auto& member : members) {
            if (member.email == Email) {
                alice = member;
            }
        }
        listed = true;
    });
    QTRY_VERIFY(listed);
    bool confirmed = false;
    oliviaAdmin.confirmMember(orgId, alice, {{vaultId, "write"}}, [&](bool ok, const QString&) { confirmed = ok; });
    QTRY_VERIFY(confirmed);
    account->handleAccessEvent({});
    QTRY_VERIFY_WITH_TIMEOUT(account->keyring().contains(vaultId), 10000);
    QTRY_COMPARE(sidebarRows(pane), QStringList({"Acme", "Deploy"}));
}

void TestGuiAccounts::testInvitesPromotesAndRemovesMembers()
{
    const auto secretKey = registerElsewhere(Email);
    createWindow({});
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* controller = m_mainWindow->accountController();
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    const auto orgId = m_server->addOrg("Acme", Email);
    controller->account()->reloadVaults();
    QTRY_VERIFY(controller->account()->org(orgId).canAdminister());
    controller->createVault("Deploy", orgId);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
    QTRY_COMPARE(controller->account()->keyring().vaults().size(), 1);
    const auto vaultId = controller->account()->keyring().vaults().first().id;
    QString bob;
    const auto bobKeys = m_server->addUser("bob@example.com", &bob);
    m_server->addOrgMember(orgId, "bob@example.com", "member", "accepted");

    auto* dialog = openOrgDialog(m_mainWindow.data());
    QVERIFY(dialog);
    QTRY_COMPARE(dialog->currentOrg(), orgId);
    auto* members = child<QTreeWidget>(dialog, "membersTree");
    auto* invitations = child<QTreeWidget>(dialog, "invitationsTree");
    QTRY_COMPARE(members->topLevelItemCount(), 2);

    // Invite by email, with a role; the pending invitation shows and can be cancelled
    QVERIFY(child<QPushButton>(dialog, "inviteButton")->isEnabled());
    dialog->invite("carol@example.com", "admin");
    QTRY_COMPARE(invitations->topLevelItemCount(), 1);
    QCOMPARE(invitations->topLevelItem(0)->text(1), QString("Admin"));
    QCOMPARE(m_server->sentInvitations.last().first, QString("carol@example.com"));
    QTRY_VERIFY(!dialog->isBusy());
    invitations->topLevelItem(0)->setSelected(true);
    QVERIFY(child<QPushButton>(dialog, "cancelInvitationButton")->isEnabled());
    dialog->cancelSelectedInvitation();
    QTRY_COMPARE(invitations->topLevelItemCount(), 0);
    QVERIFY(m_server->invitations.isEmpty());

    // Confirm Bob with the vault, then promote him: he gets manage on the org vaults
    QTRY_VERIFY(!dialog->isBusy());
    dialog->confirmMember(bob.mid(5), {{vaultId, "read"}});
    QTRY_COMPARE(m_server->roleOf(vaultId, bob), QString("read"));
    QTRY_VERIFY(!dialog->isBusy());
    QTRY_COMPARE(row(members, "bob@example.com")->text(2), QString("confirmed"));
    row(members, "bob@example.com")->setSelected(true);
    QVERIFY(child<QPushButton>(dialog, "changeRoleButton")->isEnabled());
    dialog->setMemberRole(bob.mid(5), "admin");
    QTRY_COMPARE(m_server->orgMember(orgId, bob.mid(5))->role, QString("admin"));
    QTRY_COMPARE(m_server->roleOf(vaultId, bob), QString("manage"));
    QTRY_COMPARE(row(members, "bob@example.com")->text(1), QString("Admin"));
    QTRY_VERIFY(!dialog->isBusy());
    dialog->setMemberRole(bob.mid(5), "member");
    QTRY_COMPARE(m_server->orgMember(orgId, bob.mid(5))->role, QString("member"));
    QTRY_VERIFY(!dialog->isBusy());

    // Removing him rotates the vault he could open
    const auto oldKey = controller->account()->keyring().vaultKey(vaultId);
    QTRY_VERIFY(row(members, "bob@example.com"));
    row(members, "bob@example.com")->setSelected(true);
    MessageBox::setNextAnswer(MessageBox::Remove);
    QTest::mouseClick(child<QPushButton>(dialog, "removeMemberButton"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->isBusy() && members->topLevelItemCount() == 1, 10000);
    QVERIFY(!m_server->orgMember(orgId, bob.mid(5)));
    QCOMPARE(m_server->vaults.value(vaultId).current, 2);
    QTRY_VERIFY(controller->account()->keyring().vaultKey(vaultId) != oldKey);
    QVERIFY(m_server->vaultKeyFor(vaultId, bob, bobKeys.encSk, 2).empty());
    // The open vault follows the new key
    auto* widget = controller->vaultWidget(vaultId);
    QVERIFY(widget);
    QTRY_VERIFY(widget->database()->key()->rawKey()
                == VaultKeyring::compositeKeyFor(controller->account()->keyring().vaultKey(vaultId))->rawKey());
    // Only the owner is left; the last owner can't leave
    members->topLevelItem(0)->setSelected(true);
    QCOMPARE(child<QPushButton>(dialog, "removeMemberButton")->text(), QString("Leave…"));
    QVERIFY(!child<QPushButton>(dialog, "changeRoleButton")->isEnabled());
    auto* audit = child<QTreeWidget>(dialog, "auditTree");
    QStringList actions;
    QTRY_VERIFY([&] {
        actions.clear();
        for (int i = 0; i < audit->topLevelItemCount(); ++i) {
            actions << audit->topLevelItem(i)->text(2);
        }
        return actions.contains("org.member.remove");
    }());
    QVERIFY(actions.contains("org.member.role"));
    QVERIFY(actions.contains("org.invitation.cancel"));
    dialog->close();
}

namespace
{
    // Stands in for Touch ID, Windows Hello and Polkit: keeps the keys in memory, scrambled
    class MockQuickUnlock : public QuickUnlockInterface
    {
    public:
        bool available = true;
        // What the user does at the prompt
        enum class Answer
        {
            Authenticate,
            Cancel,
            Fail
        } answer = Answer::Authenticate;
        int prompts = 0;
        QHash<QUuid, QByteArray> keys;

        bool isAvailable() const override
        {
            return available;
        }
        QString errorString() const override
        {
            return QStringLiteral("the user could not be authenticated");
        }
        bool setKey(const QUuid& uuid, const QByteArray& key) override
        {
            keys.insert(uuid, scramble(key));
            return true;
        }
        bool getKey(const QUuid& uuid, QByteArray& key) override
        {
            ++prompts;
            key.clear();
            if (answer == Answer::Fail || !keys.contains(uuid)) {
                return false;
            }
            if (answer == Answer::Authenticate) {
                key = scramble(keys.value(uuid));
            }
            // A cancelled prompt succeeds with no key, as TouchID does
            return true;
        }
        bool hasKey(const QUuid& uuid) const override
        {
            return keys.contains(uuid);
        }
        void reset(const QUuid& uuid) override
        {
            keys.remove(uuid);
        }
        void reset() override
        {
            keys.clear();
        }

    private:
        static QByteArray scramble(QByteArray data)
        {
            for (auto& byte : data) {
                byte = static_cast<char>(byte ^ 0x5a);
            }
            return data;
        }
    };
    // The keychain, keeping every value ever saved so a test can check what reached it
    class RecordingBackend : public SyncTokenStore::Backend
    {
    public:
        explicit RecordingBackend(QStringList* saved)
            : m_saved(saved)
        {
        }
        QString load(const QString& key) override
        {
            return m_values.value(key);
        }
        bool save(const QString& key, const QString& token) override
        {
            m_values.insert(key, token);
            *m_saved << token;
            return true;
        }
        bool remove(const QString& key) override
        {
            m_values.remove(key);
            return true;
        }
        QString description() const override
        {
            return QStringLiteral("test");
        }

    private:
        QHash<QString, QString> m_values;
        QStringList* m_saved;
    };
} // namespace

void TestGuiAccounts::testQuickUnlocksTheAccount()
{
    MockQuickUnlock quickUnlock;
    const auto server = m_server;
    AccountController::setAccountManagerFactory([server, &quickUnlock] {
        auto* manager = createManager(server);
        manager->setQuickUnlock(&quickUnlock);
        return manager;
    });
    const auto secretKey = registerElsewhere(Email);
    QStringList saved;
    SyncTokenStore::setBackend(std::make_unique<RecordingBackend>(&saved));
    createWindow({});
    auto* controller = m_mainWindow->accountController();
    auto* account = controller->account();
    // The window's manager was made before this test; give it the mock too
    account->setQuickUnlock(&quickUnlock);
    signIn(Email, secretKey);
    QTRY_VERIFY_WITH_TIMEOUT(!m_mainWindow->isStartScreenShown(), 10000);
    auto* tabs = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    controller->createVault("Personal");
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);

    // Signing in with the password handed the AUK (32 bytes) to the provider, never to the keychain
    QCOMPARE(quickUnlock.keys.size(), 1);
    QCOMPARE(quickUnlock.keys.constBegin()->size(), 32);
    const auto auk = [&] {
        auto key = quickUnlock.keys.constBegin().value();
        for (auto& byte : key) {
            byte = static_cast<char>(byte ^ 0x5a);
        }
        return key;
    }();
    QVERIFY(!saved.isEmpty());
    for (const auto& value : saved) {
        const auto bytes = value.toUtf8();
        QVERIFY(!bytes.contains(auk) && !bytes.contains(auk.toHex())
                && !value.contains(AccountCrypto::toBase64Url(auk)) && !bytes.contains(auk.toBase64()));
    }

    // Locked, the sidebar offers Quick Unlock beside the password
    auto* pane = m_mainWindow->accountVaultsPane();
    auto* quickButton = child<QPushButton>(pane, "accountVaultsQuickUnlock");
    QVERIFY(quickButton->isHidden());
    controller->lockAccount();
    QCOMPARE(account->state(), AccountManager::State::Locked);
    QTRY_COMPARE(tabs->count(), 0);
    QVERIFY(account->canQuickUnlock());
    QVERIFY(!quickButton->isHidden());
    QVERIFY(!child<QWidget>(pane, "accountVaultsPassword")->isHidden());

    // Cancelling the prompt leaves the account locked and says nothing
    quickUnlock.answer = MockQuickUnlock::Answer::Cancel;
    QTest::mouseClick(quickButton, Qt::LeftButton);
    QCOMPARE(quickUnlock.prompts, 1);
    QCOMPARE(account->state(), AccountManager::State::Locked);
    QVERIFY(child<QLabel>(pane, "accountVaultsError")->isHidden());

    // A failed prompt shows why
    quickUnlock.answer = MockQuickUnlock::Answer::Fail;
    QTest::mouseClick(quickButton, Qt::LeftButton);
    QCOMPARE(account->state(), AccountManager::State::Locked);
    QTRY_VERIFY(!child<QLabel>(pane, "accountVaultsError")->isHidden());
    QVERIFY(child<QLabel>(pane, "accountVaultsError")->text().contains("Quick Unlock"));

    // Authenticating opens every vault without the master password
    quickUnlock.answer = MockQuickUnlock::Answer::Authenticate;
    QTest::mouseClick(quickButton, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(account->state(), AccountManager::State::Unlocked, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(tabs->count(), 1, 10000);
    QVERIFY(!tabs->databaseWidgetFromIndex(0)->isLocked());
    QVERIFY(quickButton->isHidden());

    // Off in the settings, or without a provider, the button is gone
    controller->lockAccount();
    QTRY_COMPARE(tabs->count(), 0);
    config()->set(Config::Security_QuickUnlock, false);
    QMetaObject::invokeMethod(pane, "refresh");
    QVERIFY(!account->canQuickUnlock());
    QVERIFY(quickButton->isHidden());
    config()->set(Config::Security_QuickUnlock, true);
    quickUnlock.available = false;
    QMetaObject::invokeMethod(pane, "refresh");
    QVERIFY(quickButton->isHidden());
    quickUnlock.available = true;
    QMetaObject::invokeMethod(pane, "refresh");
    QVERIFY(!quickButton->isHidden());

    // A restart starts without it: the provider only keeps it in memory, as for databases
    {
        MockQuickUnlock afterRestart;
        DatabaseTabWidget otherTabs;
        AccountController nextLaunch(&otherTabs);
        nextLaunch.account()->setQuickUnlock(&afterRestart);
        nextLaunch.restore();
        QCOMPARE(nextLaunch.account()->state(), AccountManager::State::Locked);
        QVERIFY(!nextLaunch.account()->canQuickUnlock());
        // Don't end the session the window still uses
        SyncServerClient::setAccountTokenProvider(account->accountId(), account->tokenProvider());
    }

    // When the master password changed elsewhere the old AUK no longer opens the keyset: it is dropped
    auto wrapped = m_server->users[Email].keyset;
    auto changed = wrapped;
    changed["wrapped"] = AccountCrypto::toBase64Url(AccountCrypto::randomSecure(80));
    m_server->users[Email].keyset = changed;
    QTest::mouseClick(quickButton, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(account->state(), AccountManager::State::Locked, 10000);
    QTRY_VERIFY(child<QLabel>(pane, "accountVaultsError")->text().contains("master password"));
    QVERIFY(quickUnlock.keys.isEmpty());
    QTRY_VERIFY(quickButton->isHidden());
    m_server->users[Email].keyset = wrapped;

    // Signing out forgets it as well
    controller->unlockAccount(Password);
    QTRY_COMPARE_WITH_TIMEOUT(account->state(), AccountManager::State::Unlocked, 10000);
    QCOMPARE(quickUnlock.keys.size(), 1);
    controller->signOut();
    QVERIFY(quickUnlock.keys.isEmpty());
    account->setQuickUnlock(nullptr);
    const auto restored = m_server;
    AccountController::setAccountManagerFactory([restored] { return createManager(restored); });
}
