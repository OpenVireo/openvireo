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

#include "TestGuiVault.h"

#include "config-keepassx-tests.h"
#include "core/Config.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"
#include "gui/Application.h"
#include "gui/ApplicationSettingsWidget.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/MessageBox.h"
#include "gui/PasswordWidget.h"
#include "gui/SearchWidget.h"
#include "gui/entry/EntryView.h"
#include "gui/group/GroupModel.h"
#include "gui/group/GroupView.h"
#include "gui/vault/EntryDetailCard.h"
#include "gui/vault/VaultPanes.h"
#include "gui/vault/VaultSettingsModal.h"
#include "gui/vault/VaultWindowFrame.h"

#include <components/textfields/AutoSuggestBox.h>
#include <components/windowing/TitleBar.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QToolBar>

int main(int argc, char* argv[])
{
    Application app(argc, argv);
    app.setApplicationName("KeePassXC");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    QTEST_DISABLE_KEYPAD_NAVIGATION
    TestGuiVault tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    bool hasLabelWithText(QWidget* parent, const QString& text)
    {
        for (auto* label : parent->findChildren<QLabel*>()) {
            if (label->isVisibleTo(parent) && label->text() == text) {
                return true;
            }
        }
        return false;
    }
} // namespace

void TestGuiVault::initTestCase()
{
    QVERIFY(Crypto::init());
    // A temporary local config too, so databases opened by a developer's own build don't reopen here
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    QLocale::setDefault(QLocale::c());
    Application::bootstrap();

    config()->set(Config::GUI_VaultLayout, true);
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    config()->set(Config::UpdateCheckMessageShown, true);
    config()->set(Config::Security_QuickUnlock, false);
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock, false);

    m_mainWindow.reset(new MainWindow());
    // Screenshots need capture allowed, and offscreen windows can't block it anyway
    m_mainWindow->setAllowScreenCapture(true);
    m_tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_mainWindow->resize(1180, 720);
    m_mainWindow->show();
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    QVERIFY(m_dbFile.copyFromFile(QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx")));
    fileDialog()->setNextFileName(m_dbFile.fileName());
    m_mainWindow->findChild<QAction*>("actionDatabaseOpen")->trigger();
    QApplication::processEvents();

    m_dbWidget = m_tabWidget->currentDatabaseWidget();
    auto* openWidget = m_dbWidget->findChild<QWidget*>("databaseOpenWidget");
    QVERIFY(openWidget);
    auto* editPassword = openWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    QVERIFY(editPassword);
    editPassword->setFocus();
    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);
    QTRY_VERIFY(!m_dbWidget->isLocked());
    m_db = m_dbWidget->database();

    populate();
    selectGroup(m_workGroup);
}

void TestGuiVault::cleanupTestCase()
{
    m_db->markAsClean();
    MessageBox::setNextAnswer(MessageBox::No);
    m_mainWindow->findChild<QAction*>("actionDatabaseClose")->trigger();
    QApplication::processEvents();
    MessageBox::setNextAnswer(MessageBox::NoButton);
    m_dbFile.remove();
}

// Example content matching the Vault Index concept: a Work group with a TOTP
// entry, an expired entry and a weak password
void TestGuiVault::populate()
{
    m_workGroup = new Group();
    m_workGroup->setUuid(QUuid::createUuid());
    m_workGroup->setName("Work");
    m_workGroup->setParent(m_db->rootGroup());

    auto addEntry = [this](const QString& title, const QString& username, const QString& url, const QString& password) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setUsername(username);
        entry->setUrl(url);
        entry->setPassword(password);
        entry->setGroup(m_workGroup);
        return entry;
    };

    m_github = addEntry("GitHub", "bongani", "https://github.com/login", "c7#Lq9!vRz2@pWm8xKe4");
    m_github->setTotp(Totp::createSettings("JBSWY3DPEHPK3PXP"));
    m_github->setTags("work;2fa");
    m_github->setNotes("Recovery codes are attached. Passkey also registered on the YubiKey.");
    addEntry("Atlassian Cloud", "bongani@example.com", "https://id.atlassian.com", "Vt8$kq2#Nw5!zRp7Lc");
    auto* vpn = addEntry("Home VPN", "bongani", "https://vpn.home.lan", "Mx4!qZ8#tR2wLp6v");
    vpn->setExpires(true);
    vpn->setExpiryTime(QDateTime::currentDateTimeUtc().addDays(-3));
    addEntry("Postgres (staging)", "app_rw", "db-staging.acme.internal:5432", "staging1");
    addEntry("Slack - Acme", "bongani@acme.example", "https://acme.slack.com", "Hq7#vN3!pX9zKt2w");
    addEntry("Docker Hub", "bongani", "https://hub.docker.com", "Rz5!mK8#wQ2tVp4x");
}

void TestGuiVault::selectGroup(Group* group)
{
    auto* groupView = m_dbWidget->findChild<GroupView*>("groupView");
    groupView->setCurrentGroup(group);
    QApplication::processEvents();
}

void TestGuiVault::selectEntry(Entry* entry)
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    entryView->setCurrentEntry(entry);
    QApplication::processEvents();
}

void TestGuiVault::testLayout()
{
    QVERIFY(m_dbWidget->findChild<VaultNavPane*>());
    QVERIFY(m_dbWidget->findChild<VaultCommandBar*>());
    QVERIFY(m_dbWidget->findChild<EntryDetailCard*>());

    // Commands move out of the toolbar; search moves into the header
    auto* toolBar = m_mainWindow->findChild<QToolBar*>("toolBar");
    QVERIFY(toolBar->isHidden());
    auto* header = m_mainWindow->findChild<VaultHeaderBar*>();
    QVERIFY(header);
    QVERIFY(header->isVisible());
    QVERIFY(header->findChild<SearchWidget*>("SearchWidget"));

    // The classic preview panel never shows
    auto* preview = m_dbWidget->findChild<QWidget*>("previewWidget");
    QVERIFY(preview);
    QVERIFY(!preview->isVisible());
}

void TestGuiVault::testEntryListMode()
{
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    QVERIFY(entryView->isListMode());
    QVERIFY(entryView->header()->isHidden());
    // List mode has no column layout, so it must never overwrite the saved table layout
    QVERIFY(entryView->viewState().isEmpty());
    QVERIFY(m_dbWidget->entryViewState().isEmpty());
    QCOMPARE(entryView->model()->rowCount(), 6);
}

void TestGuiVault::testDetailCardFollowsSelection()
{
    auto* card = m_dbWidget->findChild<EntryDetailCard*>();
    selectEntry(m_github);
    QTRY_VERIFY(hasLabelWithText(card, "GitHub"));
    QVERIFY(hasLabelWithText(card, "bongani"));
    QVERIFY(hasLabelWithText(card, "https://github.com/login"));
    // Passwords stay hidden by default
    QVERIFY(!hasLabelWithText(card, m_github->password()));

    // Edits show up without reselecting
    m_github->setUsername("bongani-dev");
    QTRY_VERIFY(hasLabelWithText(card, "bongani-dev"));
    m_github->setUsername("bongani");
}

void TestGuiVault::testCommandBar()
{
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    QVERIFY(hasLabelWithText(commandBar, "Work"));
    QVERIFY(hasLabelWithText(commandBar, "6 entries"));

    // New Entry opens the entry editor, same as the toolbar action
    auto* newEntry = commandBar->findChild<QAbstractButton*>("vaultNewEntryButton");
    QVERIFY(newEntry);
    QVERIFY(newEntry->isEnabled());
    QTest::mouseClick(newEntry, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* editEntryWidget = m_dbWidget->findChild<QWidget*>("editEntryWidget");
    auto* buttonBox = editEntryWidget->findChild<QDialogButtonBox*>("buttonBox");
    QVERIFY(buttonBox);
    QTest::mouseClick(buttonBox->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestGuiVault::testSearchTitle()
{
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    m_dbWidget->search("docker");
    QTRY_VERIFY(hasLabelWithText(commandBar, "Search Results (1)"));
    QVERIFY(hasLabelWithText(commandBar, "1 entries"));
    m_dbWidget->endSearch();
    QTRY_VERIFY(hasLabelWithText(commandBar, "Work"));
}

// Saves Light and Dark captures when KPXC_VAULT_SCREENSHOT_DIR is set, for visual review
void TestGuiVault::testFluentSearchBox()
{
    auto* header = m_mainWindow->findChild<VaultHeaderBar*>();
    auto* box = header->findChild<fluent::textfields::AutoSuggestBox*>("searchEdit");
    QVERIFY(box);
    QVERIFY(box->isVisible());

    // Typing searches after the usual delay, Escape clears it
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    box->setFocus();
    QTest::keyClicks(box, "postgres");
    QTRY_VERIFY(hasLabelWithText(commandBar, "Search Results (1)"));
    QTest::keyClick(box, Qt::Key_Escape);
    QTRY_VERIFY(box->text().isEmpty());
    QTRY_VERIFY(hasLabelWithText(commandBar, "Work"));
}

void TestGuiVault::testDatabasePicker()
{
    auto* header = m_mainWindow->findChild<VaultHeaderBar*>();
    auto* picker = header->databasePicker();
    QVERIFY(picker->isVisible());
    QCOMPARE(picker->text(), m_dbWidget->displayName());

    // Open a second database and leave it locked
    TemporaryFile secondFile;
    QVERIFY(secondFile.copyFromFile(QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx")));
    fileDialog()->setNextFileName(secondFile.fileName());
    m_mainWindow->findChild<QAction*>("actionDatabaseOpen")->trigger();
    QApplication::processEvents();
    QCOMPARE(m_tabWidget->count(), 2);
    auto* second = m_tabWidget->currentDatabaseWidget();
    QVERIFY(second != m_dbWidget);
    QVERIFY(second->isLocked());

    // The picker replaces the tab bar and stays available while locked; search doesn't apply
    QVERIFY(m_tabWidget->tabBar()->isHidden());
    QVERIFY(header->isVisible());
    QCOMPARE(picker->text(), second->displayName());
    QVERIFY(!header->findChild<SearchWidget*>("SearchWidget")->isVisible());

    // Switch back from the picker's menu: open databases first, then database commands
    QStringList menuItems;
    bool firstChecked = true;
    QTimer::singleShot(0, this, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        for (auto* action : menu->actions()) {
            menuItems << (action->isSeparator() ? QStringLiteral("-") : action->objectName());
        }
        firstChecked = menu->actions().at(0)->isChecked();
        const auto dir = qEnvironmentVariable("KPXC_VAULT_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QTest::qWait(300);
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath("vault-picker.png"));
            menu->grab().save(QDir(dir).absoluteFilePath("vault-picker-menu.png"));
        }
        menu->actions().at(0)->trigger();
    });
    QTest::mouseClick(picker, Qt::LeftButton);
    QApplication::processEvents();

    QVERIFY(!firstChecked);
    QCOMPARE(menuItems.size(), 8);
    QCOMPARE(menuItems.mid(2),
             QStringList({"-", "actionDatabaseNew", "actionDatabaseOpen", "-", "actionLockAllDatabases",
                          "actionDatabaseClose"}));
    QCOMPARE(m_tabWidget->currentDatabaseWidget(), m_dbWidget.data());
    QCOMPARE(picker->text(), m_dbWidget->displayName());
    QVERIFY(header->findChild<SearchWidget*>("SearchWidget")->isVisible());

    // Close the second database again
    QVERIFY(m_tabWidget->closeDatabaseTab(second));
    QCOMPARE(m_tabWidget->count(), 1);
    QCOMPARE(picker->text(), m_dbWidget->displayName());
}

void TestGuiVault::testWindowFrame()
{
    if (!VaultWindowFrame::isSupported()) {
        QSKIP("The Fluent window frame replaces the native title bar on macOS only");
    }

    // The header sits in a Fluent title bar at the very top of the window, beside the window controls
    auto* titleBar = m_mainWindow->findChild<fluent::windowing::TitleBar*>("vaultTitleBar");
    QVERIFY(titleBar);
    auto* header = m_mainWindow->findChild<VaultHeaderBar*>();
    QCOMPARE(header->parentWidget(), titleBar);
    QCOMPARE(titleBar->mapTo(m_mainWindow.data(), QPoint(0, 0)).y(), 0);
    QTRY_VERIFY(titleBar->systemReservedLeadingWidth() > 0);
    QVERIFY(header->x() >= titleBar->systemReservedLeadingWidth());
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    QVERIFY(m_mainWindow->windowFlags().testFlag(Qt::ExpandedClientAreaHint));
#endif

    // The title bar stays (and can move the window) when no database is shown
    m_mainWindow->findChild<QAction*>("actionPasswordGenerator")->trigger();
    QApplication::processEvents();
    QVERIFY(!header->isVisible());
    QVERIFY(titleBar->isVisible());
    m_mainWindow->findChild<QAction*>("actionPasswordGenerator")->trigger();
    QApplication::processEvents();
    QVERIFY(header->isVisible());
}

void TestGuiVault::testToolbarSettingsHidden()
{
    m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
    auto* settingsWidget = m_mainWindow->findChild<ApplicationSettingsWidget*>();
    QVERIFY(settingsWidget);
    QTRY_VERIFY(settingsWidget->isVisible());

    // The Vault Index layout has no toolbar to show, style or move
    QVERIFY(settingsWidget->findChild<QWidget*>("toolbarShowCheckBox")->isHidden());
    QVERIFY(settingsWidget->findChild<QWidget*>("toolButtonStyleLabel")->isHidden());
    QVERIFY(settingsWidget->findChild<QWidget*>("toolButtonStyleComboBox")->isHidden());
    QVERIFY(settingsWidget->findChild<QWidget*>("toolbarMovableCheckBox")->isHidden());
    QVERIFY(!settingsWidget->findChild<QWidget*>("languageComboBox")->isHidden());

    QTest::keyClick(settingsWidget, Qt::Key_Escape);
    QTRY_VERIFY(!settingsWidget->isVisible());
}

void TestGuiVault::testSettingsModal()
{
    auto* modal = m_mainWindow->findChild<VaultSettingsModal*>("vaultSettingsModal");
    QVERIFY(modal);
    QVERIFY(!modal->isOpen());
    auto* settingsAction = m_mainWindow->findChild<QAction*>("actionSettings");
    auto* stackedWidget = m_mainWindow->centralWidget()->findChild<QStackedWidget*>("stackedWidget");
    const int screen = stackedWidget->currentIndex();

    settingsAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QVERIFY(settingsAction->isChecked());
    // The modal sits over the window; the database stays behind it but can't be used
    QCOMPARE(modal->geometry(), m_mainWindow->rect());
    QCOMPARE(stackedWidget->currentIndex(), screen);
    QVERIFY(m_dbWidget->isVisible());
    QVERIFY(!m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());
    QVERIFY(m_mainWindow->windowTitle().startsWith("Settings"));

    auto* settingsWidget = m_mainWindow->findChild<ApplicationSettingsWidget*>();
    QVERIFY(modal->card()->isAncestorOf(settingsWidget));
    QVERIFY(settingsWidget->findChild<QWidget*>("categoryList")->isHidden());
    QVERIFY(settingsWidget->findChild<QDialogButtonBox*>("buttonBox")->isHidden());

    // The sidebar drives the settings pages
    auto* navigation = modal->navigation();
    auto* categories = settingsWidget->findChild<QListWidget*>("categoryList");
    QCOMPARE(navigation->model()->rowCount(), categories->count());
    QCOMPARE(m_mainWindow->focusWidget(), navigation);
    const auto securityRect = navigation->visualRect(navigation->model()->index(1, 0));
    QTest::mouseClick(navigation->viewport(), Qt::LeftButton, {}, securityRect.center());
    QCOMPARE(categories->currentRow(), 1);
    QCOMPARE(settingsWidget->findChild<QStackedWidget*>("stackedWidget")->currentIndex(), 1);

    // Tab stays inside the modal
    for (int i = 0; i < 40; ++i) {
        QTest::keyClick(m_mainWindow->focusWidget(), Qt::Key_Tab);
        QVERIFY(modal->isAncestorOf(m_mainWindow->focusWidget()));
    }

    // Cancel closes it and brings the database back
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultSettingsCancel"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    QVERIFY(!settingsAction->isChecked());
    QVERIFY(m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());
    QVERIFY(!m_mainWindow->windowTitle().startsWith("Settings"));

    // So do Save and the close button
    settingsAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultSettingsSave"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    settingsAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultSettingsClose"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    categories->setCurrentRow(0);
}

void TestGuiVault::testAutoTypeSettingsPage()
{
    m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
    auto* settingsWidget = m_mainWindow->findChild<ApplicationSettingsWidget*>();
    QTRY_VERIFY(settingsWidget->isVisible());

    // Auto-Type is a category of its own rather than a tab on the General page
    QVERIFY(!settingsWidget->findChild<QTabWidget*>("generalSettingsTabWidget"));
    auto* categories = settingsWidget->findChild<QListWidget*>("categoryList");
    QCOMPARE(categories->item(0)->text(), QString("General"));
    QCOMPARE(categories->item(1)->text(), QString("Security"));
    QCOMPARE(categories->item(2)->text(), QString("Auto-Type"));

    categories->setCurrentRow(2);
    auto* pages = settingsWidget->findChild<QStackedWidget*>("stackedWidget");
    QCOMPARE(pages->currentIndex(), 2);
    auto* autoTypePage = pages->currentWidget()->findChild<QWidget*>("ApplicationSettingsWidgetAutoType");
    QVERIFY(autoTypePage);
    QVERIFY(autoTypePage->findChild<QWidget*>("autoTypeShortcutWidget"));
    QVERIFY(autoTypePage->findChild<QWidget*>("autoTypeAskCheckBox")->isVisibleTo(settingsWidget));

    categories->setCurrentRow(0);
    QTest::keyClick(settingsWidget, Qt::Key_Escape);
    QTRY_VERIFY(!settingsWidget->isVisible());
}

void TestGuiVault::testSidebarListsOnlyTags()
{
    // Searching happens in the header, so the sidebar lists tags without the searches
    auto* tagView = m_dbWidget->findChild<QListView*>("tagView");
    QVERIFY(tagView);
    QTRY_VERIFY(tagView->isVisible());
    QStringList rows;
    for (int row = 0; row < tagView->model()->rowCount(); ++row) {
        rows << tagView->model()->index(row, 0).data().toString();
    }
    QCOMPARE(rows, m_db->tagList());
    QVERIFY(!rows.contains("Clear Search"));
    QVERIFY(!tagView->selectionModel()->hasSelection());

    auto* tagWidget = m_dbWidget->findChild<QWidget*>("tagWidget");
    QVERIFY(tagWidget->findChildren<QLabel*>().first()->text() == QString("Tags"));

    // Clicking a tag still searches for it
    QTest::mouseClick(tagView->viewport(), Qt::LeftButton, {}, tagView->visualRect(tagView->model()->index(0, 0)).center());
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QVERIFY(m_dbWidget->getCurrentSearch().startsWith("tag:"));
    m_dbWidget->endSearch();
    QVERIFY(!tagView->selectionModel()->hasSelection());
    QVERIFY(!tagView->currentIndex().isValid());

    // The groups get the height the few tags don't need
    auto* groupView = m_dbWidget->findChild<QWidget*>("groupView");
    QCOMPARE(groupView->height() + tagWidget->height() + groupView->parentWidget()->style()->pixelMetric(
                 QStyle::PM_SplitterWidth, nullptr, groupView->parentWidget()),
             groupView->parentWidget()->height());
}

void TestGuiVault::testScreenshots()
{
    const auto dir = qEnvironmentVariable("KPXC_VAULT_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set KPXC_VAULT_SCREENSHOT_DIR to save screenshots");
    }

    selectGroup(m_workGroup);
    selectEntry(m_github);
    for (const auto& theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        config()->set(Config::GUI_ApplicationTheme, theme);
        kpxcApp->applyTheme();
        QApplication::processEvents();
        QTest::qWait(200);
        QVERIFY(m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("vault-%1.png").arg(theme))));
        m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
        QApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("vault-settings-%1.png").arg(theme))));
        m_mainWindow->findChild<ApplicationSettingsWidget*>()->findChild<QListWidget*>("categoryList")->setCurrentRow(2);
        QApplication::processEvents();
        QVERIFY(m_mainWindow->grab().save(
            QDir(dir).absoluteFilePath(QString("vault-settings-autotype-%1.png").arg(theme))));
        m_mainWindow->findChild<ApplicationSettingsWidget*>()->findChild<QListWidget*>("categoryList")->setCurrentRow(0);
        m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
        QApplication::processEvents();
        // Includes the native window controls; needs screen recording permission
        const auto frame = m_mainWindow->frameGeometry();
        m_mainWindow->screen()
            ->grabWindow(0, frame.x(), frame.y(), frame.width(), frame.height())
            .save(QDir(dir).absoluteFilePath(QString("vault-window-%1.png").arg(theme)));
    }
}
