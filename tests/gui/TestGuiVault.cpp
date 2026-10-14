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
#include "core/Database.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"
#include "gui/Application.h"
#include "gui/ApplicationSettingsWidget.h"
#include "gui/DatabaseTabWidget.h"
#include "gui/DatabaseWidget.h"
#include "gui/FileDialog.h"
#include "gui/MainWindow.h"
#include "gui/PasswordGeneratorWidget.h"
#include "gui/MessageBox.h"
#include "gui/PasswordWidget.h"
#include "gui/SearchWidget.h"
#include "gui/TotpSetupDialog.h"
#include "gui/entry/EntryView.h"
#include "gui/group/GroupModel.h"
#include "gui/group/GroupView.h"
#include "gui/vault/EntryDetailCard.h"
#include "gui/vault/VaultEditShell.h"
#include "gui/vault/VaultEntryEditCard.h"
#include "gui/vault/VaultPanes.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultNewDatabaseModal.h"
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
#include <QComboBox>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QRadioButton>
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

VaultEntryEditCard* TestGuiVault::entryEditCard() const
{
    // New entries are edited in a modal over the window, so look beyond the database widget
    return m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
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
    auto* buttonBox = entryEditCard()->editor()->buttonBox();
    QVERIFY(buttonBox);
    QTest::mouseClick(buttonBox->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestGuiVault::testEditEntryInPlace()
{
    selectGroup(m_workGroup);
    selectEntry(m_github);
    auto* card = entryEditCard();
    auto* details = m_dbWidget->findChild<EntryDetailCard*>();
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");
    auto* navPane = m_dbWidget->findChild<VaultNavPane*>();
    QVERIFY(card);
    QVERIFY(!card->isVisible());

    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    QVERIFY(m_dbWidget->isEntryEditActive());
    QVERIFY(!m_dbWidget->isEntryViewActive());
    QCOMPARE(m_dbWidget->currentSelectedEntry(), m_github.data());

    // The editor takes the detail card's place; the groups and the list stay in view but wait
    QVERIFY(card->isVisible());
    QVERIFY(!details->isVisible());
    QVERIFY(entryView->isVisible());
    QVERIFY(!entryView->isEnabled());
    QVERIFY(navPane->isVisible());
    QVERIFY(!navPane->isEnabled());
    QVERIFY(m_dbWidget->isAncestorOf(card));
    if (auto* newEntryModal = m_mainWindow->findChild<VaultModal*>("vaultNewEntryModal")) {
        QVERIFY(!newEntryModal->isOpen());
    }

    // Cancel and Save replace the entry commands in the command bar, and there is no Apply since saving
    // returns to the details
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    auto* save = commandBar->findChild<QAbstractButton*>("vaultEditSaveButton");
    QVERIFY(save->isVisible());
    QVERIFY(commandBar->findChild<QAbstractButton*>("vaultEditCancelButton")->isVisible());
    QVERIFY(!commandBar->findChild<QAbstractButton*>("vaultNewEntryButton")->isVisible());
    QApplication::processEvents();
    auto* bar = card->findChild<VaultButtonBar*>();
    for (auto* button : bar->findChildren<QAbstractButton*>(Qt::FindDirectChildrenOnly)) {
        QVERIFY(!button->isVisible());
    }

    auto* title = card->findChild<QLineEdit*>("titleEdit");
    title->setText("GitHub Enterprise");
    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(m_github->title(), QString("GitHub Enterprise"));
    QVERIFY(details->isVisible());
    QVERIFY(!card->isVisible());
    QVERIFY(entryView->isEnabled());
    QVERIFY(navPane->isEnabled());
    QVERIFY(!save->isVisible());
    QVERIFY(commandBar->findChild<QAbstractButton*>("vaultNewEntryButton")->isVisible());
    QTRY_VERIFY(hasLabelWithText(details, "GitHub Enterprise"));
    m_github->setTitle("GitHub");
}

void TestGuiVault::testNewEntryModal()
{
    selectGroup(m_workGroup);
    const auto entriesBefore = m_workGroup->entries().size();
    auto* card = entryEditCard();
    auto* details = m_dbWidget->findChild<EntryDetailCard*>();
    auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>();
    QTest::mouseClick(commandBar->findChild<QAbstractButton*>("vaultNewEntryButton"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    QVERIFY(m_dbWidget->isEntryEditActive());

    // The new entry opens in a modal over the window, with Cancel and Save in its footer
    auto* modal = m_mainWindow->findChild<VaultModal*>("vaultNewEntryModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
    QCOMPARE(modal->geometry(), m_mainWindow->rect());
    QVERIFY(modal->card()->isAncestorOf(card));
    QVERIFY(card->isVisible());
    QVERIFY(details->isVisible());
    // The icon and the title stay at the top; the modal's own title says where the entry goes
    QVERIFY(card->findChild<QWidget*>("vaultEditCardHeader")->isVisible());
    QVERIFY(card->findChild<QWidget*>("vaultEditIconTile")->isVisible());
    QVERIFY(!card->findChild<QLabel*>("vaultEditSubtitle")->isVisible());
    QVERIFY(!commandBar->findChild<QAbstractButton*>("vaultEditSaveButton")->isVisible());
    auto* save = modal->findChild<QPushButton*>("vaultNewEntrySave");
    auto* cancel = modal->findChild<QPushButton*>("vaultNewEntryCancel");
    QVERIFY(save && cancel);
    QTRY_COMPARE(QApplication::focusWidget(), card->findChild<QLineEdit*>("titleEdit"));

    // It starts compact: notes, tags and expiry wait behind chips
    auto* notes = card->findChild<QPlainTextEdit*>("notesEdit");
    auto* tags = card->findChild<QWidget*>("tagsList");
    auto* expires = card->findChild<QWidget*>("expireCheck");
    QVERIFY(!notes->isVisible());
    QVERIFY(!tags->isVisible());
    QVERIFY(!expires->isVisible());
    QVERIFY(card->findChild<QWidget*>("vaultEditAddRow")->isVisible());

    // The title follows the site until the user types one
    auto* title = card->findChild<QLineEdit*>("titleEdit");
    auto* url = card->findChild<QLineEdit*>("urlEdit");
    QTest::keyClicks(url, "https://www.example.org/login");
    QCOMPARE(title->text(), QString("example.org"));
    title->setText("Example");
    QTest::keyClicks(url, "/");
    QCOMPARE(title->text(), QString("Example"));

    auto* addNotes = card->findChild<QAbstractButton*>("vaultEditAddnotesEdit");
    QVERIFY(addNotes->isVisible());
    QTest::mouseClick(addNotes, Qt::LeftButton);
    QVERIFY(notes->isVisible());
    QVERIFY(!addNotes->isVisible());
    QVERIFY(!tags->isVisible());
    notes->setPlainText("Created in place");

    QTest::mouseClick(save, Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QVERIFY(!modal->isOpen());
    QCOMPARE(m_workGroup->entries().size(), entriesBefore + 1);
    auto* created = m_dbWidget->currentSelectedEntry();
    QVERIFY(created);
    QCOMPARE(created->title(), QString("Example"));
    QCOMPARE(created->url(), QString("https://www.example.org/login/"));
    QCOMPARE(created->notes(), QString("Created in place"));

    // Editing stays in the detail pane, and shows every row whether or not it has a value
    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    QVERIFY(!modal->isOpen());
    QVERIFY(m_dbWidget->isAncestorOf(card));
    QVERIFY(card->isVisible());
    QVERIFY(!details->isVisible());
    QVERIFY(card->findChild<QWidget*>("vaultEditCardHeader")->isVisible());
    QVERIFY(card->findChild<QLabel*>("vaultEditSubtitle")->isVisible());
    QVERIFY(notes->isVisible());
    QVERIFY(tags->isVisible());
    QVERIFY(expires->isVisible());
    QVERIFY(!card->findChild<QWidget*>("vaultEditAddRow")->isVisible());
    QTest::mouseClick(card->editor()->buttonBox()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    delete created;
    QCOMPARE(m_workGroup->entries().size(), entriesBefore);

    // Setting a new entry aside, with Cancel or Escape, leaves nothing behind
    for (bool escape : {false, true}) {
        QTest::mouseClick(commandBar->findChild<QAbstractButton*>("vaultNewEntryButton"), Qt::LeftButton);
        QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
        QTRY_VERIFY(modal->isOpen());
        QVERIFY(!notes->isVisible());
        if (escape) {
            QTest::keyClick(modal, Qt::Key_Escape);
        } else {
            QTest::mouseClick(cancel, Qt::LeftButton);
        }
        QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
        QVERIFY(!modal->isOpen());
        QVERIFY(m_dbWidget->isAncestorOf(card));
        QCOMPARE(m_workGroup->entries().size(), entriesBefore);
    }
}

void TestGuiVault::testEditorSections()
{
    selectGroup(m_workGroup);
    selectEntry(m_github);
    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);

    // The other pages are listed under the Entry page and open in its place
    auto* card = entryEditCard();
    auto* sections = card->findChild<QWidget*>("vaultEditSections");
    QVERIFY(sections->isVisible());
    QAbstractButton* advanced = nullptr;
    for (auto* row : sections->findChildren<QAbstractButton*>()) {
        if (row->text() == "Advanced") {
            advanced = row;
        }
    }
    QVERIFY(advanced);
    auto* back = card->findChild<QAbstractButton*>("vaultEditBackButton");
    QVERIFY(!back->isVisible());

    QTest::mouseClick(advanced, Qt::LeftButton);
    QCOMPARE(card->editor()->pageStack()->currentIndex(), 1);
    QVERIFY(back->isVisible());
    QVERIFY(hasLabelWithText(card, "Advanced"));

    QTest::mouseClick(back, Qt::LeftButton);
    QCOMPARE(card->editor()->pageStack()->currentIndex(), 0);
    QVERIFY(!back->isVisible());

    QTest::mouseClick(card->editor()->buttonBox()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestGuiVault::testAdvancedPage()
{
    selectGroup(m_workGroup);
    m_dbWidget->createEntry();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* card = entryEditCard();
    card->editor()->findChild<QListWidget*>("categoryList")->setCurrentRow(1);

    // The page reads as settings cards, with the options as switches
    auto* advanced = card->findChild<QWidget*>("EditEntryWidgetAdvanced");
    QVERIFY(advanced->findChild<QWidget*>("vaultSettingsPageForm"));
    QVERIFY(!advanced->findChild<QWidget*>("vaultSettingsPageForm")->isVisible());
    QVERIFY(card->findChild<QWidget*>("excludeReportsCheckBox")->isVisible());
    QVERIFY(card->findChild<QWidget*>("fgColorButton")->isVisible());

    // A new entry has no attributes: a note stands in for the empty list and value until one is added
    auto* none = card->findChild<QLabel*>("vaultNoAttributesLabel");
    auto* view = card->findChild<QAbstractItemView*>("attributesView");
    auto* value = card->findChild<QPlainTextEdit*>("attributesEdit");
    QVERIFY(none->isVisible());
    QVERIFY(!view->isVisible());
    QVERIFY(!value->isVisible());
    QTest::mouseClick(card->findChild<QAbstractButton*>("addAttributeButton"), Qt::LeftButton);
    QVERIFY(!none->isVisible());
    QVERIFY(view->isVisible());
    QVERIFY(value->isVisible());
    QVERIFY(view->y() < value->y());

    // The attachment buttons sit in a row above the attachments
    auto* attachments = card->findChild<QWidget*>("attachmentsView");
    auto* addFile = card->findChild<QWidget*>("addAttachmentButton");
    auto* removeFile = card->findChild<QWidget*>("removeAttachmentButton");
    QVERIFY(addFile->mapTo(card, QPoint()).y() < attachments->mapTo(card, QPoint()).y());
    QCOMPARE(addFile->mapTo(card, QPoint()).y(), removeFile->mapTo(card, QPoint()).y());

    // Adding the attribute changed the entry, so save it rather than answer the prompt to discard it
    const auto entriesBefore = m_workGroup->entries().size();
    QTest::mouseClick(m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(m_workGroup->entries().size(), entriesBefore + 1);
    delete m_dbWidget->currentSelectedEntry();
}

void TestGuiVault::testAutoTypePage()
{
    selectGroup(m_workGroup);
    m_dbWidget->createEntry();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* card = entryEditCard();
    auto* categories = card->editor()->findChild<QListWidget*>("categoryList");
    categories->setCurrentItem(categories->findItems("Auto-Type", Qt::MatchExactly).first());

    // Auto-Type is a switch, and the sequence fields stay in the page
    auto* autoType = card->findChild<QWidget*>("EditEntryWidgetAutoType");
    QVERIFY(!autoType->findChild<QWidget*>("vaultSettingsPageForm")->isVisible());
    QVERIFY(card->findChild<QWidget*>("enableButton")->isVisible());
    QVERIFY(card->findChild<QWidget*>("sequenceEdit")->isVisible());
    QVERIFY(card->findChild<QWidget*>("openHelpButton")->isVisible());

    // Without associations, a note stands in for the list and the association's fields until one is added
    auto* none = card->findChild<QLabel*>("vaultNoWindowAssociationsLabel");
    auto* view = card->findChild<QAbstractItemView*>("assocView");
    auto* title = card->findChild<QComboBox*>("windowTitleCombo");
    auto* remove = card->findChild<QAbstractButton*>("assocRemoveButton");
    QVERIFY(none->isVisible());
    QVERIFY(!view->isVisible());
    QVERIFY(!title->isVisible());
    QVERIFY(!remove->isVisible());
    QTest::mouseClick(card->findChild<QAbstractButton*>("assocAddButton"), Qt::LeftButton);
    QVERIFY(!none->isVisible());
    QVERIFY(view->isVisible());
    QVERIFY(title->isVisible());
    QVERIFY(remove->isVisible());
    // The association's fields sit under the list rather than beside it
    QTRY_VERIFY(title->mapTo(card, QPoint()).y() > view->mapTo(card, QPoint()).y() + view->height());
    QTest::mouseClick(remove, Qt::LeftButton);
    QVERIFY(none->isVisible());
    QVERIFY(!view->isVisible());

    // The association came and went, which still counts as a change, so save rather than discard
    const auto entriesBefore = m_workGroup->entries().size();
    QTest::mouseClick(m_mainWindow->findChild<QPushButton*>("vaultNewEntrySave"), Qt::LeftButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(m_workGroup->entries().size(), entriesBefore + 1);
    delete m_dbWidget->currentSelectedEntry();
}

void TestGuiVault::testLeavingViewEndsEditing()
{
    selectGroup(m_workGroup);
    selectEntry(m_github);
    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);

    // Opening the database settings ends inline editing, like the pages that replace the main view
    m_dbWidget->switchToDatabaseSettings();
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::DatabaseSettingsMode);
    m_dbWidget->switchToMainView();
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QVERIFY(m_dbWidget->findChild<EntryDetailCard*>()->isVisible());
    QVERIFY(!entryEditCard()->isVisible());
    QVERIFY(m_dbWidget->findChild<EntryView*>("entryView")->isEnabled());
    QVERIFY(m_dbWidget->findChild<VaultNavPane*>()->isEnabled());
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
    QCOMPARE(menuItems.size(), 10);
    QCOMPARE(menuItems.mid(2),
             QStringList({"-", "actionDatabaseNew", "actionDatabaseOpen", "-", "actionDatabaseSettings", "-",
                          "actionLockAllDatabases", "actionDatabaseClose"}));
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
    auto* stackedWidget = m_mainWindow->centralWidget()->findChild<QStackedWidget*>("stackedWidget");
    stackedWidget->setCurrentIndex(MainWindow::WelcomeScreen);
    QApplication::processEvents();
    QVERIFY(!header->isVisible());
    QVERIFY(titleBar->isVisible());
    stackedWidget->setCurrentIndex(MainWindow::DatabaseTabScreen);
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

void TestGuiVault::testPasswordGeneratorModal()
{
    auto* modal = m_mainWindow->findChild<VaultModal*>("vaultPasswordGeneratorModal");
    QVERIFY(modal);
    QVERIFY(!modal->isOpen());
    auto* generatorAction = m_mainWindow->findChild<QAction*>("actionPasswordGenerator");
    auto* stackedWidget = m_mainWindow->centralWidget()->findChild<QStackedWidget*>("stackedWidget");
    const int screen = stackedWidget->currentIndex();

    generatorAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QVERIFY(generatorAction->isChecked());
    // Like the settings, the generator sits over the window with the database behind it
    QCOMPARE(modal->geometry(), m_mainWindow->rect());
    QCOMPARE(stackedWidget->currentIndex(), screen);
    QVERIFY(m_dbWidget->isVisible());
    QVERIFY(!m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());
    QVERIFY(m_mainWindow->windowTitle().startsWith("Password Generator"));

    auto* generator = m_mainWindow->findChild<PasswordGeneratorWidget*>("passwordGeneratorWidget");
    QVERIFY(modal->card()->isAncestorOf(generator));
    QVERIFY(generator->findChild<QWidget*>("buttonClose")->isHidden());
    QVERIFY(!generator->getGeneratedPassword().isEmpty());
    QVERIFY(modal->card()->height() < m_mainWindow->height());

    // Tab stays inside the modal
    for (int i = 0; i < 30; ++i) {
        QTest::keyClick(m_mainWindow->focusWidget(), Qt::Key_Tab);
        QVERIFY(modal->isAncestorOf(m_mainWindow->focusWidget()));
    }

    // The footer button, Escape and the close button all close it
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultPasswordGeneratorDone"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    QVERIFY(!generatorAction->isChecked());
    QVERIFY(m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());
    QVERIFY(!m_mainWindow->windowTitle().startsWith("Password Generator"));

    generatorAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QTest::keyClick(m_mainWindow->focusWidget(), Qt::Key_Escape);
    QTRY_VERIFY(!modal->isOpen());
    generatorAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultPasswordGeneratorClose"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());

    // Opening the settings replaces it instead of stacking a second modal
    generatorAction->trigger();
    QTRY_VERIFY(modal->isOpen());
    m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
    auto* settingsModal = m_mainWindow->findChild<VaultSettingsModal*>("vaultSettingsModal");
    QTRY_VERIFY(settingsModal->isOpen());
    QVERIFY(!modal->isOpen());
    QTest::mouseClick(settingsModal->findChild<QPushButton*>("vaultSettingsCancel"), Qt::LeftButton);
    QTRY_VERIFY(!settingsModal->isOpen());
}

void TestGuiVault::testDatabaseModals()
{
    auto* settingsAction = m_mainWindow->findChild<QAction*>("actionDatabaseSettings");
    auto* reportsAction = m_mainWindow->findChild<QAction*>("actionReports");
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // Database settings open in a modal over the window, with the database behind it
    settingsAction->trigger();
    auto* settingsModal = m_mainWindow->findChild<VaultSettingsModal*>("vaultDatabaseSettingsModal");
    QVERIFY(settingsModal);
    QTRY_VERIFY(settingsModal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::DatabaseSettingsMode);
    QVERIFY(settingsAction->isChecked());
    QCOMPARE(settingsModal->geometry(), m_mainWindow->rect());
    QVERIFY(entryView->isVisible());
    QVERIFY(!m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());
    auto* settingsDialog = m_mainWindow->findChild<QWidget*>("databaseSettingsDialog");
    QVERIFY(settingsModal->card()->isAncestorOf(settingsDialog));
    // The pages fill the card beside the sidebar
    QVERIFY(settingsDialog->isVisible());
    QVERIFY(settingsDialog->width() > settingsModal->card()->width() / 2);
    QVERIFY(settingsDialog->findChild<QWidget*>("categoryList")->isHidden());
    QVERIFY(settingsModal->findChild<QPushButton*>("vaultDatabaseSettingsSave"));

    QTest::mouseClick(settingsModal->findChild<QPushButton*>("vaultDatabaseSettingsCancel"), Qt::LeftButton);
    QTRY_VERIFY(!settingsModal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QVERIFY(!settingsAction->isChecked());
    QVERIFY(m_mainWindow->findChild<QAction*>("actionEntryNew")->isEnabled());

    // Reports can only be closed
    reportsAction->trigger();
    auto* reportsModal = m_mainWindow->findChild<VaultSettingsModal*>("vaultReportsModal");
    QVERIFY(reportsModal);
    QTRY_VERIFY(reportsModal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ReportsMode);
    QVERIFY(reportsAction->isChecked());
    auto* reportsDialog = m_mainWindow->findChild<QWidget*>("reportsDialog");
    QVERIFY(reportsModal->card()->isAncestorOf(reportsDialog));
    QVERIFY(reportsDialog->isVisible());
    QVERIFY(reportsDialog->width() > reportsModal->card()->width() / 2);
    QVERIFY(!reportsModal->findChild<QPushButton*>("vaultReportsSave"));
    QTest::mouseClick(reportsModal->findChild<QPushButton*>("vaultReportsDone"), Qt::LeftButton);
    QTRY_VERIFY(!reportsModal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QVERIFY(!reportsAction->isChecked());

    // Escape closes them too, and so does another page taking the main view's place, such as locking
    reportsAction->trigger();
    QTRY_VERIFY(reportsModal->isOpen());
    QTest::keyClick(m_mainWindow->focusWidget(), Qt::Key_Escape);
    QTRY_VERIFY(!reportsModal->isOpen());
    settingsAction->trigger();
    QTRY_VERIFY(settingsModal->isOpen());
    m_dbWidget->switchToGroupEdit();
    QVERIFY(!settingsModal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditGroupMode);
    m_dbWidget->switchToMainView();
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestGuiVault::testGroupModals()
{
    selectGroup(m_workGroup);
    const auto childrenBefore = m_workGroup->children().size();
    auto* entryView = m_dbWidget->findChild<EntryView*>("entryView");

    // A new group opens in a modal over the window, with the database behind it
    m_mainWindow->findChild<QAction*>("actionGroupNew")->trigger();
    auto* modal = m_mainWindow->findChild<VaultSettingsModal*>("vaultGroupModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditGroupMode);
    QVERIFY(m_dbWidget->isGroupEditActive());
    QCOMPARE(modal->geometry(), m_mainWindow->rect());
    QVERIFY(entryView->isVisible());
    QVERIFY(!m_mainWindow->findChild<QAction*>("actionGroupNew")->isEnabled());
    QCOMPARE(modal->findChild<QLabel*>("vaultGroupTitle")->text(), QString("New Group"));
    auto* editor = m_mainWindow->findChild<QWidget*>("editGroupWidget");
    QVERIFY(modal->card()->isAncestorOf(editor));
    QVERIFY(editor->isVisible());
    QVERIFY(editor->findChild<QWidget*>("categoryList")->isHidden());
    auto* name = editor->findChild<QLineEdit*>("editName");
    QTRY_COMPARE(m_mainWindow->focusWidget(), name);

    QTest::keyClicks(name, "Created in a modal");
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultGroupSave"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(m_workGroup->children().size(), childrenBefore + 1);
    auto* created = m_dbWidget->currentGroup();
    QCOMPARE(created->name(), QString("Created in a modal"));
    QCOMPARE(created->parentGroup(), m_workGroup.data());

    // Editing the group uses the same modal under its own title; Escape sets changes aside
    m_mainWindow->findChild<QAction*>("actionGroupEdit")->trigger();
    QTRY_VERIFY(modal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditGroupMode);
    QCOMPARE(modal->findChild<QLabel*>("vaultGroupTitle")->text(), QString("Edit Group"));
    QCOMPARE(name->text(), QString("Created in a modal"));
    QVERIFY(editor->findChild<QLabel*>("headerLabel")->isHidden());
    QTest::keyClick(modal, Qt::Key_Escape);
    QTRY_VERIFY(!modal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);

    // Renaming saves through the modal too
    m_mainWindow->findChild<QAction*>("actionGroupEdit")->trigger();
    QTRY_VERIFY(modal->isOpen());
    name->setText("Renamed in a modal");
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultGroupSave"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    QCOMPARE(created->name(), QString("Renamed in a modal"));

    // A new group set aside with Cancel leaves nothing behind
    selectGroup(m_workGroup);
    m_mainWindow->findChild<QAction*>("actionGroupNew")->trigger();
    QTRY_VERIFY(modal->isOpen());
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultGroupCancel"), Qt::LeftButton);
    QTRY_VERIFY(!modal->isOpen());
    QCOMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
    QCOMPARE(m_workGroup->children().size(), childrenBefore + 1);

    delete created;
    selectGroup(m_workGroup);
}

void TestGuiVault::testTotpSetupModal()
{
    auto* entry = new Entry();
    entry->setUuid(QUuid::createUuid());
    entry->setTitle("TOTP setup");
    entry->setGroup(m_workGroup);
    selectGroup(m_workGroup);
    selectEntry(entry);

    // Setting up TOTP opens a modal over the window, with Cancel and Save in its footer
    m_mainWindow->findChild<QAction*>("actionEntrySetupTotp")->trigger();
    auto* modal = m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
    QCOMPARE(modal->geometry(), m_mainWindow->rect());
    auto* dialog = modal->findChild<TotpSetupDialog*>();
    QVERIFY(dialog);
    QVERIFY(!dialog->isWindow());
    QVERIFY(dialog->findChild<QDialogButtonBox*>("buttonBox")->isHidden());
    auto* seedEdit = dialog->findChild<QLineEdit*>("seedEdit");
    QTRY_COMPARE(m_mainWindow->focusWidget(), seedEdit);

    // Custom settings only show while they apply
    auto* customSettings = dialog->findChild<QWidget*>("customSettingsGroup");
    auto* algorithm = dialog->findChild<QWidget*>("algorithmComboBox");
    QVERIFY(!algorithm->isVisible());
    dialog->findChild<QRadioButton*>("radioCustom")->setChecked(true);
    QTRY_VERIFY(algorithm->isVisible());
    QVERIFY(customSettings->isEnabled());
    dialog->findChild<QRadioButton*>("radioDefault")->setChecked(true);
    QTRY_VERIFY(!algorithm->isVisible());

    // An invalid key shows an error in the modal instead of a message box, and keeps it open
    auto* errorBar = dialog->findChild<QWidget*>("totpErrorBar");
    QVERIFY(!errorBar->isVisible());
    QTest::keyClicks(seedEdit, "not base32!");
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultTotpSetupSave"), Qt::LeftButton);
    QTRY_VERIFY(errorBar->isVisible());
    QVERIFY(modal->isOpen());
    QVERIFY(!entry->hasTotp());
    // Editing the key clears the error
    QTest::keyClick(seedEdit, Qt::Key_Backspace);
    QVERIFY(!errorBar->isVisible());

    // A valid key saves and closes the modal
    seedEdit->clear();
    QTest::keyClicks(seedEdit, "JBSWY3DPEHPK3PXP");
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultTotpSetupSave"), Qt::LeftButton);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal"));
    QVERIFY(entry->hasTotp());
    QCOMPARE(entry->totpSettings()->key, QString("JBSWY3DPEHPK3PXP"));

    // Escape sets changes aside
    m_mainWindow->findChild<QAction*>("actionEntrySetupTotp")->trigger();
    modal = m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal");
    QTRY_VERIFY(modal && modal->isOpen());
    seedEdit = modal->findChild<QLineEdit*>("seedEdit");
    QCOMPARE(seedEdit->text(), QString("JBSWY3DPEHPK3PXP"));
    seedEdit->setText("GEZDGNBVGY3TQOJQ");
    QTest::keyClick(modal, Qt::Key_Escape);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal"));
    QCOMPARE(entry->totpSettings()->key, QString("JBSWY3DPEHPK3PXP"));

    delete entry;
    selectGroup(m_workGroup);
}

void TestGuiVault::testNewDatabaseModal()
{
    const int tabsBefore = m_tabWidget->count();
    auto* newAction = m_mainWindow->findChild<QAction*>("actionDatabaseNew");

    // The modal waits like the wizard did, so drive it from inside its event loop
    QStringList titles;
    QList<int> steps;
    QString lastButton;
    bool backHiddenOnFirst = false;
    bool nameKept = false;
    bool coversWindow = false;
    QTimer::singleShot(0, this, [&] {
        auto* modal = m_mainWindow->findChild<VaultNewDatabaseModal*>("vaultNewDatabaseModal");
        if (!modal || !modal->isOpen()) {
            return;
        }
        coversWindow = modal->geometry() == m_mainWindow->rect();
        auto* title = modal->findChild<QLabel*>("vaultNewDatabaseStepTitle");
        auto* back = modal->findChild<QPushButton*>("vaultNewDatabaseBack");
        auto* next = modal->findChild<QPushButton*>("vaultNewDatabaseContinue");
        auto* name = modal->findChild<QLineEdit*>("databaseName");
        backHiddenOnFirst = back->isHidden();
        titles << title->text();
        steps << modal->currentStep();

        const auto dir = qEnvironmentVariable("KPXC_VAULT_SCREENSHOT_DIR");
        QTest::keyClicks(name, "Created in a modal");
        if (!dir.isEmpty()) {
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath("vault-new-database-general.png"));
        }
        QTest::mouseClick(next, Qt::LeftButton);
        titles << title->text();
        steps << modal->currentStep();
        if (!dir.isEmpty()) {
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath("vault-new-database-encryption.png"));
        }

        // Back keeps what was entered
        QTest::mouseClick(back, Qt::LeftButton);
        steps << modal->currentStep();
        nameKept = name->text() == "Created in a modal";

        QTest::keyClick(name, Qt::Key_Enter);
        QTest::mouseClick(next, Qt::LeftButton);
        titles << title->text();
        steps << modal->currentStep();
        lastButton = next->text();

        auto* password = modal->findChild<PasswordWidget*>("enterPasswordEdit")->findChild<QLineEdit*>("passwordEdit");
        auto* repeat = modal->findChild<PasswordWidget*>("repeatPasswordEdit")->findChild<QLineEdit*>("passwordEdit");
        QTest::keyClicks(password, "test");
        QTest::keyClicks(repeat, "test");
        if (!dir.isEmpty()) {
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath("vault-new-database-credentials.png"));
        }
        MessageBox::setNextAnswer(MessageBox::ContinueWithWeakPass);
        QTest::mouseClick(next, Qt::LeftButton);
    });
    newAction->trigger();

    QVERIFY(coversWindow);
    QVERIFY(backHiddenOnFirst);
    QCOMPARE(titles, QStringList({"General", "Encryption", "Database Credentials"}));
    QCOMPARE(steps, QList<int>({0, 1, 0, 2}));
    QVERIFY(nameKept);
    QCOMPARE(lastButton, QString("Create Database"));
    QTRY_VERIFY(!m_mainWindow->findChild<VaultNewDatabaseModal*>());
    QCOMPARE(m_tabWidget->count(), tabsBefore + 1);
    auto* created = m_tabWidget->currentDatabaseWidget();
    QCOMPARE(created->database()->metadata()->name(), QString("Created in a modal"));
    QVERIFY(created->database()->key());
    QVERIFY(created->database()->kdf());

    MessageBox::setNextAnswer(MessageBox::Discard);
    QVERIFY(m_tabWidget->closeDatabaseTab(created));
    QCOMPARE(m_tabWidget->count(), tabsBefore);

    // Escape sets it aside without a new database
    bool cancelled = false;
    QTimer::singleShot(0, this, [&] {
        auto* modal = m_mainWindow->findChild<VaultNewDatabaseModal*>("vaultNewDatabaseModal");
        if (modal && modal->isOpen()) {
            QTest::keyClick(m_mainWindow->focusWidget(), Qt::Key_Escape);
            cancelled = !modal->isOpen();
        }
    });
    newAction->trigger();
    QVERIFY(cancelled);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultNewDatabaseModal*>());
    QCOMPARE(m_tabWidget->count(), tabsBefore);
    m_tabWidget->setCurrentWidget(m_dbWidget);
}

void TestGuiVault::testMenuClickSurvivesLostHighlight()
{
    VaultMenu menu;
    menu.addAction("First");
    auto* second = menu.addAction("Second");
    int triggered = 0;
    connect(second, &QAction::triggered, this, [&] { ++triggered; });
    menu.popup(m_mainWindow->mapToGlobal(QPoint(40, 40)));
    QTRY_VERIFY(menu.isVisible());

    // On macOS, raising the menu on press sends it a Leave that drops the highlight before the release
    const auto pos = menu.actionGeometry(second).center();
    QTest::mouseMove(&menu, pos);
    QTest::mousePress(&menu, Qt::LeftButton, {}, pos);
    menu.setActiveAction(nullptr);
    QTest::mouseRelease(&menu, Qt::LeftButton, {}, pos);
    QCOMPARE(triggered, 1);
    QVERIFY(!menu.isVisible());
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

void TestGuiVault::testFeatureSettingsPages()
{
    m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
    auto* settingsWidget = m_mainWindow->findChild<ApplicationSettingsWidget*>();
    QTRY_VERIFY(settingsWidget->isVisible());
    auto* categories = settingsWidget->findChild<QListWidget*>("categoryList");
    auto* pages = settingsWidget->findChild<QStackedWidget*>("stackedWidget");

    // The Browser Integration, SSH Agent and KeeShare pages show settings cards in place of their forms
    const QList<QPair<QString, QString>> features = {
#ifdef KPXC_FEATURE_BROWSER
        {"Browser Integration", "enableBrowserSupport"},
#endif
#ifdef KPXC_FEATURE_SSHAGENT
        {"SSH Agent", "enableSSHAgentCheckBox"},
#endif
        {"KeeShare", "enableImportCheckBox"},
    };
    for (const auto& feature : features) {
        const auto items = categories->findItems(feature.first, Qt::MatchExactly);
        QCOMPARE(items.size(), 1);
        categories->setCurrentItem(items.first());
        auto* page = pages->currentWidget();
        auto* toggle = page->findChild<QAbstractButton*>(feature.second);
        QVERIFY(toggle);
        QVERIFY(toggle->isVisibleTo(settingsWidget));
        QVERIFY(page->findChild<QWidget*>(feature.second + "Switch")->isVisibleTo(settingsWidget));
        QVERIFY(!page->findChild<QWidget*>("vaultSettingsPageForm")->isVisibleTo(settingsWidget));
    }

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
    auto* tagsHeader = tagWidget->findChild<QAbstractButton*>("vaultTagsHeader");
    QVERIFY(tagsHeader);
    QCOMPARE(tagsHeader->text(), QString("Tags"));
    QVERIFY(tagsHeader->isChecked());

    // Clicking a tag still searches for it
    QTest::mouseClick(tagView->viewport(), Qt::LeftButton, {}, tagView->visualRect(tagView->model()->index(0, 0)).center());
    QTRY_VERIFY(m_dbWidget->isSearchActive());
    QVERIFY(m_dbWidget->getCurrentSearch().startsWith("tag:"));
    m_dbWidget->endSearch();
    QVERIFY(!tagView->selectionModel()->hasSelection());
    QVERIFY(!tagView->currentIndex().isValid());

    // The groups get the height the few tags don't need
    auto* groupView = m_dbWidget->findChild<QWidget*>("groupView");
    const int splitterWidth =
        groupView->parentWidget()->style()->pixelMetric(QStyle::PM_SplitterWidth, nullptr, groupView->parentWidget());
    QCOMPARE(groupView->height() + tagWidget->height() + splitterWidth, groupView->parentWidget()->height());

    // The title folds the tags away, and the groups take their room
    const int expandedGroupsHeight = groupView->height();
    QTest::mouseClick(tagsHeader, Qt::LeftButton);
    QVERIFY(!tagsHeader->isChecked());
    QVERIFY(config()->get(Config::GUI_VaultTagsCollapsed).toBool());
    QTRY_VERIFY(!tagView->isVisible());
    QVERIFY(tagsHeader->isVisible());
    QTRY_COMPARE(tagWidget->height(), tagsHeader->height());
    QVERIFY(groupView->height() > expandedGroupsHeight);
    QCOMPARE(groupView->height() + tagWidget->height() + splitterWidth, groupView->parentWidget()->height());

    // It stays folded when the tags change, and the setting reaches the header too
    const auto tags = m_github->tags();
    m_github->setTags(tags + ";folded");
    QVERIFY(!tagView->isVisible());
    m_github->setTags(tags);
    config()->set(Config::GUI_VaultTagsCollapsed, false);
    QVERIFY(tagsHeader->isChecked());
    QTRY_VERIFY(tagView->isVisible());
    QTRY_COMPARE(groupView->height(), expandedGroupsHeight);

    QTest::mouseClick(tagsHeader, Qt::LeftButton);
    QTRY_VERIFY(!tagView->isVisible());
    QTest::mouseClick(tagsHeader, Qt::LeftButton);
    QVERIFY(tagsHeader->isChecked());
    QVERIFY(!config()->get(Config::GUI_VaultTagsCollapsed).toBool());
    QTRY_VERIFY(tagView->isVisible());
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
        m_mainWindow->findChild<QAction*>("actionEntrySetupTotp")->trigger();
        QApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("vault-totp-%1.png").arg(theme))));
        auto* totpModal = m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal");
        totpModal->findChild<QRadioButton*>("radioCustom")->setChecked(true);
        QApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("vault-totp-custom-%1.png").arg(theme))));
        QTest::keyClick(totpModal, Qt::Key_Escape);
        QTRY_VERIFY(!m_mainWindow->findChild<VaultModal*>("vaultTotpSetupModal"));
        m_mainWindow->findChild<QAction*>("actionSettings")->trigger();
        QApplication::processEvents();
        QTest::qWait(100);
        QVERIFY(
            m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("vault-settings-%1.png").arg(theme))));
        m_mainWindow->findChild<ApplicationSettingsWidget*>()->findChild<QListWidget*>("categoryList")->setCurrentRow(2);
        QApplication::processEvents();
        QVERIFY(m_mainWindow->grab().save(
            QDir(dir).absoluteFilePath(QString("vault-settings-autotype-%1.png").arg(theme))));
        auto* categories = m_mainWindow->findChild<ApplicationSettingsWidget*>()->findChild<QListWidget*>("categoryList");
        for (const auto& name : {QStringLiteral("Browser Integration"), QStringLiteral("SSH Agent"), QStringLiteral("KeeShare")}) {
            const auto items = categories->findItems(name, Qt::MatchExactly);
            if (items.isEmpty()) {
                continue;
            }
            categories->setCurrentItem(items.first());
            QApplication::processEvents();
            const auto file = QString("vault-settings-%1-%2.png").arg(name.toLower().remove(' '), theme);
            QVERIFY(m_mainWindow->grab().save(QDir(dir).absoluteFilePath(file)));
        }
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

// Screenshots of the views around the main window: editors, generator, reports, unlock and welcome
void TestGuiVault::testViewScreenshots()
{
    const auto dir = qEnvironmentVariable("KPXC_VAULT_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set KPXC_VAULT_SCREENSHOT_DIR to save screenshots");
    }

    auto save = [this, dir](const QString& name, const QString& theme) {
        QApplication::processEvents();
        QTest::qWait(150);
        QVERIFY(m_mainWindow->grab().save(QDir(dir).absoluteFilePath(QString("view-%1-%2.png").arg(name, theme))));
    };
    auto* stack = m_mainWindow->findChild<QStackedWidget*>("stackedWidget");
    // Locking reloads the database from disk, so keep the example content there
    QString error;
    QVERIFY2(m_db->save(Database::Atomic, {}, &error), qPrintable(error));
    auto reload = [this] {
        m_db = m_dbWidget->database();
        m_workGroup = m_db->rootGroup()->findGroupByPath("/Work");
        QVERIFY(m_workGroup);
        m_github = m_db->rootGroup()->findEntryByPath("/Work/GitHub");
        QVERIFY(m_github);
    };

    for (const auto& theme : {QStringLiteral("light"), QStringLiteral("dark")}) {
        config()->set(Config::GUI_ApplicationTheme, theme);
        kpxcApp->applyTheme();
        selectGroup(m_workGroup);
        selectEntry(m_github);

        m_dbWidget->switchToEntryEdit();
        save("entry", theme);
        auto* entryPage = qobject_cast<QScrollArea*>(entryEditCard()->editor()->pageStack()->widget(0));
        entryPage->verticalScrollBar()->setValue(entryPage->verticalScrollBar()->maximum());
        save("entry-more", theme);
        entryPage->verticalScrollBar()->setValue(0);
        auto* entryCategories = entryEditCard()->editor()->findChild<QListWidget*>("categoryList");
        entryCategories->setCurrentRow(1);
        save("entry-advanced", theme);
        auto* advancedPage = qobject_cast<QScrollArea*>(entryEditCard()->editor()->pageStack()->widget(1));
        advancedPage->verticalScrollBar()->setValue(advancedPage->verticalScrollBar()->maximum());
        save("entry-advanced-more", theme);
        advancedPage->ensureWidgetVisible(advancedPage->findChild<QWidget*>("attachmentsWidget"));
        save("entry-advanced-attachments", theme);
        entryCategories->setCurrentRow(3);
        save("entry-autotype", theme);
        QTest::mouseClick(entryEditCard()->findChild<QAbstractButton*>("assocAddButton"), Qt::LeftButton);
        entryEditCard()->findChild<QComboBox*>("windowTitleCombo")->setEditText("*GitHub*");
        auto* autoTypePage = qobject_cast<QScrollArea*>(entryEditCard()->editor()->pageStack()->widget(3));
        // Let the page grow by the association's fields before scrolling to them
        QApplication::processEvents();
        autoTypePage->verticalScrollBar()->setValue(autoTypePage->verticalScrollBar()->maximum());
        save("entry-autotype-window", theme);
        entryCategories->setCurrentRow(0);
        m_dbWidget->switchToMainView();

        m_dbWidget->createEntry();
        QTest::keyClicks(entryEditCard()->findChild<QLineEdit*>("urlEdit"), "https://www.example.org");
        save("entry-new", theme);
        m_dbWidget->switchToMainView();

        m_dbWidget->switchToGroupEdit();
        save("group", theme);
        m_dbWidget->switchToMainView();

        m_dbWidget->switchToDatabaseSettings();
        save("dbsettings", theme);
        m_dbWidget->switchToMainView();

        m_dbWidget->switchToDatabaseReports();
        save("reports", theme);
        m_dbWidget->switchToMainView();

        m_mainWindow->findChild<QAction*>("actionPasswordGenerator")->trigger();
        save("generator", theme);
        auto* generator = m_mainWindow->findChild<PasswordGeneratorWidget*>("passwordGeneratorWidget");
        auto* advancedToggle = generator->findChild<QAbstractButton*>("advancedModeToggle");
        advancedToggle->setChecked(true);
        save("generator-advanced", theme);
        advancedToggle->setChecked(false);
        generator->findChild<QTabWidget*>("tabWidget")->setCurrentIndex(PasswordGeneratorWidget::Diceware);
        save("generator-passphrase", theme);
        generator->findChild<QTabWidget*>("tabWidget")->setCurrentIndex(PasswordGeneratorWidget::Password);
        m_mainWindow->findChild<QAction*>("actionPasswordGenerator")->trigger();

        QVERIFY(m_dbWidget->lock());
        save("unlock", theme);
        auto* editPassword =
            m_dbWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
        editPassword->setFocus();
        QTest::keyClicks(editPassword, "a");
        QTest::keyClick(editPassword, Qt::Key_Enter);
        QTRY_VERIFY(!m_dbWidget->isLocked());
        reload();

        const auto index = stack->currentIndex();
        stack->setCurrentIndex(MainWindow::WelcomeScreen);
        save("welcome", theme);
        stack->setCurrentIndex(index);
    }
}
