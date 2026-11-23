/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2010 Felix Geyer <debfx@fobos.de>
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

#include "TestGui.h"
#include "gui/Application.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabBar>
#include <QTableWidget>
#include <QTest>

#include "config-keepassx-tests.h"
#include "core/PasswordHealth.h"
#include "core/Tools.h"
#include "crypto/Crypto.h"
#include "gui/ActionCollection.h"
#include "gui/ApplicationSettingsWidget.h"
#include "gui/CategoryListWidget.h"
#include "gui/CloneDialog.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/FileDialog.h"
#include "gui/MessageBox.h"
#include "gui/PasswordGeneratorWidget.h"
#include "gui/PasswordWidget.h"
#include "gui/SearchWidget.h"
#include "gui/ShortcutSettingsPage.h"
#include "gui/TotpDialog.h"
#include "gui/TotpSetupDialog.h"
#include "gui/databasekey/KeyFileEditWidget.h"
#include "gui/databasekey/PasswordEditWidget.h"
#include "gui/dbsettings/DatabaseSettingsDialog.h"
#include "gui/dbsettings/DatabaseSettingsWidgetEncryption.h"
#include "gui/entry/EditEntryWidget.h"
#include "gui/entry/EntryView.h"
#include "gui/group/EditGroupWidget.h"
#include "gui/group/GroupModel.h"
#include "gui/group/GroupView.h"
#include "gui/remote/RemoteHandler.h"
#include "gui/remote/SyncTokenStore.h"
#include "gui/tag/TagsEdit.h"
#include "gui/vault/EntryDetailCard.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultEntryEditCard.h"
#include "gui/vault/VaultImportModal.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultNewDatabaseModal.h"
#include "gui/vault/VaultPanes.h"
#include "gui/vault/VaultSearchHelp.h"
#include "gui/wizard/ImportWizard.h"
#include "keys/FileKey.h"
#include "mock/MockRemoteProcess.h"

#include <components/textfields/AutoSuggestBox.h>

#define TEST_MODAL_NO_WAIT(TEST_CODE)                                                                                  \
    bool dialogFinished = false;                                                                                       \
    QTimer::singleShot(0, [&]() { TEST_CODE dialogFinished = true; })

#define TEST_MODAL(TEST_CODE)                                                                                          \
    TEST_MODAL_NO_WAIT(TEST_CODE);                                                                                     \
    QTRY_VERIFY(dialogFinished)

int main(int argc, char* argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    Application app(argc, argv);
    app.setApplicationName("OpenVireo");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    TestGui tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

void TestGui::initTestCase()
{
    // Never touch the real Keychain or token file
    SyncTokenStore::setBackend(SyncTokenStore::createMemoryBackend());
    QVERIFY(Crypto::init());

    // Create temporary config files, so databases opened by a developer's own build don't reopen here
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());

    QLocale::setDefault(QLocale::c());
    Application::bootstrap();

    // Today's local experience, without the account start screen
    config()->set(Config::Account_StartMode, "local");
    m_mainWindow.reset(new MainWindow());
    m_tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_mainWindow->show();
    m_mainWindow->resize(1024, 768);
}

// Every test starts with resetting config settings and opening the temp database
void TestGui::init()
{
    // Reset config to defaults
    config()->resetToDefaults();
    // Disable autosave so we can test the modified file indicator
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    // Enable the tray icon so we can test hiding/restoring the windowQByteArray
    config()->set(Config::GUI_ShowTrayIcon, true);
    // Disable the update check first time alert
    config()->set(Config::UpdateCheckMessageShown, true);
    // Disable quick unlock
    config()->set(Config::Security_QuickUnlock, false);
    // Disable atomic saves to prevent transient errors on some platforms
    config()->set(Config::UseAtomicSaves, false);
    // Disable showing expired entries on unlock
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock, false);

    // Copy the test database file to the temporary file
    auto origFilePath = QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx");
    QVERIFY(m_dbFile.copyFromFile(origFilePath));

    m_dbFileName = QFileInfo(m_dbFile.fileName()).fileName();
    m_dbFilePath = m_dbFile.fileName();

    // make sure window is activated or focus tests may fail
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    fileDialog()->setNextFileName(m_dbFilePath);
    triggerAction("actionDatabaseOpen");

    QApplication::processEvents();

    m_dbWidget = m_tabWidget->currentDatabaseWidget();
    auto* databaseOpenWidget = m_tabWidget->currentDatabaseWidget()->findChild<QWidget*>("databaseOpenWidget");
    QVERIFY(databaseOpenWidget);
    // editPassword is not QLineEdit anymore but PasswordWidget
    auto* editPassword =
        databaseOpenWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);
    editPassword->setFocus();
    QTRY_VERIFY(editPassword->hasFocus());

    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);

    QTRY_VERIFY(!m_dbWidget->isLocked());
    m_db = m_dbWidget->database();

    QApplication::processEvents();
}

// Every test ends with closing the temp database without saving
void TestGui::cleanup()
{
    if (m_tabWidget->isVisible()) {
        // DO NOT save the database
        m_db->markAsClean();
        MessageBox::setNextAnswer(MessageBox::No);
        triggerAction("actionDatabaseClose");
        QApplication::processEvents();
        MessageBox::setNextAnswer(MessageBox::NoButton);
        delete m_dbWidget;
    }
}

void TestGui::cleanupTestCase()
{
    m_dbFile.remove();
}

void TestGui::testSettingsDefaultTabOrder()
{
    // check application settings default tab order
    triggerAction("actionSettings");
    auto* settingsWidget = m_mainWindow->findChild<ApplicationSettingsWidget*>();
    QVERIFY(settingsWidget->isVisible());
    QCOMPARE(settingsWidget->findChild<CategoryListWidget*>("categoryList")->currentCategory(), 0);
    for (auto* w : settingsWidget->findChildren<QTabWidget*>()) {
        if (w->currentIndex() != 0) {
            QFAIL("Application settings contain QTabWidgets whose default index is not 0");
        }
    }
    QTest::keyClick(settingsWidget, Qt::Key::Key_Escape);

    // check database settings default tab order
    triggerAction("actionDatabaseSettings");
    auto* dbSettingsWidget = m_mainWindow->findChild<DatabaseSettingsDialog*>();
    QVERIFY(dbSettingsWidget->isVisible());
    QCOMPARE(dbSettingsWidget->findChild<CategoryListWidget*>("categoryList")->currentCategory(), 0);
    for (auto* w : dbSettingsWidget->findChildren<QTabWidget*>()) {
        if (w->currentIndex() != 0 && w->objectName() != "encryptionSettingsTabWidget") {
            QFAIL("Database settings contain QTabWidgets whose default index is not 0");
        }
    }
    QTest::keyClick(dbSettingsWidget, Qt::Key::Key_Escape);
}

void TestGui::testCreateDatabase()
{
    // The new database modal waits like the wizard did, so drive it from inside its event loop
    TEST_MODAL_NO_WAIT(
        VaultNewDatabaseModal * modal; QTRY_VERIFY(
            (modal = m_mainWindow->findChild<VaultNewDatabaseModal*>("vaultNewDatabaseModal")) && modal->isOpen());
        auto* next = modal->findChild<QPushButton*>("vaultNewDatabaseContinue");
        QVERIFY(next);

        QTest::keyClicks(modal->findChild<QLineEdit*>("databaseName"), "Test Name");
        QTest::keyClicks(modal->findChild<QLineEdit*>("databaseDescription"), "Test Description");
        QCOMPARE(modal->currentStep(), 0);

        QTest::mouseClick(next, Qt::LeftButton);
        QCOMPARE(modal->currentStep(), 1);

        // Check that basic encryption settings are visible
        auto decryptionTimeSlider = modal->findChild<QSlider*>("decryptionTimeSlider");
        auto algorithmComboBox = modal->findChild<QComboBox*>("algorithmComboBox");
        QTRY_VERIFY(decryptionTimeSlider->isVisible());
        QVERIFY(!algorithmComboBox->isVisible());

        // Set the encryption settings to the advanced view
        auto encryptionSettings = modal->findChild<QTabWidget*>("encryptionSettingsTabWidget");
        auto advancedTab = encryptionSettings->findChild<QWidget*>("advancedTab");
        encryptionSettings->setCurrentWidget(advancedTab);
        QTRY_VERIFY(!decryptionTimeSlider->isVisible());
        QVERIFY(algorithmComboBox->isVisible());

        auto rounds = modal->findChild<QSpinBox*>("transformRoundsSpinBox");
        QVERIFY(rounds);
        QVERIFY(rounds->isVisible());
        QTest::mouseClick(rounds, Qt::MouseButton::LeftButton);
        QTest::keyClick(rounds, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(rounds, "2");
        QTest::keyClick(rounds, Qt::Key_Tab);
        QTest::keyClick(rounds, Qt::Key_Tab);

        auto memory = modal->findChild<QSpinBox*>("memorySpinBox");
        QVERIFY(memory);
        QVERIFY(memory->isVisible());
        QTest::mouseClick(memory, Qt::MouseButton::LeftButton);
        QTest::keyClick(memory, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(memory, "50");
        QTest::keyClick(memory, Qt::Key_Tab);

        auto parallelism = modal->findChild<QSpinBox*>("parallelismSpinBox");
        QVERIFY(parallelism);
        QVERIFY(parallelism->isVisible());
        QTest::mouseClick(parallelism, Qt::MouseButton::LeftButton);
        QTest::keyClick(parallelism, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(parallelism, "1");
        QTest::mouseClick(next, Qt::LeftButton);

        QCOMPARE(modal->currentStep(), 2);

        // enter password
        auto* passwordWidget = modal->findChild<PasswordEditWidget*>();
        QCOMPARE(passwordWidget->visiblePage(), KeyFileEditWidget::Page::Edit);
        auto* passwordEdit =
            passwordWidget->findChild<PasswordWidget*>("enterPasswordEdit")->findChild<QLineEdit*>("passwordEdit");
        auto* passwordRepeatEdit =
            passwordWidget->findChild<PasswordWidget*>("repeatPasswordEdit")->findChild<QLineEdit*>("passwordEdit");
        QTRY_VERIFY(passwordEdit->isVisible());
        passwordEdit->setFocus();
        QTest::keyClicks(passwordEdit, "test");
        QTest::keyClick(passwordEdit, Qt::Key::Key_Tab);
        QTest::keyClicks(passwordRepeatEdit, "test");

        // add key file
        auto* additionalOptionsButton = modal->findChild<QPushButton*>("additionalKeyOptionsToggle");
        auto* keyFileWidget = modal->findChild<KeyFileEditWidget*>();
        QVERIFY(additionalOptionsButton->isVisible());
        QTest::mouseClick(additionalOptionsButton, Qt::MouseButton::LeftButton);
        QTRY_VERIFY(keyFileWidget->isVisible());
        QTRY_VERIFY(!additionalOptionsButton->isVisible());
        QCOMPARE(passwordWidget->visiblePage(), KeyFileEditWidget::Page::Edit);
        QTest::mouseClick(keyFileWidget->findChild<QPushButton*>("addButton"), Qt::MouseButton::LeftButton);
        auto* fileEdit = keyFileWidget->findChild<QLineEdit*>("keyFileLineEdit");
        QTRY_VERIFY(fileEdit);
        QTRY_VERIFY(fileEdit->isVisible());
        fileDialog()->setNextFileName(QString("%1/%2").arg(QString(KEEPASSX_TEST_DATA_DIR), "FileKeyHashed.key"));
        auto* browseButton = keyFileWidget->findChild<QPushButton*>("browseKeyFileButton");
        QTest::mouseClick(browseButton, Qt::LeftButton);
        QCOMPARE(fileEdit->text(), QString("%1/%2").arg(QString(KEEPASSX_TEST_DATA_DIR), "FileKeyHashed.key"));

        // save database to temporary file
        TemporaryFile tmpFile;
        QVERIFY(tmpFile.open());
        tmpFile.close();
        fileDialog()->setNextFileName(tmpFile.fileName());

        // click Continue on the warning due to weak password
        MessageBox::setNextAnswer(MessageBox::ContinueWithWeakPass);
        QCOMPARE(next->text(), QString("Create Database"));
        QTest::mouseClick(next, Qt::LeftButton);

        tmpFile.remove(););

    triggerAction("actionDatabaseNew");

    QCOMPARE(m_tabWidget->count(), 2);

    checkEntryCount(0);

    // there is a new empty db
    m_db = m_tabWidget->currentDatabaseWidget()->database();
    QCOMPARE(m_db->rootGroup()->children().size(), 0);

    // check meta data
    QCOMPARE(m_db->metadata()->name(), QString("Test Name"));
    QCOMPARE(m_db->metadata()->description(), QString("Test Description"));

    // check key and encryption
    QCOMPARE(m_db->key()->keys().size(), 2);
    QCOMPARE(m_db->kdf()->rounds(), 2);
    QCOMPARE(m_db->kdf()->uuid(), KeePass2::KDF_ARGON2D);
    QCOMPARE(m_db->cipher(), KeePass2::CIPHER_AES256);
    auto compositeKey = QSharedPointer<CompositeKey>::create();
    compositeKey->addKey(QSharedPointer<PasswordKey>::create("test"));
    auto fileKey = QSharedPointer<FileKey>::create();
    fileKey->load(QString("%1/%2").arg(QString(KEEPASSX_TEST_DATA_DIR), "FileKeyHashed.key"));
    compositeKey->addKey(fileKey);
    QCOMPARE(m_db->key()->rawKey(), compositeKey->rawKey());

    checkEntryCount(0);

    // Test the switching to other DB tab
    m_tabWidget->setCurrentIndex(0);
    checkEntryCount(1);

    m_tabWidget->setCurrentIndex(1);
    checkEntryCount(0);

    // close the new database
    MessageBox::setNextAnswer(MessageBox::No);
    triggerAction("actionDatabaseClose");

    // Wait for dialog to terminate
    QTRY_VERIFY(dialogFinished);
}

void TestGui::testMergeDatabase()
{
    // It is safe to ignore the warning this line produces
    QSignalSpy dbMergeSpy(m_dbWidget.data(), SIGNAL(databaseMerged(QSharedPointer<Database>)));
    QApplication::processEvents();

    // Set file to merge from
    fileDialog()->setNextFileName(QString(KEEPASSX_TEST_DATA_DIR).append("/MergeDatabase.kdbx"));
    triggerAction("actionDatabaseMerge");

    QWidget* editPasswordMerge;
    QTRY_VERIFY((editPasswordMerge = QApplication::focusWidget()) && editPasswordMerge->objectName() == "passwordEdit");
    QVERIFY(editPasswordMerge->isVisible());

    QTest::keyClicks(editPasswordMerge, "a");
    QTest::keyClick(editPasswordMerge, Qt::Key_Enter);

    // Confirm merge in confirmation dialog
    auto focusedWindow = QApplication::topLevelWindows().last();
    QVERIFY(focusedWindow);
    QTRY_VERIFY(focusedWindow->title().contains("Merge"));

    auto topLevelWidgets = QApplication::topLevelWidgets();
    QWidget* mergeDialog = nullptr;
    for (auto* widget : topLevelWidgets) {
        if (widget && widget->objectName() == "MergeDialog") {
            mergeDialog = widget;
        }
    }

    QVERIFY(mergeDialog);
    QTest::keyClick(mergeDialog, Qt::Key_Enter);

    QTRY_COMPARE(dbMergeSpy.count(), 1);
    QTRY_VERIFY(m_tabWidget->tabText(m_tabWidget->currentIndex()).contains("*"));

    m_db = m_tabWidget->currentDatabaseWidget()->database();

    // there are seven child groups of the root group
    QCOMPARE(m_db->rootGroup()->children().size(), 7);
    // the merged group should contain an entry
    QCOMPARE(m_db->rootGroup()->children().at(6)->entries().size(), 1);
    // the General group contains one entry merged from the other db
    QCOMPARE(m_db->rootGroup()->findChildByName("General")->entries().size(), 1);
}

void TestGui::prepareAndTriggerRemoteSync()
{
    // create remote settings in settings dialog
    triggerAction("actionDatabaseSettings");

    auto dbSettingsDialog = m_mainWindow->findChild<DatabaseSettingsDialog*>("databaseSettingsDialog");
    QVERIFY(dbSettingsDialog);
    dbSettingsDialog->showRemoteSettings();

    auto name = "testCommand";
    auto nameEdit = dbSettingsDialog->findChild<QLineEdit*>("nameLineEdit");
    QVERIFY(nameEdit);
    QVERIFY(nameEdit->isVisible());
    nameEdit->setText(name);

    auto downloadCommandEdit = dbSettingsDialog->findChild<QLineEdit*>("downloadCommand");
    QVERIFY(downloadCommandEdit);
    downloadCommandEdit->setText("sftp user@server:Database.kdbx");

    auto saveSettingsButton = dbSettingsDialog->findChild<QPushButton*>("saveSettingsButton");
    QVERIFY(saveSettingsButton);
    QTest::mouseClick(saveSettingsButton, Qt::LeftButton);

    // Save in the settings modal's footer
    auto* dbSettingsModal = m_mainWindow->findChild<VaultModal*>("vaultDatabaseSettingsModal");
    QVERIFY(dbSettingsModal);
    QVERIFY(dbSettingsModal->isOpen());
    QTest::mouseClick(dbSettingsModal->findChild<QPushButton*>("vaultDatabaseSettingsSave"), Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());

    QTRY_COMPARE(m_dbWidget->getRemoteParams().size(), 1);

    // Show menu to trigger populating with remote sync action
    auto menuRemoteSync = m_mainWindow->findChild<QMenu*>("menuRemoteSync");
    QVERIFY(menuRemoteSync);
    menuRemoteSync->popup({0, 0});
    QApplication::processEvents();
    menuRemoteSync->close();

    // Trigger the remote sync action
    for (const auto remoteAction : menuRemoteSync->actions()) {
        if (remoteAction->text() == name) {
            remoteAction->trigger();
            return;
        }
    }

    // If we get here then something didn't work properly
    QFAIL("Remote sync action not present in menu.");
}

void TestGui::testRemoteSyncDatabaseSameKey()
{
    RemoteHandler::setRemoteProcessFunc([](QObject* parent) {
        return QScopedPointer<RemoteProcess>(
            new MockRemoteProcess(parent, QString(KEEPASSX_TEST_DATA_DIR).append("/SyncDatabase.kdbx")));
    });
    QSignalSpy dbSyncSpy(m_dbWidget.data(), &DatabaseWidget::databaseSyncCompleted);
    prepareAndTriggerRemoteSync();
    QTRY_COMPARE(dbSyncSpy.count(), 1);

    m_db = m_tabWidget->currentDatabaseWidget()->database();

    // there are seven child groups of the root group
    QCOMPARE(m_db->rootGroup()->children().size(), 7);
    // the merged group should contain an entry
    QCOMPARE(m_db->rootGroup()->children().at(6)->entries().size(), 1);
    // the General group contains one entry merged from the other db
    QCOMPARE(m_db->rootGroup()->findChildByName("General")->entries().size(), 1);
}

void TestGui::testRemoteSyncDatabaseRequiresPassword()
{
    RemoteHandler::setRemoteProcessFunc([](QObject* parent) {
        return QScopedPointer<RemoteProcess>(new MockRemoteProcess(
            parent, QString(KEEPASSX_TEST_DATA_DIR).append("/SyncDatabaseDifferentPassword.kdbx")));
    });
    QSignalSpy dbSyncSpy(m_dbWidget.data(), &DatabaseWidget::databaseSyncCompleted);
    prepareAndTriggerRemoteSync();

    // need to process more events as opening with the same key did not work and more events have been fired
    QApplication::processEvents(QEventLoop::WaitForMoreEvents);

    QWidget* editPasswordSync;
    QTRY_VERIFY((editPasswordSync = QApplication::focusWidget()) && editPasswordSync->objectName() == "passwordEdit");
    QVERIFY(editPasswordSync->isVisible());

    QTest::keyClicks(editPasswordSync, "b");
    QTest::keyClick(editPasswordSync, Qt::Key_Enter);

    QTRY_COMPARE(dbSyncSpy.count(), 1);
    m_db = m_tabWidget->currentDatabaseWidget()->database();

    // there are seven child groups of the root group
    QCOMPARE(m_db->rootGroup()->children().size(), 7);
    // the merged group should contain an entry
    QCOMPARE(m_db->rootGroup()->children().at(6)->entries().size(), 1);
    // the General group contains one entry merged from the other db
    QCOMPARE(m_db->rootGroup()->findChildByName("General")->entries().size(), 1);
}

void TestGui::testOpenRemoteDatabase()
{
    // close current database
    cleanup();

    QString sourceToSync = "sftp user@server:Database.kdbx";
    RemoteHandler::setRemoteProcessFunc([sourceToSync](QObject* parent) {
        return QScopedPointer<RemoteProcess>(
            new MockRemoteProcess(parent, QString(KEEPASSX_TEST_DATA_DIR).append("/SyncDatabase.kdbx")));
    });
    auto* openRemoteButton = m_mainWindow->findChild<QPushButton*>("buttonImport");
    QVERIFY(openRemoteButton);
    QVERIFY(openRemoteButton->isVisible());
    QTest::mouseClick(openRemoteButton, Qt::LeftButton);
    QApplication::processEvents();

    // The import wizard shows in a modal over the window
    auto* modal = m_mainWindow->findChild<VaultImportModal*>("vaultImportModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
    auto* next = modal->findChild<QPushButton*>("vaultImportContinue");
    QVERIFY(next);

    auto* importTypeList = modal->findChild<QListWidget*>("importTypeList");
    QVERIFY(importTypeList);
    for (int i = 0; i < importTypeList->count(); ++i) {
        auto item = importTypeList->item(i);
        if (item->data(Qt::UserRole) == ImportWizard::IMPORT_REMOTE) {
            importTypeList->setCurrentItem(item);
            break;
        }
    }
    QTest::keyClick(importTypeList, Qt::Key_Enter);
    QCOMPARE(modal->currentStep(), int(VaultImportModal::DetailsStep));

    auto* downloadCommandEdit = modal->findChild<QLineEdit*>("downloadCommand");
    QVERIFY(downloadCommandEdit);
    QVERIFY(downloadCommandEdit->isVisible());
    QTest::keyClicks(downloadCommandEdit, sourceToSync);

    auto* temporaryDatabaseRadio = modal->findChild<QRadioButton*>("temporaryDatabaseRadio");
    QVERIFY(temporaryDatabaseRadio);
    QTest::mouseClick(temporaryDatabaseRadio, Qt::LeftButton);

    auto* passwordEdit = modal->findChild<PasswordWidget*>("passwordEdit")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(passwordEdit);
    QTest::keyClicks(passwordEdit, "a");
    QTest::mouseClick(next, Qt::LeftButton);

    // The review previews the remote database
    QCOMPARE(modal->currentStep(), int(VaultImportModal::ReviewStep));
    QTRY_VERIFY(modal->findChildren<QTableWidget*>().count() > 0);
    QTRY_VERIFY(next->isEnabled());
    QTest::mouseClick(next, Qt::LeftButton);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultImportModal*>());

    // remote database has been opened
    QTRY_COMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), QString("SyncDatabase [Temporary]"));
}

void TestGui::testAutoreloadDatabase()
{
    config()->set(Config::AutoReloadOnChange, false);

    // Test accepting new file in autoreload
    MessageBox::setNextAnswer(MessageBox::Yes);
    // Overwrite the current database with the temp data
    QVERIFY(m_dbFile.copyFromFile(QString(KEEPASSX_TEST_DATA_DIR).append("/MergeDatabase.kdbx")));
    QApplication::processEvents();

    QTRY_VERIFY(m_db != m_dbWidget->database());
    m_db = m_dbWidget->database();

    // the General group contains one entry from the new db data
    auto generalGroup = m_db->rootGroup()->findChildByName("General");
    QVERIFY(generalGroup);
    QCOMPARE(generalGroup->entries().size(), 1);
    QVERIFY(!m_tabWidget->tabText(m_tabWidget->currentIndex()).endsWith("*"));

    // Reset the state
    cleanup();
    init();

    config()->set(Config::AutoReloadOnChange, false);

    // Test rejecting new file in autoreload
    MessageBox::setNextAnswer(MessageBox::No);
    // Overwrite the current database with the temp data
    QVERIFY(m_dbFile.copyFromFile(QString(KEEPASSX_TEST_DATA_DIR).append("/MergeDatabase.kdbx")));
    QApplication::processEvents();

    // Ensure the merge did not take place
    generalGroup = m_db->rootGroup()->findChildByName("General");
    QVERIFY(generalGroup);
    QCOMPARE(generalGroup->entries().size(), 0);
    QTRY_VERIFY(m_tabWidget->tabText(m_tabWidget->currentIndex()).endsWith("*"));

    // Reset the state
    cleanup();
    init();

    // Test accepting a merge of edits into autoreload
    // Turn on autoload so we only get one messagebox (for the merge)
    config()->set(Config::AutoReloadOnChange, true);
    // Modify some entries
    testEditEntry();

    // This is saying yes to merging the entries
    MessageBox::setNextAnswer(MessageBox::Merge);
    // Overwrite the current database with the temp data
    QVERIFY(m_dbFile.copyFromFile(QString(KEEPASSX_TEST_DATA_DIR).append("/MergeDatabase.kdbx")));
    QApplication::processEvents();

    QTRY_VERIFY(m_db != m_dbWidget->database());
    m_db = m_dbWidget->database();

    generalGroup = m_db->rootGroup()->findChildByName("General");
    QVERIFY(generalGroup);
    QCOMPARE(generalGroup->entries().size(), 1);
    QTRY_VERIFY(m_tabWidget->tabText(m_tabWidget->currentIndex()).endsWith("*"));
}

void TestGui::testTabs()
{
    // The database picker in the header stands in for the tab bar
    QCOMPARE(m_tabWidget->count(), 1);
    QVERIFY(m_tabWidget->tabBar()->isHidden());
    auto* picker = m_mainWindow->findChild<VaultHeaderBar*>()->databasePicker();
    QVERIFY(picker->isVisible());
    QCOMPARE(picker->text(), m_dbFileName);
    QCOMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), m_dbFileName);
}

void TestGui::testEditEntry()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    auto* saveButton = commandBar->findChild<QAbstractButton*>("vaultEditSaveButton");
    QVERIFY(saveButton);

    entryView->setFocus();
    QVERIFY(entryView->hasFocus());

    // Select the first entry in the database
    QModelIndex entryItem = entryView->model()->index(0, 1);
    Entry* entry = entryView->entryFromIndex(entryItem);
    clickIndex(entryItem, entryView, Qt::LeftButton);

    // Confirm the edit action and its command bar button are enabled
    auto* entryEditAction = m_mainWindow->findChild<QAction*>("actionEntryEdit");
    QVERIFY(entryEditAction->isEnabled());

    // Record current history count
    int editCount = entry->historyItems().size();

    // Edit the first entry ("Sample Entry")
    clickEditEntry();
    auto* editEntryWidget = this->editEntryWidget();
    QVERIFY(editEntryWidget);
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QTest::keyClicks(titleEdit, "_test");

    // The editor's own buttons stay in its button box, out of sight, and there is no Apply: Save in the
    // command bar returns to the entry's details
    auto* editEntryWidgetButtonBox = editEntryWidget->findChild<QDialogButtonBox*>("buttonBox");
    QVERIFY(editEntryWidgetButtonBox);
    QVERIFY(saveButton->isVisible());
    QVERIFY(commandBar->findChild<QAbstractButton*>("vaultEditCancelButton")->isVisible());

    // Save the edit, then open the editor again
    auto saveAndReopen = [&] {
        QTest::mouseClick(saveButton, Qt::LeftButton);
        QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
        clickEditEntry();
    };
    saveAndReopen();
    QCOMPARE(entry->title(), QString("Sample Entry_test"));
    QCOMPARE(entry->historyItems().size(), ++editCount);

    // Test viewing entry history
    auto historyView = editEntryWidget->findChild<QTreeView*>("historyView");
    auto showButton = editEntryWidget->findChild<QPushButton*>("showButton");
    QVERIFY(historyView);
    editEntryWidget->switchToPage(EditEntryWidget::Page::History);
    QApplication::processEvents();
    QVERIFY(historyView->isVisible());
    QVERIFY(!showButton->isEnabled());
    // Select the second row in the history view
    historyView->setCurrentIndex(historyView->model()->index(1, 0));
    QVERIFY(showButton->isEnabled());
    QTest::mouseClick(showButton, Qt::LeftButton);
    // Verify that the entry history widget is shown, read-only, in the editor's place
    auto entryHistoryWidget = m_dbWidget->findChild<QWidget*>("editEntryHistoryWidget");
    QVERIFY(entryHistoryWidget);
    QTRY_VERIFY(entryHistoryWidget->isVisible());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    QVERIFY(!saveButton->isVisible());
    QTest::keyClick(entryHistoryWidget, Qt::Key_Escape);
    QTRY_VERIFY(historyView->isVisible());

    // Test the "known bad" checkbox
    editEntryWidget->switchToPage(EditEntryWidget::Page::Advanced);
    auto excludeReportsCheckBox = editEntryWidget->findChild<QAbstractButton*>("excludeReportsCheckBox");
    QVERIFY(excludeReportsCheckBox);
    QCOMPARE(excludeReportsCheckBox->isChecked(), false);
    excludeReportsCheckBox->setChecked(true);
    saveAndReopen();
    QCOMPARE(entry->historyItems().size(), ++editCount);
    QVERIFY(entry->excludeFromReports());

    // Test tags
    auto* tags = editEntryWidget->findChild<TagsEdit*>("tagsList");
    QTest::keyClicks(tags, "_tag1");
    QTest::keyClick(tags, Qt::Key_Return);
    QCOMPARE(tags->tags().last(), QString("_tag1"));
    QTest::keyClicks(tags, "tag 2"); // adds another tag
    QTest::keyClick(tags, Qt::Key_Return);
    QCOMPARE(tags->tags().last(), QString("tag 2"));
    QTest::keyClick(tags, Qt::Key_Backspace); // Back into editing last tag
    QTest::keyClicks(tags, "_is!awesome");
    QTest::keyClick(tags, Qt::Key_Return);
    QCOMPARE(tags->tags().last(), QString("tag 2_is!awesome"));

    // Test entry colors (simulate choosing a color)
    editEntryWidget->switchToPage(EditEntryWidget::Page::Advanced);
    auto fgColor = QColor(255, 0, 0);
    auto bgColor = QColor(0, 0, 255);
    // Set foreground color
    auto colorButton = editEntryWidget->findChild<QPushButton*>("fgColorButton");
    auto colorCheckBox = editEntryWidget->findChild<QAbstractButton*>("fgColorCheckBox");
    colorButton->setProperty("color", fgColor);
    colorCheckBox->setChecked(true);
    // Set background color
    colorButton = editEntryWidget->findChild<QPushButton*>("bgColorButton");
    colorCheckBox = editEntryWidget->findChild<QAbstractButton*>("bgColorCheckBox");
    colorButton->setProperty("color", bgColor);
    colorCheckBox->setChecked(true);
    saveAndReopen();
    QCOMPARE(entry->historyItems().size(), ++editCount);

    // Test protected attributes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Advanced);
    auto* attrTextEdit = editEntryWidget->findChild<QPlainTextEdit*>("attributesEdit");
    QTest::mouseClick(editEntryWidget->findChild<QAbstractButton*>("addAttributeButton"), Qt::LeftButton);
    QString attrText = "TEST TEXT";
    QTest::keyClicks(attrTextEdit, attrText);
    QCOMPARE(attrTextEdit->toPlainText(), attrText);
    QTest::mouseClick(editEntryWidget->findChild<QAbstractButton*>("protectAttributeButton"), Qt::LeftButton);
    QVERIFY(attrTextEdit->toPlainText().contains("PROTECTED"));
    QTest::mouseClick(editEntryWidget->findChild<QAbstractButton*>("revealAttributeButton"), Qt::LeftButton);
    QCOMPARE(attrTextEdit->toPlainText(), attrText);
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);

    // Save the edit
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QApplication::processEvents();

    // Confirm edit was made
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(entry->title(), QString("Sample Entry_test"));
    QCOMPARE(entry->foregroundColor().toUpper(), fgColor.name().toUpper());
    QCOMPARE(entryItem.data(Qt::ForegroundRole), QVariant(fgColor));
    QCOMPARE(entry->backgroundColor().toUpper(), bgColor.name().toUpper());
    QCOMPARE(entryItem.data(Qt::BackgroundRole), QVariant(bgColor));
    QCOMPARE(entry->historyItems().size(), ++editCount);

    // The details show the saved entry again
    auto* details = m_dbWidget->findChild<EntryDetailCard*>();
    QTRY_VERIFY(details->isVisible());

    // Confirm modified indicator is showing
    QTRY_COMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), QString("%1*").arg(m_dbFileName));

    // Test copy & paste newline sanitization
    clickEditEntry();
    titleEdit->setText("multiline\ntitle");
    editEntryWidget->findChild<QComboBox*>("usernameComboBox")->lineEdit()->setText("multiline\nusername");
    editEntryWidget->findChild<PasswordWidget*>("passwordEdit")->setText("multiline\npassword");
    editEntryWidget->findChild<QLineEdit*>("urlEdit")->setText("multiline\nurl");
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    QCOMPARE(entry->title(), QString("multiline title"));
    QCOMPARE(entry->username(), QString("multiline username"));
    // here we keep newlines, so users can't lock themselves out accidentally
    QCOMPARE(entry->password(), QString("multiline\npassword"));
    QCOMPARE(entry->url(), QString("multiline url"));
}

void TestGui::testSearchEditEntry()
{
    // Regression test for Issue #1447 -- Uses example from issue description

    // Groups are created in a modal over the window
    auto* editGroupWidget = m_mainWindow->findChild<EditGroupWidget*>("editGroupWidget");
    auto* nameEdit = editGroupWidget->findChild<QLineEdit*>("editName");

    // Add groups "Good" and "Bad"
    m_dbWidget->createGroup();
    auto* groupModal = m_mainWindow->findChild<VaultModal*>("vaultGroupModal");
    QVERIFY(groupModal);
    auto* groupSave = groupModal->findChild<QPushButton*>("vaultGroupSave");
    QTRY_VERIFY(groupModal->isOpen());
    QTest::keyClicks(nameEdit, "Good");
    QTest::mouseClick(groupSave, Qt::LeftButton);
    QTRY_VERIFY(!groupModal->isOpen());
    m_dbWidget->groupView()->setCurrentGroup(m_db->rootGroup()); // Makes "Good" and "Bad" on the same level
    m_dbWidget->createGroup();
    QTRY_VERIFY(groupModal->isOpen());
    QTest::keyClicks(nameEdit, "Bad");
    QTest::mouseClick(groupSave, Qt::LeftButton);
    QTRY_VERIFY(!groupModal->isOpen());
    m_dbWidget->groupView()->setCurrentGroup(m_db->rootGroup());

    // Find the entry editor
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");

    // Create "Doggy" in "Good"
    Group* goodGroup = m_dbWidget->currentGroup()->findChildByName(QString("Good"));
    m_dbWidget->groupView()->setCurrentGroup(goodGroup);
    clickNewEntry();
    QTest::keyClicks(titleEdit, "Doggy");
    QTest::mouseClick(m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    // Select "Bad" group in groupView
    Group* badGroup = m_db->rootGroup()->findChildByName(QString("Bad"));
    m_dbWidget->groupView()->setCurrentGroup(badGroup);

    // Search for "Doggy" entry in the header search box
    auto* searchWidget = m_mainWindow->findChild<VaultHeaderBar*>()->findChild<SearchWidget*>("SearchWidget");
    auto* searchTextEdit = searchWidget->findChild<QLineEdit*>("searchEdit");
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    QTest::keyClicks(searchTextEdit, "Doggy");
    QTRY_VERIFY(m_dbWidget->isSearchActive());

    // Goto "Doggy"'s edit view
    QTest::keyClick(searchTextEdit, Qt::Key_Return);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);

    // Check the editor names the entry's own group, not the selected one
    QCOMPARE(titleEdit->text(), QString("Doggy"));
    auto* card = m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
    QTRY_COMPARE(card->findChild<QLabel*>("vaultEditSubtitle")->text(), QStringLiteral("Good \u00b7 Editing"));
}

void TestGui::testAddEntry()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // Given the status bar label with initial number of entries.
    checkEntryCount(1);

    // Find the new entry action
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    // Click the new entry button in the command bar and check that we enter edit mode
    clickNewEntry();
    auto* saveButton = m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave");
    auto* cancelButton = m_mainWindow->findChild<QPushButton*>("vaultNewEntryCancel");
    QVERIFY(saveButton);
    QVERIFY(cancelButton);

    // Add entry "test" and confirm added
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QTest::keyClicks(titleEdit, "test");
    auto* usernameComboBox = editEntryWidget->findChild<QComboBox*>("usernameComboBox");
    QVERIFY(usernameComboBox);
    QTest::mouseClick(usernameComboBox, Qt::LeftButton);
    QTest::keyClicks(usernameComboBox, "AutocompletionUsername");
    QTest::mouseClick(saveButton, Qt::LeftButton);

    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QModelIndex item = entryView->model()->index(1, 1);
    Entry* entry = entryView->entryFromIndex(item);

    QCOMPARE(entry->title(), QString("test"));
    QCOMPARE(entry->username(), QString("AutocompletionUsername"));
    QCOMPARE(entry->historyItems().size(), 0);

    m_db->updateCommonUsernames();

    // Then the status bar label should be updated with incremented number of entries.
    checkEntryCount(2);

    // Add entry "something 2"
    clickNewEntry();
    QTest::keyClicks(titleEdit, "something 2");
    QTest::mouseClick(usernameComboBox, Qt::LeftButton);
    QTest::keyClicks(usernameComboBox, "Auto");
    QTest::keyPress(usernameComboBox, Qt::Key_Right);
    auto* passwordEdit =
        editEntryWidget->findChild<PasswordWidget*>("passwordEdit")->findChild<QLineEdit*>("passwordEdit");
    QTest::keyClicks(passwordEdit, "something 2");
    QTest::mouseClick(saveButton, Qt::LeftButton);

    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    item = entryView->model()->index(1, 1);
    entry = entryView->entryFromIndex(item);

    QCOMPARE(entry->title(), QString("something 2"));
    QCOMPARE(entry->username(), QString("AutocompletionUsername"));
    QCOMPARE(entry->historyItems().size(), 0);

    // Add entry "something 5" but click cancel button (does NOT add entry)
    clickNewEntry();
    QTest::keyClicks(titleEdit, "something 5");
    MessageBox::setNextAnswer(MessageBox::Discard);
    QTest::mouseClick(cancelButton, Qt::LeftButton);

    QApplication::processEvents();

    // Confirm no changed entry count
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QTRY_COMPARE(entryView->model()->rowCount(), 3);
}

void TestGui::testPasswordEntryEntropy_data()
{
    QTest::addColumn<QString>("password");
    QTest::addColumn<QString>("expectedStrengthLabel");

    QTest::newRow("Empty password") << "" << "Password Quality: Poor";

    QTest::newRow("Well-known password") << "hello" << "Password Quality: Poor";

    QTest::newRow("Password composed of well-known words.") << "helloworld" << "Password Quality: Poor";

    QTest::newRow("Password composed of well-known words with number.") << "password1" << "Password Quality: Poor";

    QTest::newRow("Password out of small character space.") << "D0g.................." << "Password Quality: Poor";

    QTest::newRow("XKCD, easy substitutions.") << "Tr0ub4dour&3" << "Password Quality: Poor";

    QTest::newRow("XKCD, word generator.") << "correcthorsebatterystaple" << "Password Quality: Weak";

    QTest::newRow("Random characters, medium length.") << "YQC3kbXbjC652dTDH" << "Password Quality: Good";

    QTest::newRow("Random characters, long.") << "Bs5ZFfthWzR8DGFEjaCM6bGqhmCT4km" << "Password Quality: Excellent";

    QTest::newRow("Long password using Zxcvbn chunk estimation")
        << "quintet-tamper-kinswoman-humility-vengeful-haven-tastiness-aspire-widget-ipad-cussed-reaffirm-ladylike-"
           "ashamed-anatomy-daybed-jam-swear-strudel-neatness-stalemate-unbundle-flavored-relation-emergency-underrate-"
           "registry-getting-award-unveiled-unshaken-stagnate-cartridge-magnitude-ointment-hardener-enforced-scrubbed-"
           "radial-fiddling-envelope-unpaved-moisture-unused-crawlers-quartered-crushed-kangaroo-tiptop-doily"
        << "Password Quality: Excellent";

    QTest::newRow("Longer password above Zxcvbn threshold")
        << "quintet-tamper-kinswoman-humility-vengeful-haven-tastiness-aspire-widget-ipad-cussed-reaffirm-ladylike-"
           "ashamed-anatomy-daybed-jam-swear-strudel-neatness-stalemate-unbundle-flavored-relation-emergency-underrate-"
           "registry-getting-award-unveiled-unshaken-stagnate-cartridge-magnitude-ointment-hardener-enforced-scrubbed-"
           "radial-fiddling-envelope-unpaved-moisture-unused-crawlers-quartered-crushed-kangaroo-tiptop-doily-hefty-"
           "untie-fidgeting-radiance-twilight-freebase-sulphuric-parrot-decree-monotype-nautical-pout-sip-geometric-"
           "crunching-deviancy-festival-hacking-rage-unify-coronary-zigzagged-dwindle-possum-lilly-exhume-daringly-"
           "barbell-rage-animate-lapel-emporium-renounce-justifier-relieving-gauze-arrive-alive-collected-immobile-"
           "unleash-snowman-gift-expansion-marbles-requisite-excusable-flatness-displace-caloric-sensuous-moustache-"
           "sensuous-capillary-aversion-contents-cadet-giggly-amenity-peddling-spotting-drier-mooned-rudder-peroxide-"
           "posting-oppressor-scrabble-scorer-whomever-paprika-slapstick-said-spectacle-capture-debate-attire-emcee-"
           "unfocused-sympathy-doily-election-ambulance-polish-subtype-grumbling-neon-stooge-reanalyze-rockfish-"
           "disparate-decorated-washroom-threefold-muzzle-buckwheat-kerosene-swell-why-reprocess-correct-shady-"
           "impatient-slit-banshee-scrubbed-dreadful-unlocking-urologist-hurried-citable-fragment-septic-lapped-"
           "prankish-phantom-unpaved-moisture-unused-crawlers-quartered-crushed-kangaroo-lapel-emporium-renounce"
        << "Password Quality: Excellent";
}

void TestGui::testPasswordEntryEntropy()
{
    // Find the new entry action
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    // Click the new entry button in the command bar and check that we enter edit mode
    clickNewEntry();

    // Add entry "test" and confirm added
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QTest::keyClicks(titleEdit, "test");

    // Open the password generator
    auto* passwordEdit =
        editEntryWidget->findChild<PasswordWidget*>("passwordEdit")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(passwordEdit);
    QTest::mouseClick(passwordEdit, Qt::LeftButton);

#ifdef Q_OS_MACOS
    QTest::keyClick(passwordEdit, Qt::Key_G, Qt::MetaModifier);
#else
    QTest::keyClick(passwordEdit, Qt::Key_G, Qt::ControlModifier);
#endif

    TEST_MODAL(
        PasswordGeneratorWidget * pwGeneratorWidget;
        QTRY_VERIFY(pwGeneratorWidget = editEntryWidget->findChild<PasswordGeneratorWidget*>());

        // Type in some password
        auto* generatedPassword =
            pwGeneratorWidget->findChild<PasswordWidget*>("editNewPassword")->findChild<QLineEdit*>("passwordEdit");
        auto* entropyLabel = pwGeneratorWidget->findChild<QLabel*>("entropyLabel");
        auto* strengthLabel = pwGeneratorWidget->findChild<QLabel*>("strengthLabel");
        auto* passwordLengthLabel = pwGeneratorWidget->findChild<QLabel*>("passwordLengthLabel");

        QFETCH(QString, password);
        QFETCH(QString, expectedStrengthLabel);

        // Dynamically calculate entropy due to variances with zxcvbn wordlists
        PasswordHealth health(password);
        auto expectedEntropy = QString("Entropy: %1 bit").arg(QString::number(health.entropy(), 'f', 2));
        auto expectedPasswordLength = QString("Characters: %1").arg(QString::number(password.length()));

        generatedPassword->setText(password);
        QCOMPARE(entropyLabel->text(), expectedEntropy);
        QCOMPARE(strengthLabel->text(), expectedStrengthLabel);
        QCOMPARE(passwordLengthLabel->text(), expectedPasswordLength);

        QTest::mouseClick(generatedPassword, Qt::LeftButton);
        QTest::keyClick(generatedPassword, Qt::Key_Escape););
}

void TestGui::testDicewareEntryEntropy()
{
    // Find the new entry action
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    // Click the new entry button in the command bar and check that we enter edit mode
    clickNewEntry();

    // Add entry "test" and confirm added
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QTest::keyClicks(titleEdit, "test");

    // Open the password generator
    auto* passwordEdit = editEntryWidget->findChild<PasswordWidget*>()->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(passwordEdit);
    QTest::mouseClick(passwordEdit, Qt::LeftButton);

#ifdef Q_OS_MACOS
    QTest::keyClick(passwordEdit, Qt::Key_G, Qt::MetaModifier);
#else
    QTest::keyClick(passwordEdit, Qt::Key_G, Qt::ControlModifier);
#endif

    TEST_MODAL(
        PasswordGeneratorWidget * pwGeneratorWidget;
        QTRY_VERIFY(pwGeneratorWidget = editEntryWidget->findChild<PasswordGeneratorWidget*>());

        // Select Diceware
        auto* generatedPassword =
            pwGeneratorWidget->findChild<PasswordWidget*>("editNewPassword")->findChild<QLineEdit*>("passwordEdit");
        auto* tabWidget = pwGeneratorWidget->findChild<QTabWidget*>("tabWidget");
        auto* dicewareWidget = pwGeneratorWidget->findChild<QWidget*>("dicewareWidget");
        tabWidget->setCurrentWidget(dicewareWidget);

        auto* comboBoxWordList = dicewareWidget->findChild<QComboBox*>("comboBoxWordList");
        comboBoxWordList->setCurrentText("eff_large.wordlist");
        auto* spinBoxWordCount = dicewareWidget->findChild<VaultSpinBox*>("spinBoxWordCount");
        spinBoxWordCount->setValue(6);

        // Confirm a password was generated
        QVERIFY(!pwGeneratorWidget->getGeneratedPassword().isEmpty());

        // Verify entropy and strength
        auto* entropyLabel = pwGeneratorWidget->findChild<QLabel*>("entropyLabel");
        auto* strengthLabel = pwGeneratorWidget->findChild<QLabel*>("strengthLabel");
        auto* wordLengthLabel = pwGeneratorWidget->findChild<QLabel*>("passwordLengthLabel");

        QTRY_COMPARE_WITH_TIMEOUT(entropyLabel->text(), QString("Entropy: 77.54 bit"), 200);
        QCOMPARE(strengthLabel->text(), QString("Password Quality: Good"));
        QCOMPARE(wordLengthLabel->text(),
                 QString("Characters: %1").arg(QString::number(pwGeneratorWidget->getGeneratedPassword().length())));

        QTest::mouseClick(generatedPassword, Qt::LeftButton);
        QTest::keyClick(generatedPassword, Qt::Key_Escape););
}

void TestGui::testTotp()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    QCOMPARE(entryView->model()->rowCount(), 1);
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QModelIndex item = entryView->model()->index(0, 1);
    Entry* entry = entryView->entryFromIndex(item);
    clickIndex(item, entryView, Qt::LeftButton);

    triggerAction("actionEntrySetupTotp");

    // TOTP is set up in a modal over the window
    auto* setupTotpModal = m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal");
    QVERIFY(setupTotpModal);
    QTRY_VERIFY(setupTotpModal->isOpen());
    auto* setupTotpDialog = setupTotpModal->findChild<TotpSetupDialog*>("TotpSetupDialog");
    QVERIFY(setupTotpDialog);

    QApplication::processEvents();

    QString exampleSeed = "gezd gnbvgY 3tqojqGEZdgnb vgy3tqoJq===";
    QString expectedFinalSeed = exampleSeed.toUpper().remove(" ").remove("=");
    auto* seedEdit = setupTotpDialog->findChild<QLineEdit*>("seedEdit");
    seedEdit->setText("");
    QTest::keyClicks(seedEdit, exampleSeed);

    QTest::mouseClick(setupTotpModal->findChild<QPushButton*>("vaultTotpSetupSave"), Qt::LeftButton);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal"));

    // Make sure the entryView is selected and active
    entryView->activateWindow();
    entryView->setFocus();
    QApplication::processEvents();
    QTRY_VERIFY(entryView->hasFocus());

    clickEditEntry();

    auto* editEntryWidget = this->editEntryWidget();
    editEntryWidget->switchToPage(EditEntryWidget::Page::Advanced);
    QApplication::processEvents();
    // Find the "TOTP Seed" attribute in the list view
    auto* attrListView = editEntryWidget->findChild<QListView*>("attributesView");
    auto index = attrListView->model()->index(0, 0);
    for (auto i = 0; i < attrListView->model()->rowCount(); ++i) {
        index = attrListView->model()->index(i, 0);
        if (attrListView->model()->data(index).toString().compare("TOTP Seed") == 0) {
            break;
        }
    }
    attrListView->setCurrentIndex(index);
    auto* attrTextEdit = editEntryWidget->findChild<QPlainTextEdit*>("attributesEdit");
    QTest::mouseClick(editEntryWidget->findChild<QAbstractButton*>("revealAttributeButton"), Qt::LeftButton);
    QCOMPARE(attrTextEdit->toPlainText(), expectedFinalSeed);

    QTest::mouseClick(m_dbWidget->findChild<QAbstractButton*>("vaultEditSaveButton"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // Test the TOTP value
    triggerAction("actionEntryTotp");

    auto* totpDialog = m_dbWidget->findChild<TotpDialog*>("TotpDialog");
    auto* totpLabel = totpDialog->findChild<QLabel*>("totpLabel");

    QTRY_COMPARE(totpLabel->text().replace(" ", ""), entry->totp());
    QTest::keyClick(totpDialog, Qt::Key_Escape);

    // Test the QR code
    triggerAction("actionEntryTotpQRCode");
    auto* qrCodeDialog = m_mainWindow->findChild<QDialog*>("entryQrCodeWidget");
    QVERIFY(qrCodeDialog);
    QVERIFY(qrCodeDialog->isVisible());
    auto* qrCodeWidget = qrCodeDialog->findChild<QWidget*>("squareSvgWidget");
    QVERIFY2(qrCodeWidget->geometry().width() == qrCodeWidget->geometry().height(), "Initial QR code is not square");

    // Test the QR code window resizing, make the dialog bigger.
    qrCodeDialog->setFixedSize(800, 600);
    QVERIFY2(qrCodeWidget->geometry().width() == qrCodeWidget->geometry().height(), "Resized QR code is not square");
    QTest::keyClick(qrCodeDialog, Qt::Key_Escape);
}

void TestGui::testSearch()
{
    // Add canned entries for consistent testing
    addCannedEntries();

    // Search sits in the header
    auto* searchWidget = m_mainWindow->findChild<VaultHeaderBar*>()->findChild<SearchWidget*>("SearchWidget");
    QVERIFY(searchWidget->isEnabled());
    auto* searchTextEdit = searchWidget->findChild<QLineEdit*>("searchEdit");
    auto* waitForEnterAction = searchWidget->findChild<QAction*>("actionSearchWaitForEnter");

    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    QVERIFY(entryView->isVisible());

    // The Fluent search box brings its own clear button
    auto* searchBox = qobject_cast<fluent::textfields::AutoSuggestBox*>(searchTextEdit);
    QVERIFY(searchBox);
    QVERIFY(searchBox->clearButtonSize() > 0);

    auto* helpButton = searchWidget->findChild<QAction*>("helpIcon");
    auto* helpPanel = searchWidget->findChild<VaultSearchHelp*>();
    QVERIFY(helpPanel);
    QVERIFY(helpButton->isVisible());
    QVERIFY(!helpPanel->isOpen());

    // Test "wait for enter" toggle
    QVERIFY(waitForEnterAction->isVisible());
    QVERIFY(waitForEnterAction->isCheckable());

    // Test search with "wait for enter" disabled (default)
    searchTextEdit->clear();
    QTest::keyClicks(searchTextEdit, "ZZZ");
    QTRY_COMPARE(searchTextEdit->text(), QString("ZZZ"));
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 0);

    // Clear search
    searchTextEdit->clear();
    QTRY_VERIFY(!m_dbWidget->isSearchActive());

    // Enable "wait for enter" mode
    waitForEnterAction->trigger();
    QVERIFY(waitForEnterAction->isChecked());

    // Test search with "wait for enter" enabled
    QTest::keyClicks(searchTextEdit, "ZZZ");
    QTRY_VERIFY(!m_dbWidget->isSearchActive());

    // Press Enter to execute search
    QTest::keyClick(searchTextEdit, Qt::Key_Return);
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 0);
    // Check that search remains active even after clearing
    searchTextEdit->clear();
    QTRY_VERIFY(m_dbWidget->isSearchActive());

    // Disable "wait for enter" mode
    waitForEnterAction->trigger();
    QVERIFY(!waitForEnterAction->isChecked());

    // Test search with "wait for enter" disabled again
    QTest::keyClicks(searchTextEdit, "ZZZ");
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 0);

    // Clear search
    searchTextEdit->clear();
    QTRY_VERIFY(!m_dbWidget->isSearchActive());

    // Enter search
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    QTRY_VERIFY(searchTextEdit->hasFocus());
    // Show/Hide search help
    helpButton->trigger();
    QTRY_VERIFY(helpPanel->isOpen());
    QApplication::processEvents();
    helpButton->trigger();
    QTRY_VERIFY(!helpPanel->isOpen());

    // Need to re-activate the window after the help test
    m_mainWindow->activateWindow();

    // Search for "ZZZ"
    QTest::keyClicks(searchTextEdit, "ZZZ");
    QTRY_COMPARE(searchTextEdit->text(), QString("ZZZ"));
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 0);
    // Press the search clear button
    searchTextEdit->clear();
    QTRY_VERIFY(searchTextEdit->text().isEmpty());
    QTRY_VERIFY(searchTextEdit->hasFocus());

    // Test tag search
    searchTextEdit->clear();
    QTest::keyClicks(searchTextEdit, "tag: testTag");
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 1);

    searchTextEdit->clear();
    QTRY_VERIFY(searchTextEdit->text().isEmpty());
    QTRY_VERIFY(searchTextEdit->hasFocus());
    // Escape clears searchedit and retains focus
    QTest::keyClicks(searchTextEdit, "ZZZ");
    QTest::keyClick(searchTextEdit, Qt::Key_Escape);
    QTRY_VERIFY(searchTextEdit->text().isEmpty());
    QTRY_VERIFY(searchTextEdit->hasFocus());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    // Search for "some"
    QTest::keyClicks(searchTextEdit, "some");
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->model()->rowCount(), 3);
    // Search for "someTHING"
    QTest::keyClicks(searchTextEdit, "THING");
    QTRY_COMPARE(entryView->model()->rowCount(), 2);
    // Press Down to focus on the entry view
    QTest::keyClick(searchTextEdit, Qt::Key_Right, Qt::ControlModifier);
    QTRY_VERIFY(searchTextEdit->hasFocus());
    QTest::keyClick(searchTextEdit, Qt::Key_Down);
    QTRY_VERIFY(entryView->hasFocus());
    auto* searchedEntry = entryView->currentEntry();
    // Restore focus using F3 key and search text selection
    QTest::keyClick(m_mainWindow.data(), Qt::Key_F3);
    QTRY_VERIFY(searchTextEdit->hasFocus());
    QTRY_COMPARE(searchTextEdit->selectedText(), QString("someTHING"));

    searchedEntry->setPassword("password");
    QClipboard* clipboard = QApplication::clipboard();

    // Copy to clipboard: should copy search text (not password)
    QTest::keyClick(searchTextEdit, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboard->text(), QString("someTHING"));
    // Deselect text and confirm password copies
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    QTRY_VERIFY(searchTextEdit->selectedText().isEmpty());
    QTRY_VERIFY(searchTextEdit->hasFocus());
    QTest::keyClick(searchTextEdit, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboard->text(), searchedEntry->password());
    // Ensure Down focuses on entry view when search text is selected
    QTest::keyClick(searchTextEdit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(searchTextEdit, Qt::Key_Down);
    QTRY_VERIFY(entryView->hasFocus());
    QCOMPARE(entryView->currentEntry(), searchedEntry);
    // Test that password copies with entry focused
    QTest::keyClick(entryView, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboard->text(), searchedEntry->password());
    // Refocus back to search edit
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    QTRY_VERIFY(searchTextEdit->hasFocus());
    // Select search text and test that password does not copy
    searchTextEdit->selectAll();
    QTest::keyClick(searchTextEdit, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(clipboard->text(), QString("someTHING"));
    // Ensure password copies when clicking on copy password button despite selected text
    auto copyPasswordAction = m_mainWindow->findChild<QAction*>("actionEntryCopyPassword");
    QVERIFY(copyPasswordAction);
    // The detail card's copy button stands in for the toolbar's
    QAbstractButton* copyPasswordWidget = nullptr;
    for (auto* button : m_dbWidget->findChild<EntryDetailCard*>()->findChildren<QAbstractButton*>()) {
        if (button->toolTip() == "Copy password") {
            copyPasswordWidget = button;
        }
    }
    QVERIFY(copyPasswordWidget);
    QVERIFY(copyPasswordWidget->isVisible());
    QTest::mouseClick(copyPasswordWidget, Qt::LeftButton);
    QCOMPARE(clipboard->text(), searchedEntry->password());
    // Deselect text and deselect entry, Ctrl+C should now do nothing
    clipboard->clear();
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    entryView->clearSelection();
    QTest::keyClick(searchTextEdit, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboard->text(), QString());

    // Test case sensitive search
    searchWidget->setCaseSensitive(true);
    QTRY_COMPARE(entryView->model()->rowCount(), 0);
    searchWidget->setCaseSensitive(false);
    QTRY_COMPARE(entryView->model()->rowCount(), 2);

    // Test group search
    searchWidget->setLimitGroup(false);
    GroupView* groupView = m_dbWidget->findChild<GroupView*>("groupView");
    QCOMPARE(groupView->currentGroup(), m_db->rootGroup());
    QModelIndex rootGroupIndex = groupView->model()->index(0, 0);
    clickIndex(groupView->model()->index(0, 0, rootGroupIndex), groupView, Qt::LeftButton);
    QCOMPARE(groupView->currentGroup()->name(), QString("General"));
    // Selecting a group should cancel search
    QTRY_COMPARE(entryView->model()->rowCount(), 0);
    // Restore search
    QTest::keyClick(m_mainWindow.data(), Qt::Key_F, Qt::ControlModifier);
    QTest::keyClicks(searchTextEdit, "someTHING");
    QTRY_COMPARE(entryView->model()->rowCount(), 2);
    // Enable group limiting
    searchWidget->setLimitGroup(true);
    QTRY_COMPARE(entryView->model()->rowCount(), 0);
    // Selecting another group should NOT cancel search
    clickIndex(rootGroupIndex, groupView, Qt::LeftButton);
    QCOMPARE(groupView->currentGroup(), m_db->rootGroup());
    QTRY_COMPARE(entryView->model()->rowCount(), 2);

    // reset
    searchWidget->setLimitGroup(false);
    clickIndex(rootGroupIndex, groupView, Qt::LeftButton);
    QCOMPARE(groupView->currentGroup(), m_db->rootGroup());
    QVERIFY(!m_dbWidget->isSearchActive());

    // check if first entry is selected after search
    QTest::keyClicks(searchTextEdit, "some");
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QTRY_COMPARE(entryView->selectedEntries().length(), 1);
    QModelIndex index_current = entryView->indexFromEntry(entryView->currentEntry());
    QTRY_COMPARE(index_current.row(), 0);

    // Try to edit the first entry from the search view
    // Refocus back to search edit
    QTest::mouseClick(searchTextEdit, Qt::LeftButton);
    QTRY_VERIFY(searchTextEdit->hasFocus());
    QTest::keyClicks(searchTextEdit, "someTHING");
    QTRY_VERIFY(m_dbWidget->isSearchActive());

    QModelIndex item = entryView->model()->index(0, 1);
    Entry* entry = entryView->entryFromIndex(item);
    QTest::keyClick(searchTextEdit, Qt::Key_Return);
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);

    // Perform the edit and save it
    EditEntryWidget* editEntryWidget = this->editEntryWidget();
    QLineEdit* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QString origTitle = titleEdit->text();
    QTest::keyClicks(titleEdit, "_edited");
    QTest::mouseClick(m_dbWidget->findChild<QAbstractButton*>("vaultEditSaveButton"), Qt::LeftButton);

    // Confirm the edit was made and we are back in search mode
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QCOMPARE(entry->title(), origTitle.append("_edited"));

    // Cancel search, should return to normal view
    QTest::keyClick(m_mainWindow.data(), Qt::Key_Escape);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestGui::testDeleteEntry()
{
    // Add canned entries for consistent testing
    addCannedEntries();
    checkEntryCount(4);

    auto* groupView = m_dbWidget->findChild<GroupView*>("groupView");
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    // Delete sits in the command bar
    QWidget* entryDeleteWidget = commandButton("actionEntryDelete");
    QVERIFY(entryDeleteWidget);
    entryView->setFocus();

    // Move one entry to the recycling bin
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    clickIndex(entryView->model()->index(1, 1), entryView, Qt::LeftButton);
    QVERIFY(entryDeleteWidget->isVisible());
    QVERIFY(entryDeleteWidget->isEnabled());
    QVERIFY(!m_db->metadata()->recycleBin());

    // Test with confirmation dialog
    if (!config()->get(Config::Security_NoConfirmMoveEntryToRecycleBin).toBool()) {
        MessageBox::setNextAnswer(MessageBox::Move);
        QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);

        QCOMPARE(entryView->model()->rowCount(), 3);
        QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 1);
    } else {
        // no confirm dialog
        QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
        QCOMPARE(entryView->model()->rowCount(), 3);
        QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 1);
    }

    checkEntryCount(3);

    // Select multiple entries and move them to the recycling bin
    clickIndex(entryView->model()->index(1, 1), entryView, Qt::LeftButton);
    clickIndex(entryView->model()->index(2, 1), entryView, Qt::LeftButton, Qt::ControlModifier);
    QCOMPARE(entryView->selectionModel()->selectedRows().size(), 2);

    if (!config()->get(Config::Security_NoConfirmMoveEntryToRecycleBin).toBool()) {
        MessageBox::setNextAnswer(MessageBox::Cancel);
        QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
        QCOMPARE(entryView->model()->rowCount(), 3);
        QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 1);

        MessageBox::setNextAnswer(MessageBox::Move);
        QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
        QCOMPARE(entryView->model()->rowCount(), 1);
        QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 3);
    } else {
        QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
        QCOMPARE(entryView->model()->rowCount(), 1);
        QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 3);
    }

    // Go to the recycling bin
    QCOMPARE(groupView->currentGroup(), m_db->rootGroup());
    QModelIndex rootGroupIndex = groupView->model()->index(0, 0);
    clickIndex(groupView->model()->index(groupView->model()->rowCount(rootGroupIndex) - 1, 0, rootGroupIndex),
               groupView,
               Qt::LeftButton);
    QCOMPARE(groupView->currentGroup()->name(), m_db->metadata()->recycleBin()->name());

    // Delete one entry from the bin
    clickIndex(entryView->model()->index(0, 1), entryView, Qt::LeftButton);
    MessageBox::setNextAnswer(MessageBox::Cancel);
    QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
    QCOMPARE(entryView->model()->rowCount(), 3);
    QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 3);

    MessageBox::setNextAnswer(MessageBox::Delete);
    QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
    QCOMPARE(entryView->model()->rowCount(), 2);
    QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 2);

    // Select the remaining entries and delete them
    clickIndex(entryView->model()->index(0, 1), entryView, Qt::LeftButton);
    clickIndex(entryView->model()->index(1, 1), entryView, Qt::LeftButton, Qt::ControlModifier);
    MessageBox::setNextAnswer(MessageBox::Delete);
    QTest::mouseClick(entryDeleteWidget, Qt::LeftButton);
    QCOMPARE(entryView->model()->rowCount(), 0);
    QCOMPARE(m_db->metadata()->recycleBin()->entries().size(), 0);

    // Ensure the detail card shows the recycling group since all entries are deleted
    auto* detailCard = m_dbWidget->findChild<EntryDetailCard*>();
    QVERIFY(detailCard);
    QTRY_VERIFY(detailCard->isVisible());
    const auto recycleBinName = m_db->metadata()->recycleBin()->name();
    auto showsRecycleBin = [detailCard, recycleBinName] {
        for (auto* label : detailCard->findChildren<QLabel*>()) {
            if (label->isVisible() && label->text() == recycleBinName) {
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY(showsRecycleBin());

    // Go back to the root group
    clickIndex(groupView->model()->index(0, 0), groupView, Qt::LeftButton);
    QCOMPARE(groupView->currentGroup(), m_db->rootGroup());
}

void TestGui::testCloneEntry()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    entryView->setFocus();

    QCOMPARE(entryView->model()->rowCount(), 1);

    QModelIndex item = entryView->model()->index(0, 1);
    Entry* entryOrg = entryView->entryFromIndex(item);
    clickIndex(item, entryView, Qt::LeftButton);

    triggerAction("actionEntryClone");

    auto* cloneDialog = m_dbWidget->findChild<CloneDialog*>("CloneDialog");
    auto* cloneButtonBox = cloneDialog->findChild<QDialogButtonBox*>("buttonBox");
    QTest::mouseClick(cloneButtonBox->button(QDialogButtonBox::Ok), Qt::LeftButton);

    QCOMPARE(entryView->model()->rowCount(), 2);
    Entry* entryClone = entryView->entryFromIndex(entryView->model()->index(1, 1));
    QVERIFY(entryOrg->uuid() != entryClone->uuid());
    QCOMPARE(entryClone->title(), entryOrg->title() + QString(" - Clone"));
    QVERIFY(m_dbWidget->currentSelectedEntry()->uuid() == entryClone->uuid());
}

void TestGui::testEntryPlaceholders()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // Find the new entry action
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    // Click the new entry button in the command bar and check that we enter edit mode
    clickNewEntry();

    // Add entry "test" and confirm added
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QLineEdit* urlEdit = editEntryWidget->findChild<QLineEdit*>("urlEdit");
    QTest::keyClicks(urlEdit, "{TITLE}.{USERNAME}");
    // The title follows the URL until one is typed
    titleEdit->clear();
    QTest::keyClicks(titleEdit, "test");
    QComboBox* usernameComboBox = editEntryWidget->findChild<QComboBox*>("usernameComboBox");
    QTest::keyClicks(usernameComboBox, "john");
    QTest::mouseClick(m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    QCOMPARE(entryView->model()->rowCount(), 2);

    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QModelIndex item = entryView->model()->index(1, 1);
    Entry* entry = entryView->entryFromIndex(item);

    QCOMPARE(entry->title(), QString("test"));
    QCOMPARE(entry->url(), QString("{TITLE}.{USERNAME}"));

    // Test password copy
    QClipboard* clipboard = QApplication::clipboard();
    m_dbWidget->copyURL();
    QTRY_COMPARE(clipboard->text(), QString("test.john"));
}

void TestGui::testDragAndDropEntry()
{
    auto entryView = m_dbWidget->findChild<EntryView*>("entryView");
    auto groupView = m_dbWidget->findChild<GroupView*>("groupView");
    auto groupModel = qobject_cast<GroupModel*>(groupView->model());

    QModelIndex sourceIndex = entryView->model()->index(0, 1);
    QModelIndex targetIndex = groupModel->index(0, 0, groupModel->index(0, 0));
    QVERIFY(sourceIndex.isValid());
    QVERIFY(targetIndex.isValid());
    auto targetGroup = groupModel->groupFromIndex(targetIndex);

    QMimeData mimeData;
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);

    auto entry = entryView->entryFromIndex(sourceIndex);
    stream << entry->group()->database()->uuid() << entry->uuid();
    mimeData.setData("application/x-keepassx-entry", encoded);

    // Test Copy, UUID should change, history remain
    QVERIFY(groupModel->dropMimeData(&mimeData, Qt::CopyAction, -1, 0, targetIndex));
    // Find the copied entry
    auto newEntry = targetGroup->findEntryByPath(entry->title());
    QVERIFY(newEntry);
    QVERIFY(entry->uuid() != newEntry->uuid());
    QCOMPARE(entry->historyItems().count(), newEntry->historyItems().count());

    encoded.clear();
    entry = entryView->entryFromIndex(sourceIndex);
    auto history = entry->historyItems().count();
    auto uuid = entry->uuid();
    stream << entry->group()->database()->uuid() << entry->uuid();
    mimeData.setData("application/x-keepassx-entry", encoded);

    // Test Move, entry pointer should remain the same
    QCOMPARE(entry->group()->name(), QString("NewDatabase"));
    QVERIFY(groupModel->dropMimeData(&mimeData, Qt::MoveAction, -1, 0, targetIndex));
    QCOMPARE(entry->group()->name(), QString("General"));
    QCOMPARE(entry->uuid(), uuid);
    QCOMPARE(entry->historyItems().count(), history);
}

void TestGui::testDragAndDropGroup()
{
    QAbstractItemModel* groupModel = m_dbWidget->findChild<GroupView*>("groupView")->model();
    QModelIndex rootIndex = groupModel->index(0, 0);

    dragAndDropGroup(groupModel->index(0, 0, rootIndex), groupModel->index(1, 0, rootIndex), -1, true, "Windows", 0);

    // dropping parent on child is supposed to fail
    dragAndDropGroup(groupModel->index(0, 0, rootIndex),
                     groupModel->index(0, 0, groupModel->index(0, 0, rootIndex)),
                     -1,
                     false,
                     "NewDatabase",
                     0);

    dragAndDropGroup(groupModel->index(1, 0, rootIndex), rootIndex, 0, true, "NewDatabase", 0);

    dragAndDropGroup(groupModel->index(0, 0, rootIndex), rootIndex, -1, true, "NewDatabase", 4);
}

void TestGui::testSaveAs()
{
    QFileInfo fileInfo(m_dbFilePath);
    QDateTime lastModified = fileInfo.lastModified();

    m_db->metadata()->setName("testSaveAs");

    // open temporary file so it creates a filename
    TemporaryFile tmpFile;
    QVERIFY(tmpFile.open());
    QString tmpFileName = tmpFile.fileName();
    tmpFile.remove();

    fileDialog()->setNextFileName(tmpFileName);

    triggerAction("actionDatabaseSaveAs");

    QCOMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), QString("testSaveAs"));

    checkDatabase(tmpFileName);

    fileInfo.refresh();
    QCOMPARE(fileInfo.lastModified(), lastModified);
    tmpFile.remove();
}

void TestGui::testSaveBackup()
{
    m_db->metadata()->setName("testSaveBackup");

    QFileInfo fileInfo(m_dbFilePath);
    QDateTime lastModified = fileInfo.lastModified();

    // open temporary file so it creates a filename
    TemporaryFile tmpFile;
    QVERIFY(tmpFile.open());
    QString tmpFileName = tmpFile.fileName();
    tmpFile.remove();

    // wait for modified timer
    QTRY_COMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), QString("testSaveBackup*"));

    fileDialog()->setNextFileName(tmpFileName);

    triggerAction("actionDatabaseSaveBackup");

    QTRY_COMPARE(m_tabWidget->tabText(m_tabWidget->currentIndex()), QString("testSaveBackup*"));

    checkDatabase(tmpFileName);

    fileInfo.refresh();
    QCOMPARE(fileInfo.lastModified(), lastModified);
    tmpFile.remove();
}

void TestGui::testSave()
{
    // Make a modification to the database then save
    m_db->metadata()->setName("testSave");
    checkSaveDatabase();
}

void TestGui::testSaveBackupPath_data()
{
    QTest::addColumn<QString>("backupFilePathPattern");
    QTest::addColumn<QString>("expectedBackupFile");

    // Absolute paths should remain absolute
    TemporaryFile tmpFile;
    QVERIFY(tmpFile.open());
    tmpFile.remove();

    QTest::newRow("Absolute backup path") << tmpFile.fileName() << tmpFile.fileName();
    // relative paths should be resolved to database parent directory
    QTest::newRow("Relative backup path (implicit)") << "other_dir/test.old.kdbx" << "other_dir/test.old.kdbx";
    QTest::newRow("Relative backup path (explicit)") << "./other_dir2/test2.old.kdbx" << "other_dir2/test2.old.kdbx";

    QTest::newRow("Path with placeholders") << "{DB_FILENAME}.old.kdbx" << "OpenVireo.old.kdbx";
    // empty path should be replaced with default pattern
    QTest::newRow("Empty path") << QString("") << config()->getDefault(Config::BackupFilePathPattern).toString();
    // {DB_FILENAME} should be replaced with database filename
    QTest::newRow("") << "{DB_FILENAME}_.old.kdbx" << "{DB_FILENAME}_.old.kdbx";
}

void TestGui::testSaveBackupPath()
{
    /**
     * Tests that the backupFilePathPattern config entry is respected. We do not test patterns like {TIME} etc here
     * as this is done in a separate test case. We do however check {DB_FILENAME} as this is a feature of the
     * performBackup() function.
     */

    // Get test data
    QFETCH(QString, backupFilePathPattern);
    QFETCH(QString, expectedBackupFile);

    // Enable automatic backups
    config()->set(Config::BackupBeforeSave, true);
    config()->set(Config::BackupFilePathPattern, backupFilePathPattern);

    // Replace placeholders and resolve relative paths. This cannot be done in the _data() function as the
    // db path/filename is not known yet
    auto dbFileInfo = QFileInfo(m_dbFilePath);
    if (!QDir::isAbsolutePath(expectedBackupFile)) {
        expectedBackupFile = QDir(dbFileInfo.absolutePath()).absoluteFilePath(expectedBackupFile);
    }
    expectedBackupFile.replace("{DB_FILENAME}", dbFileInfo.completeBaseName());

    // Save a modified database
    auto prevName = m_db->metadata()->name();
    m_db->metadata()->setName("testBackupPathPattern");
    checkSaveDatabase();

    // Test that the backup file has the previous database name
    checkDatabase(expectedBackupFile, prevName);

    // Clean up
    QFile(expectedBackupFile).remove();
}

void TestGui::testDatabaseSettings()
{
    m_db->metadata()->setName("testDatabaseSettings");
    triggerAction("actionDatabaseSettings");
    auto* dbSettingsDialog = m_mainWindow->findChild<QWidget*>("databaseSettingsDialog");
    QVERIFY(dbSettingsDialog);
    auto* dbSettingsCategoryList = dbSettingsDialog->findChild<CategoryListWidget*>("categoryList");
    QVERIFY(dbSettingsCategoryList);
    auto* dbSettingsStackedWidget = dbSettingsDialog->findChild<QStackedWidget*>("stackedWidget");
    QVERIFY(dbSettingsStackedWidget);
    auto* autosaveDelayCheckBox = dbSettingsDialog->findChild<QCheckBox*>("autosaveDelayCheckBox");
    QVERIFY(autosaveDelayCheckBox);
    auto* autosaveDelaySpinBox = dbSettingsDialog->findChild<QSpinBox*>("autosaveDelaySpinBox");
    QVERIFY(autosaveDelaySpinBox);
    // The settings open in a modal over the window, with Save and Cancel in its footer
    auto* dbSettingsModal = m_mainWindow->findChild<VaultModal*>("vaultDatabaseSettingsModal");
    QVERIFY(dbSettingsModal);
    QTRY_VERIFY(dbSettingsModal->isOpen());
    auto* dbSettingsSave = dbSettingsModal->findChild<QPushButton*>("vaultDatabaseSettingsSave");
    auto* dbSettingsCancel = dbSettingsModal->findChild<QPushButton*>("vaultDatabaseSettingsCancel");
    QVERIFY(dbSettingsSave);
    QVERIFY(dbSettingsCancel);
    int autosaveDelayTestValue = 2;

    dbSettingsCategoryList->setCurrentCategory(1); // go into security category
    auto securityTabWidget = dbSettingsStackedWidget->findChild<QTabWidget*>("securityTabWidget");
    QVERIFY(securityTabWidget);
    QCOMPARE(securityTabWidget->currentIndex(), 0);

    // Interact with the password edit option
    auto passwordEditWidget = securityTabWidget->findChild<PasswordEditWidget*>();
    QVERIFY(passwordEditWidget);
    auto editPasswordButton = passwordEditWidget->findChild<QPushButton*>("changeButton");
    QVERIFY(editPasswordButton);
    QVERIFY(editPasswordButton->isVisible());
    QTest::mouseClick(editPasswordButton, Qt::LeftButton);
    QApplication::processEvents();
    auto passwordWidgets = dbSettingsDialog->findChildren<PasswordWidget*>();
    QVERIFY(passwordWidgets.count() == 2);
    QVERIFY(passwordWidgets[0]->isVisible());
    passwordWidgets[0]->setText("b");
    passwordWidgets[1]->setText("b");

    // Toggle between tabs to ensure the password remains
    securityTabWidget->setCurrentIndex(1);
    QApplication::processEvents();
    securityTabWidget->setCurrentIndex(0);
    QApplication::processEvents();
    QCOMPARE(passwordWidgets[0]->text(), QString("b"));

    // Cancel password change and confirm password is cleared
    auto cancelPasswordButton = passwordEditWidget->findChild<QPushButton*>("cancelButton");
    QVERIFY(cancelPasswordButton);
    QTest::mouseClick(cancelPasswordButton, Qt::LeftButton);
    QApplication::processEvents();
    QVERIFY(!passwordWidgets[0]->isVisible());
    QCOMPARE(passwordWidgets[0]->text(), QString(""));
    QVERIFY(editPasswordButton->isVisible());

    // Switch to encryption tab and interact with various settings
    securityTabWidget->setCurrentIndex(1);
    QApplication::processEvents();

    // Verify database is KDBX3
    auto compatibilitySelection = securityTabWidget->findChild<QComboBox*>("compatibilitySelection");
    QVERIFY(compatibilitySelection);
    QVERIFY(compatibilitySelection->isEnabled());
    QCOMPARE(compatibilitySelection->currentText(), QString("KDBX 3"));

    // Verify advanced settings
    auto encryptionSettings = securityTabWidget->findChild<QTabWidget*>("encryptionSettingsTabWidget");
    QVERIFY(encryptionSettings);
    auto advancedTab = encryptionSettings->findChild<QWidget*>("advancedTab");
    QVERIFY(advancedTab);
    encryptionSettings->setCurrentWidget(advancedTab);
    QApplication::processEvents();

    // Verify KDF is AES KDBX3
    // The Vault Index layout moves the advanced settings into cards that follow the advanced tab
    auto kdfSelection = securityTabWidget->findChild<QComboBox*>("kdfComboBox");
    QVERIFY(kdfSelection);
    QVERIFY(kdfSelection->isVisible());
    QCOMPARE(kdfSelection->currentText(), QString("AES-KDF (KDBX 3)"));

    auto transformRoundsSpinBox = securityTabWidget->findChild<QSpinBox*>("transformRoundsSpinBox");
    QVERIFY(transformRoundsSpinBox);

    // Adjust compatibility to KDBX4 and wait for KDF to update
    compatibilitySelection->setCurrentIndex(0);
    QTRY_VERIFY(transformRoundsSpinBox->isEnabled());
    QCOMPARE(compatibilitySelection->currentText().left(6), QString("KDBX 4"));
    QCOMPARE(kdfSelection->currentText().left(7), QString("Argon2d"));

    // Switch to AES KDBX4, change rounds, then accept
    kdfSelection->setCurrentIndex(2);
    QCOMPARE(kdfSelection->currentText(), QString("AES-KDF (KDBX 4)"));
    transformRoundsSpinBox->setValue(123456);
    QTest::mouseClick(dbSettingsSave, Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());
    QTRY_COMPARE(m_db->kdf()->rounds(), 123456);
    QVERIFY(m_db->formatVersion() >= KeePass2::FILE_VERSION_4);
    QCOMPARE(m_db->kdf()->uuid(), KeePass2::KDF_AES_KDBX4);

    // Go back into database settings
    triggerAction("actionDatabaseSettings");
    QTRY_VERIFY(dbSettingsModal->isOpen());

    // test disable and default values for maximum history items and size
    auto* historyMaxItemsCheckBox = dbSettingsDialog->findChild<QCheckBox*>("historyMaxItemsCheckBox");
    auto* historyMaxItemsSpinBox = dbSettingsDialog->findChild<QSpinBox*>("historyMaxItemsSpinBox");
    auto* historyMaxSizeCheckBox = dbSettingsDialog->findChild<QCheckBox*>("historyMaxSizeCheckBox");
    auto* historyMaxSizeSpinBox = dbSettingsDialog->findChild<QSpinBox*>("historyMaxSizeSpinBox");
    // test defaults
    QCOMPARE(historyMaxItemsSpinBox->value(), Metadata::DefaultHistoryMaxItems);
    QCOMPARE(historyMaxSizeSpinBox->value(), qRound(Metadata::DefaultHistoryMaxSize / qreal(1024 * 1024)));
    // disable and test setting as well
    historyMaxItemsCheckBox->setChecked(false);
    historyMaxSizeCheckBox->setChecked(false);
    QTest::mouseClick(dbSettingsSave, Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());
    QTRY_COMPARE(m_db->metadata()->historyMaxItems(), -1);
    QTRY_COMPARE(m_db->metadata()->historyMaxSize(), -1);
    // then open to check the saved disabled state in gui
    triggerAction("actionDatabaseSettings");
    QTRY_VERIFY(dbSettingsModal->isOpen());
    QCOMPARE(historyMaxItemsCheckBox->isChecked(), false);
    QCOMPARE(historyMaxSizeCheckBox->isChecked(), false);
    QTest::mouseClick(dbSettingsCancel, Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());

    // Test loading default values and setting autosaveDelay
    triggerAction("actionDatabaseSettings");
    QTRY_VERIFY(dbSettingsModal->isOpen());
    QVERIFY(autosaveDelayCheckBox->isChecked() == false);
    autosaveDelayCheckBox->toggle();
    autosaveDelaySpinBox->setValue(autosaveDelayTestValue);
    QTest::mouseClick(dbSettingsSave, Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());
    QTRY_COMPARE(m_db->metadata()->autosaveDelayMin(), autosaveDelayTestValue);

    checkSaveDatabase();

    // Test loading autosaveDelay non-default values
    triggerAction("actionDatabaseSettings");
    QTRY_VERIFY(dbSettingsModal->isOpen());
    QTRY_COMPARE(autosaveDelayCheckBox->isChecked(), true);
    QTRY_COMPARE(autosaveDelaySpinBox->value(), autosaveDelayTestValue);
    QTest::mouseClick(dbSettingsCancel, Qt::LeftButton);
    QTRY_VERIFY(!dbSettingsModal->isOpen());

    // test autosave delay

    // 1 init
    config()->set(Config::AutoSaveAfterEveryChange, true);
    QSignalSpy writeDbSignalSpy(m_db.data(), &Database::databaseSaved);

    // 2 create new entries

    // 2.a) Click the new entry button and set the title
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    clickNewEntry();

    auto* editEntryWidget = this->editEntryWidget();
    QVERIFY(editEntryWidget);
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QVERIFY(titleEdit);

    QTest::keyClicks(titleEdit, "Test autosaveDelay 1");

    // 2.b) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    auto* newEntrySave = m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave");
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // 2.c) Make sure file was not modified yet
    Tools::wait(150); // due to modify timer
    QTRY_COMPARE(writeDbSignalSpy.count(), 0);

    // 2.d) Create second entry to test delay timer reset
    clickNewEntry();
    QTest::keyClicks(titleEdit, "Test autosaveDelay 2");

    // 2.e) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // 3 Double check both true negative and true positive
    // 3.a) Test unmodified prior to delay timeout
    Tools::wait(150); // due to modify timer
    QTRY_COMPARE(writeDbSignalSpy.count(), 0);

    // 3.b) Test modification time after expected
    m_dbWidget->triggerAutosaveTimer();
    QTRY_COMPARE(writeDbSignalSpy.count(), 1);

    // 4 Test no delay when disabled autosave or autosaveDelay
    // 4.a) create new entry
    clickNewEntry();
    QTest::keyClicks(titleEdit, "Test autosaveDelay 3");

    // 4.b) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // 4.c) Start timer
    Tools::wait(150); // due to modify timer

    // 4.d) Disable autosave
    config()->set(Config::AutoSaveAfterEveryChange, false);

    // 4.e) Make sure changes are not saved
    m_dbWidget->triggerAutosaveTimer();
    QTRY_COMPARE(writeDbSignalSpy.count(), 1);

    // 4.f) Repeat for autosaveDelay
    config()->set(Config::AutoSaveAfterEveryChange, true);
    clickNewEntry();
    QTest::keyClicks(titleEdit, "Test autosaveDelay 4");
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    Tools::wait(150); // due to modify timer
    m_db->metadata()->setAutosaveDelayMin(0);

    // 4.g) Make sure changes are not saved
    m_dbWidget->triggerAutosaveTimer();
    QTRY_COMPARE(writeDbSignalSpy.count(), 1);

    // 5 Cleanup
    config()->set(Config::AutoSaveAfterEveryChange, false);
}

void TestGui::testDatabaseLocking()
{
    QString origDbName = m_tabWidget->tabText(0);

    MessageBox::setNextAnswer(MessageBox::Cancel);
    triggerAction("actionLockAllDatabases");

    QCOMPARE(m_tabWidget->tabText(0), origDbName + " [Locked]");

    auto* actionDatabaseMerge = m_mainWindow->findChild<QAction*>("actionDatabaseMerge", Qt::FindChildrenRecursively);
    QCOMPARE(actionDatabaseMerge->isEnabled(), false);
    auto* actionDatabaseSave = m_mainWindow->findChild<QAction*>("actionDatabaseSave", Qt::FindChildrenRecursively);
    QCOMPARE(actionDatabaseSave->isEnabled(), false);

    DatabaseWidget* dbWidget = m_tabWidget->currentDatabaseWidget();
    QVERIFY(dbWidget->isLocked());
    auto* unlockDatabaseWidget = dbWidget->findChild<QWidget*>("databaseOpenWidget");
    QWidget* editPassword =
        unlockDatabaseWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);

    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);

    QVERIFY(!dbWidget->isLocked());
    QCOMPARE(m_tabWidget->tabText(0), origDbName);

    actionDatabaseMerge = m_mainWindow->findChild<QAction*>("actionDatabaseMerge", Qt::FindChildrenRecursively);
    QCOMPARE(actionDatabaseMerge->isEnabled(), true);
}

void TestGui::testDragAndDropKdbxFiles()
{
    const int openedDatabasesCount = m_tabWidget->count();

    const QString badDatabaseFilePath(QString(KEEPASSX_TEST_DATA_DIR).append("/NotDatabase.notkdbx"));
    const QString goodDatabaseFilePath(QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabase.kdbx"));

    QMimeData badMimeData;
    badMimeData.setUrls({QUrl::fromLocalFile(badDatabaseFilePath)});
    QDragEnterEvent badDragEvent(QPoint(1, 1), Qt::LinkAction, &badMimeData, Qt::LeftButton, Qt::NoModifier);
    qApp->notify(m_mainWindow.data(), &badDragEvent);
    QCOMPARE(badDragEvent.isAccepted(), false);

    QDropEvent badDropEvent(QPoint(1, 1), Qt::LinkAction, &badMimeData, Qt::LeftButton, Qt::NoModifier);
    qApp->notify(m_mainWindow.data(), &badDropEvent);
    QCOMPARE(badDropEvent.isAccepted(), false);

    QCOMPARE(m_tabWidget->count(), openedDatabasesCount);

    QMimeData goodMimeData;
    goodMimeData.setUrls({QUrl::fromLocalFile(goodDatabaseFilePath)});
    QDragEnterEvent goodDragEvent(QPoint(1, 1), Qt::LinkAction, &goodMimeData, Qt::LeftButton, Qt::NoModifier);
    qApp->notify(m_mainWindow.data(), &goodDragEvent);
    QCOMPARE(goodDragEvent.isAccepted(), true);

    QDropEvent goodDropEvent(QPoint(1, 1), Qt::LinkAction, &goodMimeData, Qt::LeftButton, Qt::NoModifier);
    qApp->notify(m_mainWindow.data(), &goodDropEvent);
    QCOMPARE(goodDropEvent.isAccepted(), true);

    QCOMPARE(m_tabWidget->count(), openedDatabasesCount + 1);

    MessageBox::setNextAnswer(MessageBox::No);
    triggerAction("actionDatabaseClose");

    QTRY_COMPARE(m_tabWidget->count(), openedDatabasesCount);
}

void TestGui::testSortGroups()
{
    // Groups are created in a modal over the window
    auto* editGroupWidget = m_mainWindow->findChild<EditGroupWidget*>("editGroupWidget");
    auto* nameEdit = editGroupWidget->findChild<QLineEdit*>("editName");
    auto createGroup = [this, nameEdit](const QString& name) {
        m_dbWidget->createGroup();
        auto* modal = m_mainWindow->findChild<VaultModal*>("vaultGroupModal");
        QVERIFY(modal);
        QTRY_VERIFY(modal->isOpen());
        QTest::keyClicks(nameEdit, name);
        QTest::mouseClick(modal->findChild<QPushButton*>("vaultGroupSave"), Qt::LeftButton);
        QTRY_VERIFY(!modal->isOpen());
    };

    // Create some sub-groups
    Group* rootGroup = m_db->rootGroup();
    Group* internetGroup = rootGroup->findGroupByPath("Internet");
    for (const auto& name : {"Google", "eBay", "Amazon", "Facebook"}) {
        m_dbWidget->groupView()->setCurrentGroup(internetGroup);
        createGroup(name);
    }
    m_dbWidget->groupView()->setCurrentGroup(rootGroup);

    triggerAction("actionGroupSortAsc");
    QList<Group*> children = rootGroup->children();
    QCOMPARE(children[0]->name(), QString("eMail"));
    QCOMPARE(children[1]->name(), QString("General"));
    QCOMPARE(children[2]->name(), QString("Homebanking"));
    QCOMPARE(children[3]->name(), QString("Internet"));
    QCOMPARE(children[4]->name(), QString("Network"));
    QCOMPARE(children[5]->name(), QString("Windows"));
    QList<Group*> subChildren = internetGroup->children();
    QCOMPARE(subChildren[0]->name(), QString("Amazon"));
    QCOMPARE(subChildren[1]->name(), QString("eBay"));
    QCOMPARE(subChildren[2]->name(), QString("Facebook"));
    QCOMPARE(subChildren[3]->name(), QString("Google"));

    triggerAction("actionGroupSortDesc");
    children = rootGroup->children();
    QCOMPARE(children[0]->name(), QString("Windows"));
    QCOMPARE(children[1]->name(), QString("Network"));
    QCOMPARE(children[2]->name(), QString("Internet"));
    QCOMPARE(children[3]->name(), QString("Homebanking"));
    QCOMPARE(children[4]->name(), QString("General"));
    QCOMPARE(children[5]->name(), QString("eMail"));
    subChildren = internetGroup->children();
    QCOMPARE(subChildren[0]->name(), QString("Google"));
    QCOMPARE(subChildren[1]->name(), QString("Facebook"));
    QCOMPARE(subChildren[2]->name(), QString("eBay"));
    QCOMPARE(subChildren[3]->name(), QString("Amazon"));

    m_dbWidget->groupView()->setCurrentGroup(internetGroup);
    triggerAction("actionGroupSortAsc");
    children = rootGroup->children();
    QCOMPARE(children[0]->name(), QString("Windows"));
    QCOMPARE(children[1]->name(), QString("Network"));
    QCOMPARE(children[2]->name(), QString("Internet"));
    QCOMPARE(children[3]->name(), QString("Homebanking"));
    QCOMPARE(children[4]->name(), QString("General"));
    QCOMPARE(children[5]->name(), QString("eMail"));
    subChildren = internetGroup->children();
    QCOMPARE(subChildren[0]->name(), QString("Amazon"));
    QCOMPARE(subChildren[1]->name(), QString("eBay"));
    QCOMPARE(subChildren[2]->name(), QString("Facebook"));
    QCOMPARE(subChildren[3]->name(), QString("Google"));

    m_dbWidget->groupView()->setCurrentGroup(rootGroup);
    triggerAction("actionGroupSortAsc");
    m_dbWidget->groupView()->setCurrentGroup(internetGroup);
    triggerAction("actionGroupSortDesc");
    children = rootGroup->children();
    QCOMPARE(children[0]->name(), QString("eMail"));
    QCOMPARE(children[1]->name(), QString("General"));
    QCOMPARE(children[2]->name(), QString("Homebanking"));
    QCOMPARE(children[3]->name(), QString("Internet"));
    QCOMPARE(children[4]->name(), QString("Network"));
    QCOMPARE(children[5]->name(), QString("Windows"));
    subChildren = internetGroup->children();
    QCOMPARE(subChildren[0]->name(), QString("Google"));
    QCOMPARE(subChildren[1]->name(), QString("Facebook"));
    QCOMPARE(subChildren[2]->name(), QString("eBay"));
    QCOMPARE(subChildren[3]->name(), QString("Amazon"));
}

void TestGui::testTrayRestoreHide()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QSKIP("QSystemTrayIcon::isSystemTrayAvailable() = false, skipping tray restore/hide test…");
    }

#ifndef Q_OS_MACOS
    m_mainWindow->hideWindow();
    QVERIFY(!m_mainWindow->isVisible());

    auto* trayIcon = m_mainWindow->findChild<QSystemTrayIcon*>();
    QVERIFY(trayIcon);

    trayIcon->activated(QSystemTrayIcon::Trigger);
    QTRY_VERIFY(m_mainWindow->isVisible());

    // Wait out window hide grace period before triggering tray icon again
    int gracePeriod = 250;
#ifdef Q_OS_WIN
    // Windows requires a shorter grace period
    gracePeriod = 50;
#endif

    Tools::wait(gracePeriod);
    trayIcon->activated(QSystemTrayIcon::Trigger);
    QTRY_VERIFY(!m_mainWindow->isVisible());

    trayIcon->activated(QSystemTrayIcon::MiddleClick);
    QTRY_VERIFY(m_mainWindow->isVisible());

    Tools::wait(gracePeriod);
    trayIcon->activated(QSystemTrayIcon::MiddleClick);
    QTRY_VERIFY(!m_mainWindow->isVisible());

    trayIcon->activated(QSystemTrayIcon::DoubleClick);
    QTRY_VERIFY(m_mainWindow->isVisible());

    Tools::wait(gracePeriod);
    trayIcon->activated(QSystemTrayIcon::DoubleClick);
    QTRY_VERIFY(!m_mainWindow->isVisible());

    // Ensure window is visible at the end
    trayIcon->activated(QSystemTrayIcon::DoubleClick);
    QTRY_VERIFY(m_mainWindow->isVisible());
#endif
}

void TestGui::testShortcutConfig()
{
    // Action collection should not be empty
    QVERIFY(!ActionCollection::instance()->actions().isEmpty());

    // Add an action, make sure it gets added
    QAction* a = new QAction(ActionCollection::instance());
    a->setObjectName("MyAction1");
    ActionCollection::instance()->addAction(a);
    QVERIFY(ActionCollection::instance()->actions().contains(a));

    const QKeySequence seq(Qt::CTRL | Qt::SHIFT | Qt::ALT | Qt::Key_N);
    ActionCollection::instance()->setDefaultShortcut(a, seq);
    QCOMPARE(ActionCollection::instance()->defaultShortcut(a), seq);

    bool v = false;
    m_mainWindow->addAction(a);
    connect(a, &QAction::triggered, ActionCollection::instance(), [&v] { v = !v; });
    QTest::keyClick(m_mainWindow.data(), Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    QVERIFY(v);

    // Change shortcut and save
    const QKeySequence newSeq(Qt::CTRL | Qt::SHIFT | Qt::ALT | Qt::Key_M);
    a->setShortcut(newSeq);
    QVERIFY(a->shortcut() != ActionCollection::instance()->defaultShortcut(a));
    ActionCollection::instance()->saveShortcuts();
    QCOMPARE(a->shortcut(), newSeq);
    const auto shortcuts = Config::instance()->getShortcuts();
    Config::ShortcutEntry entryForA;
    for (const auto& s : shortcuts) {
        if (s.name == a->objectName()) {
            entryForA = s;
            break;
        }
    }
    QCOMPARE(entryForA.name, a->objectName());
    QCOMPARE(QKeySequence::fromString(entryForA.shortcut), a->shortcut());

    // trigger the old shortcut
    QTest::keyClick(m_mainWindow.data(), Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    QVERIFY(v); // value of v should not change
    QTest::keyClick(m_mainWindow.data(), Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    QVERIFY(!v);
    disconnect(a, nullptr, nullptr, nullptr);
}

void TestGui::testAutoType()
{
    // Clear entries from root group to guarantee order
    for (Entry* entry : m_db->rootGroup()->entries()) {
        m_db->rootGroup()->removeEntry(entry);
    }
    Tools::wait(150);

    // 1. Create an entry with Auto-Type disabled

    // 1.a) Click the new entry button and set the title
    auto* entryNewAction = m_mainWindow->findChild<QAction*>("actionEntryNew");
    QVERIFY(entryNewAction->isEnabled());

    clickNewEntry();

    auto* editEntryWidget = this->editEntryWidget();
    QVERIFY(editEntryWidget);

    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    QVERIFY(titleEdit);

    QTest::keyClicks(titleEdit, "1. Entry With Disabled Auto-Type");

    auto* usernameComboBox = editEntryWidget->findChild<QComboBox*>("usernameComboBox");
    QVERIFY(usernameComboBox);

    QTest::mouseClick(usernameComboBox, Qt::LeftButton);
    QTest::keyClicks(usernameComboBox, "AutocompletionUsername");

    // 1.b) Uncheck Auto-Type checkbox
    editEntryWidget->switchToPage(EditEntryWidget::Page::AutoType);
    auto* enableAutoTypeButton = editEntryWidget->findChild<QAbstractButton*>("enableButton");
    QVERIFY(enableAutoTypeButton);
    QVERIFY(enableAutoTypeButton->isVisible());
    QVERIFY(enableAutoTypeButton->isEnabled());

    enableAutoTypeButton->click();
    QVERIFY(!enableAutoTypeButton->isChecked());

    // 1.c) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    auto* newEntrySave = m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave");
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // 2. Create an entry with default/inherited Auto-Type sequence

    // 2.a) Click the new entry button and set the title
    clickNewEntry();
    QTest::keyClicks(titleEdit, "2. Entry With Default Auto-Type Sequence");
    QTest::mouseClick(usernameComboBox, Qt::LeftButton);
    QTest::keyClicks(usernameComboBox, "AutocompletionUsername");

    // 2.b) Confirm AutoType is enabled and default
    editEntryWidget->switchToPage(EditEntryWidget::Page::AutoType);
    QVERIFY(enableAutoTypeButton->isChecked());
    auto* inheritSequenceButton = editEntryWidget->findChild<QRadioButton*>("inheritSequenceButton");
    QVERIFY(inheritSequenceButton->isChecked());

    // 2.c) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // 3. Create an entry with custom Auto-Type sequence

    // 3.a) Click the new entry button and set the title
    clickNewEntry();
    QTest::keyClicks(titleEdit, "3. Entry With Custom Auto-Type Sequence");
    QTest::mouseClick(usernameComboBox, Qt::LeftButton);
    QTest::keyClicks(usernameComboBox, "AutocompletionUsername");

    // 3.b) Confirm AutoType is enabled and set custom sequence
    editEntryWidget->switchToPage(EditEntryWidget::Page::AutoType);
    QVERIFY(enableAutoTypeButton->isChecked());
    auto* customSequenceButton = editEntryWidget->findChild<QRadioButton*>("customSequenceButton");
    QTest::mouseClick(customSequenceButton, Qt::LeftButton);
    QVERIFY(customSequenceButton->isChecked());
    QVERIFY(!inheritSequenceButton->isChecked());
    auto* sequenceEdit = editEntryWidget->findChild<QLineEdit*>("sequenceEdit");
    QVERIFY(sequenceEdit);
    sequenceEdit->setFocus();
    QTRY_VERIFY(sequenceEdit->hasFocus());
    QTest::keyClicks(sequenceEdit, "{USERNAME}{TAB}{TAB}{PASSWORD}{ENTER}");

    // 3.c) Save changes
    editEntryWidget->switchToPage(EditEntryWidget::Page::Main);
    QTest::mouseClick(newEntrySave, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QApplication::processEvents();

    // Check total number of entries matches expected
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    QVERIFY(entryView);
    QTRY_COMPARE(entryView->model()->rowCount(), 3);

    // Sort entries by title
    entryView->sortByColumn(1, Qt::AscendingOrder);

    // Select first entry
    entryView->selectionModel()->clearSelection();
    QModelIndex entryIndex = entryView->model()->index(0, 0);
    entryView->setCurrentIndex(entryIndex);

    // The Vault Index layout has no Auto-Type preview tab; the Auto-Type menu action follows the entry
    auto* autoTypeAction = m_mainWindow->findChild<QAction*>("actionEntryAutoType");
    QVERIFY(autoTypeAction);

    // Check that Auto-Type is disabled for the entry with disabled Auto-Type
    Entry* entry = entryView->entryFromIndex(entryIndex);
    QCOMPARE(entry->title(), QString("1. Entry With Disabled Auto-Type"));
    QVERIFY(!entry->autoTypeEnabled());
    QTRY_VERIFY(!autoTypeAction->isEnabled());

    // Select second entry
    entryView->selectionModel()->clearSelection();
    entryIndex = entryView->model()->index(1, 0);
    entryView->setCurrentIndex(entryIndex);

    // Check that Auto-Type is enabled for the entry with default Auto-Type sequence
    entry = entryView->entryFromIndex(entryIndex);
    QCOMPARE(entry->title(), QString("2. Entry With Default Auto-Type Sequence"));
    QVERIFY(entry->autoTypeEnabled());
    QCOMPARE(entry->defaultAutoTypeSequence(), QString());
    QTRY_VERIFY(autoTypeAction->isEnabled());

    // Select third entry
    entryView->selectionModel()->clearSelection();
    entryIndex = entryView->model()->index(2, 0);
    entryView->setCurrentIndex(entryIndex);

    // Check that Auto-Type is enabled for the entry with custom Auto-Type sequence
    entry = entryView->entryFromIndex(entryIndex);
    QCOMPARE(entry->title(), QString("3. Entry With Custom Auto-Type Sequence"));
    QVERIFY(entry->autoTypeEnabled());
    QCOMPARE(entry->defaultAutoTypeSequence(), QString("{USERNAME}{TAB}{TAB}{PASSWORD}{ENTER}"));
    QTRY_VERIFY(autoTypeAction->isEnabled());

    // De-select third entry
    entryView->selectionModel()->clearSelection();
}

void TestGui::testMenuActionStates()
{
    auto isActionEnabled = [this](const QString& actionName) -> bool {
        auto action = m_mainWindow->findChild<QAction*>(actionName);
        if (!action) {
            QTest::qFail(qPrintable(QString("Invalid action specified: %1").arg(actionName)), __FILE__, __LINE__);
            return false;
        }
        return action->isEnabled();
    };

    // Start with database open and unlocked
    qInfo("Actions Test: Database open and unlocked");

    QVERIFY(isActionEnabled("actionEntryNew"));
    QVERIFY(isActionEnabled("actionGroupNew"));
    QVERIFY(isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(isActionEnabled("actionDatabaseClose"));
    QVERIFY(isActionEnabled("actionDatabaseMerge"));
    QVERIFY(isActionEnabled("actionDatabaseSettings"));
    QVERIFY(isActionEnabled("actionReports"));
    QVERIFY(isActionEnabled("actionLockDatabase"));
    QVERIFY(isActionEnabled("actionLockAllDatabases"));
    QVERIFY(isActionEnabled("actionImport"));
    QVERIFY(isActionEnabled("actionExportCsv"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Edit entry actions
    qInfo("Actions Test: Editing an entry");

    triggerAction("actionEntryEdit");

    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(isActionEnabled("actionEntryCopyUsername"));
    QVERIFY(!isActionEnabled("actionEntrySetupTotp"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(isActionEnabled("actionDatabaseClose"));
    QVERIFY(!isActionEnabled("actionDatabaseMerge"));
    QVERIFY(!isActionEnabled("actionDatabaseSettings"));
    QVERIFY(!isActionEnabled("actionReports"));
    QVERIFY(isActionEnabled("actionLockDatabase"));
    QVERIFY(isActionEnabled("actionLockAllDatabases"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Special Case - Recycle Bin
    qInfo("Actions Test: Special case - Recycle Bin");

    m_dbWidget->switchToMainView();
    QApplication::processEvents();

    QVERIFY(m_db->metadata()->recycleBinEnabled());
    triggerAction("actionEntryDelete");
    m_dbWidget->groupView()->setCurrentGroup(m_db->metadata()->recycleBin());
    QVERIFY(m_dbWidget->isRecycleBinSelected());
    QVERIFY(isActionEnabled("actionEntryRestore"));
    QVERIFY(isActionEnabled("actionGroupEmptyRecycleBin"));
    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(!isActionEnabled("actionEntryClone"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(!isActionEnabled("actionGroupClone"));

    // Database Settings
    qInfo("Actions Test: Database settings");
    triggerAction("actionDatabaseSettings");

    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(!isActionEnabled("actionEntrySetupTotp"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(isActionEnabled("actionDatabaseClose"));
    QVERIFY(!isActionEnabled("actionDatabaseMerge"));
    QVERIFY(isActionEnabled("actionDatabaseSettings"));
    QVERIFY(isActionEnabled("actionDatabaseSecurity"));
    QVERIFY(!isActionEnabled("actionReports"));
    QVERIFY(isActionEnabled("actionLockDatabase"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Database Reports
    qInfo("Actions Test: Database reports");

    triggerAction("actionDatabaseSettings");
    triggerAction("actionReports");

    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(!isActionEnabled("actionEntrySetupTotp"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(isActionEnabled("actionDatabaseClose"));
    QVERIFY(!isActionEnabled("actionDatabaseMerge"));
    QVERIFY(!isActionEnabled("actionDatabaseSettings"));
    QVERIFY(!isActionEnabled("actionDatabaseSecurity"));
    QVERIFY(isActionEnabled("actionReports"));
    QVERIFY(isActionEnabled("actionLockDatabase"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Application Settings
    qInfo("Actions Test: Application settings");

    triggerAction("actionSettings");

    QVERIFY(!isActionEnabled("actionDatabaseSettings"));
    QVERIFY(!isActionEnabled("actionDatabaseSecurity"));
    QVERIFY(!isActionEnabled("actionReports"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Locked Database
    qInfo("Actions Test: Database locked");

    triggerAction("actionSettings");
    MessageBox::setNextAnswer(MessageBox::Discard);
    triggerAction("actionLockDatabase");

    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(!isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(isActionEnabled("actionDatabaseClose"));
    QVERIFY(!isActionEnabled("actionDatabaseMerge"));
    QVERIFY(!isActionEnabled("actionDatabaseSettings"));
    QVERIFY(!isActionEnabled("actionReports"));
    QVERIFY(!isActionEnabled("actionLockDatabase"));
    QVERIFY(!isActionEnabled("actionLockAllDatabases"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));

    // Welcome Screen
    qInfo("Actions Test: Welcome screen");

    triggerAction("actionDatabaseClose");

    QVERIFY(!isActionEnabled("actionEntryNew"));
    QVERIFY(!isActionEnabled("actionGroupNew"));
    QVERIFY(!isActionEnabled("actionDatabaseSaveAs"));
    QVERIFY(!isActionEnabled("actionDatabaseClose"));
    QVERIFY(!isActionEnabled("actionDatabaseMerge"));
    QVERIFY(!isActionEnabled("actionDatabaseSettings"));
    QVERIFY(!isActionEnabled("actionReports"));
    QVERIFY(!isActionEnabled("actionLockDatabase"));
    QVERIFY(!isActionEnabled("actionLockAllDatabases"));
    QVERIFY(isActionEnabled("actionImport"));
    QVERIFY(isActionEnabled("actionSettings"));
    QVERIFY(isActionEnabled("actionPasswordGenerator"));
}

void TestGui::testDeleteEntryDuringModalDialog()
{
    // Delete key in native file dialogs on macOS
    // should not delete password entries

    // Add canned entries for consistent testing
    addCannedEntries();

    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    auto* entryDeleteAction = m_mainWindow->findChild<QAction*>("actionEntryDelete");

    // Count initial entries
    int initialEntryCount = entryView->model()->rowCount();
    QVERIFY(initialEntryCount > 0);

    // Select the first entry
    clickIndex(entryView->model()->index(0, 1), entryView, Qt::LeftButton);
    entryView->setFocus();
    QApplication::processEvents();

    // Create and show a modal dialog to simulate the file save dialog
    QDialog modalDialog(m_mainWindow.data());
    modalDialog.setModal(true);

    // Use a timer to trigger the delete action while modal dialog is shown
    bool deleteTriggered = false;
    QTimer::singleShot(50, [&]() {
        // Verify modal dialog is active
        QVERIFY(QApplication::activeModalWidget() == &modalDialog);

        // Trigger delete action while modal is open
        entryDeleteAction->trigger();
        deleteTriggered = true;

        // Close the modal dialog
        modalDialog.accept();
    });

    // Show modal dialog (blocks until closed)
    modalDialog.exec();

    QVERIFY(deleteTriggered);
    QApplication::processEvents();

    // Verify entry count unchanged - delete should have been blocked
    QCOMPARE(entryView->model()->rowCount(), initialEntryCount);

    // Now verify normal deletion still works after modal closes
    clickIndex(entryView->model()->index(0, 1), entryView, Qt::LeftButton);
    entryView->setFocus();
    QApplication::processEvents();

    if (!config()->get(Config::Security_NoConfirmMoveEntryToRecycleBin).toBool()) {
        MessageBox::setNextAnswer(MessageBox::Move);
    }
    entryDeleteAction->trigger();
    QApplication::processEvents();

    // Verify entry was deleted normally
    QCOMPARE(entryView->model()->rowCount(), initialEntryCount - 1);
}

void TestGui::addCannedEntries()
{
    // Find the entry editor
    auto* editEntryWidget = this->editEntryWidget();
    auto* titleEdit = editEntryWidget->findChild<QLineEdit*>("titleEdit");
    auto* passwordEdit =
        editEntryWidget->findChild<PasswordWidget*>("passwordEdit")->findChild<QLineEdit*>("passwordEdit");
    auto save = [this] {
        QTest::mouseClick(m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave"), Qt::LeftButton);
        QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    };

    // Add entry "test" and confirm added
    clickNewEntry();
    QTest::keyClicks(titleEdit, "test");
    auto* editEntryWidgetTagsEdit = editEntryWidget->findChild<TagsEdit*>("tagsList");
    editEntryWidgetTagsEdit->tags(QStringList() << "testTag");
    save();

    // Add entry "something 2"
    clickNewEntry();
    QTest::keyClicks(titleEdit, "something 2");
    QTest::keyClicks(passwordEdit, "something 2");
    save();

    // Add entry "something 3"
    clickNewEntry();
    QTest::keyClicks(titleEdit, "something 3");
    save();
}

void TestGui::checkDatabase(const QString& filePath, const QString& expectedDbName)
{
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("a"));
    auto dbSaved = QSharedPointer<Database>::create();
    QVERIFY(dbSaved->open(filePath, key, nullptr));
    QCOMPARE(dbSaved->metadata()->name(), expectedDbName);
}

void TestGui::checkDatabase(const QString& filePath)
{
    checkDatabase(filePath.isEmpty() ? m_dbFilePath : filePath, m_db->metadata()->name());
}

void TestGui::checkSaveDatabase()
{
    // Attempt to save the database up to two times to overcome transient file errors
    QTRY_VERIFY(m_db->isModified());
    QTRY_VERIFY(m_tabWidget->tabText(m_tabWidget->currentIndex()).endsWith("*"));
    int i = 0;
    do {
        triggerAction("actionDatabaseSave");
        if (!m_db->isModified()) {
            checkDatabase();
            return;
        }
        QWARN("Failed to save database, trying again...");
        Tools::wait(250);
    } while (++i < 2);

    QFAIL("Could not save database.");
}

void TestGui::checkEntryCount(int count)
{
    QApplication::processEvents();
    auto* entryView = m_tabWidget->currentDatabaseWidget()->entryView();
    QTRY_COMPARE(entryView->model()->rowCount(), count);
}

void TestGui::triggerAction(const QString& name)
{
    auto* action = m_mainWindow->findChild<QAction*>(name);
    QVERIFY2(action, qPrintable(QString("Action doesn't exist: %1").arg(name)));
    QVERIFY2(action->isEnabled(), qPrintable(QString("Action is disabled: %1").arg(name)));
    action->trigger();
    QApplication::processEvents();
}

EditEntryWidget* TestGui::editEntryWidget() const
{
    // The editor lives in the Vault Index edit card, which moves into a modal over the window for new entries.
    // The reports keep an entry editor of their own, so don't just look for the name.
    auto* card = m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
    return card ? qobject_cast<EditEntryWidget*>(card->editor()) : nullptr;
}

// The Vault Index command bar button bound to a main window action
QAbstractButton* TestGui::commandButton(const QString& actionName) const
{
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    if (!commandBar) {
        return nullptr;
    }
    if (actionName == "actionEntryNew") {
        return commandBar->findChild<QAbstractButton*>("vaultNewEntryButton");
    }
    static const QHash<QString, QString> toolTips = {{"actionEntryEdit", "Edit entry"},
                                                     {"actionEntryDelete", "Delete entry"},
                                                     {"actionLockDatabase", "Lock database"},
                                                     {"actionEntryAutoTypeSequence", "Perform Auto-Type"}};
    for (auto* button : commandBar->findChildren<QAbstractButton*>()) {
        if (button->toolTip() == toolTips.value(actionName)) {
            return button;
        }
    }
    return nullptr;
}

// Click New Entry in the command bar and wait for the new entry modal
void TestGui::clickNewEntry()
{
    auto* button = commandButton("actionEntryNew");
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QVERIFY(button->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* modal = m_mainWindow->findChild<VaultModal*>("vaultNewEntryModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
}

// Click Edit in the command bar and wait for the editor to take the detail card's place
void TestGui::clickEditEntry()
{
    auto* button = commandButton("actionEntryEdit");
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QVERIFY(button->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    QTRY_VERIFY(m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard")->isVisible());
}

void TestGui::dragAndDropGroup(const QModelIndex& sourceIndex,
                               const QModelIndex& targetIndex,
                               int row,
                               bool expectedResult,
                               const QString& expectedParentName,
                               int expectedPos)
{
    QVERIFY(sourceIndex.isValid());
    QVERIFY(targetIndex.isValid());

    auto groupModel = qobject_cast<GroupModel*>(m_dbWidget->findChild<GroupView*>("groupView")->model());

    QMimeData mimeData;
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    Group* group = groupModel->groupFromIndex(sourceIndex);
    stream << group->database()->uuid() << group->uuid();
    mimeData.setData("application/x-keepassx-group", encoded);

    QCOMPARE(groupModel->dropMimeData(&mimeData, Qt::MoveAction, row, 0, targetIndex), expectedResult);
    QCOMPARE(group->parentGroup()->name(), expectedParentName);
    QCOMPARE(group->parentGroup()->children().indexOf(group), expectedPos);
}

void TestGui::clickIndex(const QModelIndex& index,
                         QAbstractItemView* view,
                         Qt::MouseButton button,
                         Qt::KeyboardModifiers stateKey)
{
    view->scrollTo(index);
    // The Vault Index entry list shows one column, so click the row when the index's column isn't shown
    auto rect = view->visualRect(index);
    if (rect.isEmpty()) {
        rect = view->visualRect(index.sibling(index.row(), 0));
    }
    QTest::mouseClick(view->viewport(), button, stateKey, rect.center());
}
