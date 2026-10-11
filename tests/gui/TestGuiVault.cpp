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

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
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
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), {});
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
    }
}
