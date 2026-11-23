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

#include "TestGuiBrowser.h"
#include "gui/Application.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QTest>

#include "browser/BrowserService.h"
#include "config-keepassx-tests.h"
#include "crypto/Crypto.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/FileDialog.h"
#include "gui/MessageBox.h"
#include "gui/PasswordWidget.h"
#include "gui/entry/EditEntryWidget.h"
#include "gui/entry/EntryView.h"
#include "gui/vault/VaultEntryEditCard.h"
#include "gui/vault/VaultResponsive.h"

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
    // The window can be narrower than the wide layout on CI screens; TestGuiVault covers the others
    Vault::setLayoutForTests(Vault::Layout::Wide);
    TestGuiBrowser tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

void TestGuiBrowser::initTestCase()
{
    QVERIFY(Crypto::init());
    // Create temporary config file
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), {});
    // Disable autosave so we can test the modified file indicator
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    // Enable the tray icon so we can test hiding/restoring the windowQByteArray
    config()->set(Config::GUI_ShowTrayIcon, true);
    // Disable the update check first time alert
    config()->set(Config::UpdateCheckMessageShown, true);
    // Disable quick unlock
    config()->set(Config::Security_QuickUnlock, false);

    // Today's local experience, without the account start screen
    config()->set(Config::Account_StartMode, "local");
    m_mainWindow.reset(new MainWindow());
    m_tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_mainWindow->show();

    QLocale::setDefault(QLocale::c());
}

// Every test starts with opening the temp database
void TestGuiBrowser::init()
{
    m_dbFile.reset(new TemporaryFile());
    m_dbFile->copyFromFile(QString(KEEPASSX_TEST_DATA_DIR).append("/NewDatabaseBrowser.kdbx"));

    // make sure window is activated or focus tests may fail
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    fileDialog()->setNextFileName(m_dbFile->fileName());
    triggerAction("actionDatabaseOpen");

    auto* databaseOpenWidget = m_tabWidget->currentDatabaseWidget()->findChild<QWidget*>("databaseOpenWidget");
    QVERIFY(databaseOpenWidget);
    auto* editPassword =
        databaseOpenWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);
    editPassword->setFocus();

    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);

    m_dbWidget = m_tabWidget->currentDatabaseWidget();
    m_db = m_dbWidget->database();
}

// Every test ends with closing the temp database without saving
void TestGuiBrowser::cleanup()
{
    // DO NOT save the database
    m_db->markAsClean();
    MessageBox::setNextAnswer(MessageBox::No);
    triggerAction("actionDatabaseClose");
    QApplication::processEvents();
    MessageBox::setNextAnswer(MessageBox::NoButton);

    if (m_dbWidget) {
        delete m_dbWidget;
    }

    m_dbFile->remove();
}

void TestGuiBrowser::cleanupTestCase()
{
    m_dbFile->remove();
}

void TestGuiBrowser::testEntrySettings()
{
    // Enable the Browser Integration
    config()->set(Config::Browser_Enabled, true);

    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // Select the first entry in the database
    Entry* entry = entryView->entryFromIndex(entryView->model()->index(0, 0));
    QVERIFY(entry);
    entryView->setCurrentEntry(entry);
    QTRY_COMPARE(m_dbWidget->currentSelectedEntry(), entry);

    // The entry opens for editing in place of its detail card
    triggerAction("actionEntryEdit");
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* card = m_dbWidget->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
    QVERIFY(card);
    QVERIFY(card->isVisible());
    auto* editEntryWidget = qobject_cast<EditEntryWidget*>(card->editor());
    QVERIFY(editEntryWidget);
    QCOMPARE(editEntryWidget->objectName(), QString("editEntryWidget"));

    // Switch to Properties page and select all rows from the custom data table
    editEntryWidget->setCurrentPage(5);
    auto customDataTableView = editEntryWidget->findChild<QTableView*>("customDataTable");
    QVERIFY(customDataTableView);
    QTest::mouseClick(customDataTableView, Qt::LeftButton);
    QTest::keyClick(customDataTableView, 'a', Qt::ControlModifier);

    // Remove the data
    QCOMPARE(entry->customData()->size(), 2);
    auto* removeButton = editEntryWidget->findChild<QPushButton*>("removeCustomDataButton");
    QVERIFY(removeButton);
    MessageBox::setNextAnswer(MessageBox::Delete);
    QTest::mouseClick(removeButton, Qt::LeftButton);

    // Save the removal from the command bar, which holds Save while an entry is edited in place
    auto* saveButton = m_dbWidget->findChild<QAbstractButton*>("vaultEditSaveButton");
    QVERIFY(saveButton);
    QTRY_VERIFY(saveButton->isVisible());
    QTRY_VERIFY(saveButton->isEnabled());
    QTest::mouseClick(saveButton, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    QCOMPARE(entry->customData()->size(), 0);
}

void TestGuiBrowser::testAdditionalURLs()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // Select the first entry in the database
    Entry* entry = entryView->entryFromIndex(entryView->model()->index(0, 0));
    QVERIFY(entry);
    entryView->setCurrentEntry(entry);
    QTRY_COMPARE(m_dbWidget->currentSelectedEntry(), entry);

    // The entry opens for editing in place of its detail card
    triggerAction("actionEntryEdit");
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* card = m_dbWidget->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
    QVERIFY(card);
    QVERIFY(card->isVisible());
    auto* editEntryWidget = qobject_cast<EditEntryWidget*>(card->editor());
    QVERIFY(editEntryWidget);
    QCOMPARE(editEntryWidget->objectName(), QString("editEntryWidget"));

    // Switch to Browser Integration page and add three URL's
    editEntryWidget->setCurrentPage(4);
    auto* addURLButton = editEntryWidget->findChild<QPushButton*>("addURLButton");
    QVERIFY(addURLButton);

    auto* urlList = editEntryWidget->findChild<QListView*>("additionalURLsView");
    QVERIFY(urlList);

    QStringList testURLs = {"https://example1.com", "https://example2.com", "https://example3.com"};

    // Each new row opens an editor for the URL; type into it directly so the test doesn't depend on which
    // window has the keyboard focus (other GUI tests may be running at the same time)
    for (const auto& url : testURLs) {
        QTest::mouseClick(addURLButton, Qt::LeftButton);
        QApplication::processEvents();
        QLineEdit* editor = nullptr;
        for (auto* edit : urlList->viewport()->findChildren<QLineEdit*>()) {
            if (!edit->isHidden()) {
                editor = edit;
            }
        }
        QVERIFY(editor);
        editor->selectAll();
        QTest::keyClicks(editor, url);
        QTest::keyClick(editor, Qt::Key_Enter);
        QApplication::processEvents();
    }

    // Check the values from attributesEdit
    editEntryWidget->setCurrentPage(1);
    auto* attributesView = editEntryWidget->findChild<QListView*>("attributesView");
    auto* attrTextEdit = editEntryWidget->findChild<QPlainTextEdit*>("attributesEdit");
    QVERIFY(attributesView);
    QVERIFY(attrTextEdit);
    // The URL attributes sort before the ones this entry already had
    QVERIFY(attributesView->model()->rowCount() >= testURLs.size());

    for (int i = 0; i < testURLs.size(); ++i) {
        attributesView->setCurrentIndex(attributesView->model()->index(i, 0));
        QCOMPARE(attrTextEdit->toPlainText(), testURLs.at(i));
    }
}

void TestGuiBrowser::testGetDatabaseGroups()
{
    auto result = browserService()->getDatabaseGroups();
    QCOMPARE(result.length(), 1);

    auto groups = result["groups"].toArray();
    auto first = groups.at(0);
    auto children = first.toObject()["children"].toArray();
    QCOMPARE(first.toObject()["name"].toString(), QString("NewDatabase"));
    QCOMPARE(children.size(), 6);

    auto firstChild = children.at(0).toObject();
    auto secondChild = children.at(1).toObject();
    QCOMPARE(firstChild["name"].toString(), QString("General"));
    QCOMPARE(secondChild["name"].toString(), QString("Windows"));

    auto subGroups = firstChild["children"].toArray();
    QCOMPARE(subGroups.count(), 1);
    auto subGroupObj = subGroups.at(0).toObject();
    QCOMPARE(subGroupObj["name"].toString(), QString("SubGroup"));
}

void TestGuiBrowser::triggerAction(const QString& name)
{
    auto* action = m_mainWindow->findChild<QAction*>(name);
    QVERIFY(action);
    QVERIFY(action->isEnabled());
    action->trigger();
    QApplication::processEvents();
}
