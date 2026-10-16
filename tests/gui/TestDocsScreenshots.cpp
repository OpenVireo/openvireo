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

#include "TestDocsScreenshots.h"

#include "config-keepassx-tests.h"
#include "config-keepassx.h"
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
#include "gui/MessageBox.h"
#include "gui/PasswordGeneratorWidget.h"
#include "gui/PasswordWidget.h"
#include "gui/entry/EditEntryWidget.h"
#include "gui/entry/EntryView.h"
#include "gui/group/GroupView.h"
#include "gui/vault/EntryDetailCard.h"
#include "gui/vault/VaultEntryEditCard.h"
#include "gui/vault/VaultImportModal.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultNewDatabaseModal.h"
#include "gui/vault/VaultPanes.h"
#include "gui/vault/VaultSettingsModal.h"
#include "gui/wizard/ImportWizard.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QListView>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTest>
#include <QTimer>

int main(int argc, char* argv[])
{
    Application app(argc, argv);
    app.setApplicationName("KeePassXC");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    QTEST_DISABLE_KEYPAD_NAVIGATION
    TestDocsScreenshots tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    // Every screenshot shares one scale, so text is the same size in every image: a full
    // window comes out kOutputWidth pixels wide, crops proportionally smaller.
    constexpr int kOutputWidth = 1600;
    const QSize kWindowSize(1180, 760);
    const QColor kCalloutColor(0x3c, 0x7d, 0x20); // KeePassXC brand green

    // Lets a modal's nested event loop run a block, as the new database wizard needs
    template <typename F> void whileOpen(QObject* context, F&& body)
    {
        QTimer::singleShot(0, context, std::forward<F>(body));
    }
} // namespace

// Setup -----------------------------------------------------------------------------------------

void TestDocsScreenshots::initTestCase()
{
    const auto dir = qEnvironmentVariable("KPXC_DOCS_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        QSKIP("Set KPXC_DOCS_SCREENSHOT_DIR (or build the docs_screenshots target) to regenerate docs images");
    }
    m_outputDir = QDir(dir);
    QVERIFY(m_outputDir.mkpath("."));
    QVERIFY(m_tempDir.isValid());

    QVERIFY(Crypto::init());
    Config::createConfigFromFile(m_tempDir.filePath("config.ini"), m_tempDir.filePath("local.ini"));
    QLocale::setDefault(QLocale::c());
    Application::bootstrap();

    config()->set(Config::GUI_VaultLayout, true);
    config()->set(Config::GUI_ApplicationTheme, "light");
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    config()->set(Config::UpdateCheckMessageShown, true);
    config()->set(Config::Security_QuickUnlock, false);
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock, false);
    // Development builds show a pre-release banner that has no place in the docs
    config()->set(Config::Messages_HidePreReleaseWarning, KEEPASSXC_VERSION);
    kpxcApp->applyTheme();

    m_mainWindow.reset(new MainWindow());
    m_mainWindow->setAllowScreenCapture(true);
    // Editors only offer their Browser Integration page while it is enabled. Turned on after the
    // browser service has started (as TestGuiBrowser does), so it never touches real browsers.
    config()->set(Config::Browser_Enabled, true);
    // Debug builds have no keepassxc-proxy beside the test; any existing file stops the
    // browser page showing a "proxy missing" error
    config()->set(Config::Browser_UseCustomProxy, true);
    config()->set(Config::Browser_CustomProxyLocation, QCoreApplication::applicationFilePath());
    m_tabWidget = m_mainWindow->findChild<DatabaseTabWidget*>("tabWidget");
    m_mainWindow->resize(kWindowSize);
    m_mainWindow->show();
    m_mainWindow->activateWindow();
    QApplication::processEvents();

    // The header and unlock screen show the path, so keep it short rather than in a temp folder
    m_dbPath = QStringLiteral("/tmp/Passwords.kdbx");
    QVERIFY2(!QFile::exists(m_dbPath), qPrintable(m_dbPath + " already exists; move it out of the way first"));
    QVERIFY(QFile::copy(QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath("NewDatabase.kdbx"), m_dbPath));
    QFile::setPermissions(m_dbPath, QFile::ReadOwner | QFile::WriteOwner);
    fileDialog()->setNextFileName(m_dbPath);
    m_mainWindow->findChild<QAction*>("actionDatabaseOpen")->trigger();
    QApplication::processEvents();

    m_dbWidget = m_tabWidget->currentDatabaseWidget();
    auto* editPassword = m_dbWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
    editPassword->setFocus();
    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);
    QTRY_VERIFY(!m_dbWidget->isLocked());
    m_db = m_dbWidget->database();

    populate();
    QString error;
    QVERIFY2(m_db->save(Database::Atomic, {}, &error), qPrintable(error));
}

// Return to the plain main view between screenshots, whatever the last one left open
void TestDocsScreenshots::init()
{
    if (!m_dbWidget) {
        return;
    }
    for (auto* modal : m_mainWindow->findChildren<VaultModal*>()) {
        closeModal(modal);
    }
    m_dbWidget->switchToMainView();
    config()->set(Config::GUI_ApplicationTheme, "light");
    kpxcApp->applyTheme();
    m_mainWindow->resize(kWindowSize);
    selectGroup(m_workGroup);
    selectEntry(m_github);
}

void TestDocsScreenshots::cleanupTestCase()
{
    if (m_db) {
        m_db->markAsClean();
        MessageBox::setNextAnswer(MessageBox::No);
        m_mainWindow->findChild<QAction*>("actionDatabaseClose")->trigger();
        QApplication::processEvents();
        MessageBox::setNextAnswer(MessageBox::NoButton);
        QFile::remove(m_dbPath);
    }
    // Losslessly shrink what was written, when optipng is installed
    const auto optipng = QStandardPaths::findExecutable("optipng");
    if (!optipng.isEmpty() && !m_saved.isEmpty()) {
        QStringList files;
        for (const auto& name : m_saved) {
            files << m_outputDir.absoluteFilePath(name);
        }
        QProcess::execute(optipng, QStringList({"-quiet", "-o2", "-strip", "all"}) + files);
    }
    if (!m_saved.isEmpty()) {
        qInfo().noquote() << QString("Saved %1 screenshots to %2:\n  %3")
                                 .arg(m_saved.size())
                                 .arg(m_outputDir.absolutePath(), m_saved.join("\n  "));
    }
}

// Example content: a few groups of believable entries, one of them (GitHub) fully featured
void TestDocsScreenshots::populate()
{
    auto* root = m_db->rootGroup();
    for (auto* child : root->children()) {
        delete child;
    }
    for (auto* entry : root->entries()) {
        delete entry;
    }
    root->setName("Passwords");
    m_db->metadata()->setName("Passwords");

    auto addGroup = [root](const QString& name) {
        auto* group = new Group();
        group->setUuid(QUuid::createUuid());
        group->setName(name);
        group->setParent(root);
        return group;
    };
    auto addEntry = [](Group* group, const QString& title, const QString& username, const QString& url,
                       const QString& password) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setUsername(username);
        entry->setUrl(url);
        entry->setPassword(password);
        entry->setGroup(group);
        return entry;
    };

    auto* personal = addGroup("Personal");
    m_workGroup = addGroup("Work");
    auto* finance = addGroup("Finance");
    auto* social = addGroup("Social");

    m_github = addEntry(m_workGroup, "GitHub", "jdoe", "https://github.com/login", "c7#Lq9!vRz2@pWm8xKe4");
    m_github->setTotp(Totp::createSettings("JBSWY3DPEHPK3PXP"));
    m_github->setTags("work;2fa");
    m_github->setNotes("Recovery codes are attached. A passkey is also registered on the security key.");
    m_github->attributes()->set("Security question", "Name of your first pet?", false);
    m_github->attributes()->set("Security answer", "Whiskers", true);
    m_github->attachments()->set("recovery-codes.txt", QByteArray("a1b2-c3d4\ne5f6-g7h8\ni9j0-k1l2\n"));
    // A little history, so the History page has something to show
    for (const auto& old : {QStringLiteral("Wq3!kz8#Tn2v"), QStringLiteral("hunter2-github")}) {
        auto* item = m_github->clone(Entry::CloneNoFlags);
        item->setPassword(old);
        m_github->addHistoryItem(item);
    }
    addEntry(m_workGroup, "Atlassian Cloud", "jdoe@example.com", "https://id.atlassian.com", "Vt8$kq2#Nw5!zRp7Lc");
    auto* vpn = addEntry(m_workGroup, "Office VPN", "jdoe", "https://vpn.example.com", "Mx4!qZ8#tR2wLp6v");
    vpn->setExpires(true);
    vpn->setExpiryTime(QDateTime::currentDateTimeUtc().addDays(-3));
    addEntry(m_workGroup, "Postgres (staging)", "app_rw", "db-staging.example.com:5432", "staging1");
    addEntry(m_workGroup, "Slack", "jdoe@example.com", "https://example.slack.com", "Hq7#vN3!pX9zKt2w");
    addEntry(m_workGroup, "Docker Hub", "jdoe", "https://hub.docker.com", "Rz5!mK8#wQ2tVp4x");

    addEntry(personal, "Email", "john.doe@example.com", "https://mail.example.com", "Lp8#vQ2!nX7zRt4w");
    addEntry(personal, "Netflix", "john.doe@example.com", "https://www.netflix.com", "Kc3!wR9#mZ5tVq8x");
    addEntry(personal, "Dropbox", "john.doe@example.com", "https://www.dropbox.com", "password123");
    addEntry(finance, "Example Bank", "johndoe", "https://bank.example.com", "Tz6#qW1!vN8xLp3k");
    addEntry(social, "Mastodon", "@jdoe@fosstodon.org", "https://fosstodon.org", "Gv2!pK7#tQ9wZx4m");
    addEntry(social, "Reddit", "jdoe", "https://www.reddit.com", "Nm5#xR8!kW2qTz6v");
}

// Helpers ---------------------------------------------------------------------------------------

void TestDocsScreenshots::selectGroup(Group* group)
{
    m_dbWidget->findChild<GroupView*>("groupView")->setCurrentGroup(group);
    QApplication::processEvents();
}

void TestDocsScreenshots::selectEntry(Entry* entry)
{
    m_dbWidget->findChild<EntryView*>("entryView")->setCurrentEntry(entry);
    QApplication::processEvents();
}

// Settings, editors and dialogs all keep their pages in a list named categoryList
bool TestDocsScreenshots::selectPage(QWidget* root, const QString& name)
{
    for (auto* list : root->findChildren<QListWidget*>("categoryList")) {
        const auto items = list->findItems(name, Qt::MatchExactly);
        if (!items.isEmpty()) {
            list->setCurrentItem(items.first());
            QApplication::processEvents();
            return true;
        }
    }
    qWarning().noquote() << "No page named" << name << "in" << root->objectName();
    return false;
}

// Scroll whatever scroll area holds the widget so it is in view
void TestDocsScreenshots::reveal(QWidget* widget)
{
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* scrollArea = qobject_cast<QScrollArea*>(parent)) {
            scrollArea->ensureWidgetVisible(widget, 40, 160);
            QApplication::processEvents();
            return;
        }
    }
}

VaultModal* TestDocsScreenshots::openModal(const char* action, const QString& name)
{
    m_mainWindow->findChild<QAction*>(action)->trigger();
    auto* modal = m_mainWindow->findChild<VaultModal*>(name);
    if (modal && !QTest::qWaitFor([modal] { return modal->isOpen(); }, 2000)) {
        qWarning().noquote() << name << "did not open";
        return nullptr;
    }
    return modal;
}

// Escape cancels a modal the way a user would, so the window restores its actions and mode
void TestDocsScreenshots::closeModal(VaultModal* modal)
{
    if (modal && modal->isOpen()) {
        QTest::keyClick(modal, Qt::Key_Escape);
        if (!QTest::qWaitFor([modal] { return !modal->isOpen(); }, 2000)) {
            qWarning().noquote() << modal->objectName() << "did not close";
        }
    }
}

QRect TestDocsScreenshots::rectOf(QWidget* widget) const
{
    if (!widget || !widget->isVisible()) {
        return {};
    }
    return {widget->mapTo(m_mainWindow.data(), QPoint(0, 0)), widget->size()};
}

// Settings checkboxes are drawn as Fluent switches named <checkbox>Switch; point at what shows
QRect TestDocsScreenshots::controlRect(QWidget* root, const QString& name) const
{
    if (auto* fluentSwitch = root->findChild<QWidget*>(name + "Switch"); fluentSwitch && fluentSwitch->isVisible()) {
        return rectOf(fluentSwitch);
    }
    return rectOf(root->findChild<QWidget*>(name));
}

// A page in a settings modal's side navigation
QRect TestDocsScreenshots::navRect(VaultModal* modal, const QString& page) const
{
    auto* settingsModal = qobject_cast<VaultSettingsModal*>(modal);
    auto* navigation = settingsModal ? settingsModal->navigation() : nullptr;
    if (!navigation) {
        return {};
    }
    const auto matches = navigation->model()->match(navigation->model()->index(0, 0), Qt::DisplayRole, page, 1,
                                                    Qt::MatchExactly);
    if (matches.isEmpty()) {
        return {};
    }
    const auto rect = navigation->visualRect(matches.first());
    return {navigation->viewport()->mapTo(m_mainWindow.data(), rect.topLeft()), rect.size()};
}

// The dialog card plus a margin of the dimmed window around it
QRect TestDocsScreenshots::cardRect(VaultModal* modal) const
{
    return rectOf(modal->card()).adjusted(-24, -24, 24, 24).intersected(m_mainWindow->rect());
}

QImage TestDocsScreenshots::capture(QRect area)
{
    QApplication::processEvents();
    QTest::qWait(150); // let animations and deferred layouts settle
    if (area.isNull()) {
        area = m_mainWindow->rect();
    }
    const auto factor = double(kOutputWidth) / m_mainWindow->width();
    // Development builds call themselves "2.8.0-snapshot"; the docs describe the release
    QList<QPair<QLabel*, QString>> relabelled;
    for (auto* label : m_mainWindow->findChildren<QLabel*>()) {
        if (label->text().contains("-snapshot")) {
            relabelled.append({label, label->text()});
            label->setText(QString(label->text()).remove("-snapshot"));
        }
    }
    auto image = m_mainWindow->grab(area).toImage();
    for (const auto& [label, text] : relabelled) {
        label->setText(text);
    }
    image.setDevicePixelRatio(1);
    return image.scaled(qRound(area.width() * factor), qRound(area.height() * factor), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
}

void TestDocsScreenshots::save(const QString& name, const QList<Callout>& callouts, QRect area)
{
    if (area.isNull()) {
        area = m_mainWindow->rect();
    }
    auto image = capture(area);
    const auto factor = double(kOutputWidth) / m_mainWindow->width();

    // Callouts the docs text refers to, e.g. "(A) Groups": an outline and a numbered badge
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const auto& callout : callouts) {
        if (callout.rect.isNull()) {
            qWarning().noquote() << "Callout" << callout.label << "in" << name << "has no widget";
            continue;
        }
        const QRectF rect(QPointF(callout.rect.topLeft() - area.topLeft()) * factor,
                          QSizeF(callout.rect.size()) * factor);
        if (callout.box) {
            painter.setPen(QPen(kCalloutColor, 3 * factor));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 8 * factor, 8 * factor);
        }
        const qreal radius = 13 * factor;
        auto centre = callout.box ? rect.topRight() + QPointF(-radius - 6 * factor, radius + 6 * factor)
                                  : QPointF(rect.right() + radius + 6 * factor, rect.center().y());
        if (!callout.box && centre.x() + radius > image.width() - 24 * factor) {
            centre.setX(rect.left() - radius - 6 * factor);
        }
        painter.setPen(QPen(Qt::white, 2 * factor));
        painter.setBrush(kCalloutColor);
        painter.drawEllipse(centre, radius, radius);
        auto font = painter.font();
        font.setPixelSize(qRound(15 * factor));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(centre - QPointF(radius, radius), QSizeF(radius * 2, radius * 2)), Qt::AlignCenter,
                         callout.label);
    }
    painter.end();

    const auto file = m_outputDir.absoluteFilePath(name + ".png");
    QVERIFY2(image.convertToFormat(QImage::Format_RGB888).save(file), qPrintable(file));
    m_saved << name + ".png";
}

void TestDocsScreenshots::saveCard(const QString& name, VaultModal* modal, const QList<Callout>& callouts)
{
    QVERIFY(modal);
    save(name, callouts, cardRect(modal));
}

// Screenshots -----------------------------------------------------------------------------------

// The main window, its regions labelled as in the "Application Layout" section
void TestDocsScreenshots::mainWindow()
{
    auto* nav = m_dbWidget->findChild<VaultNavPane*>();
    auto* search = m_mainWindow->findChild<QWidget*>("SearchWidget");
    auto* entries = m_dbWidget->findChild<EntryView*>("entryView");
    auto* detail = m_dbWidget->findChild<EntryDetailCard*>();

    save("database_view");
    save("main_interface",
         {{"A", rectOf(nav), true},
          {"B", rectOf(search), true},
          {"C", rectOf(entries), true},
          {"D", rectOf(detail), true}});
    // The command bar takes over from the classic toolbar
    if (auto* commandBar = m_dbWidget->findChild<VaultCommandBar*>()) {
        save("toolbar", {}, rectOf(commandBar));
    }
}

void TestDocsScreenshots::welcomeAndUnlock()
{
    auto* stack = m_mainWindow->findChild<QStackedWidget*>("stackedWidget");
    const auto index = stack->currentIndex();
    stack->setCurrentIndex(MainWindow::WelcomeScreen);
    save("welcome_screen");
    stack->setCurrentIndex(index);

    QVERIFY(m_dbWidget->lock());
    auto* passwordWidget = m_dbWidget->findChild<PasswordWidget*>("editPassword");
    // The key file field stays folded away behind this link until it is needed
    auto* keyFile = m_dbWidget->findChild<QWidget*>("addKeyFileLinkLabel");
    save("unlock_database", {{"A", rectOf(passwordWidget)}, {"B", rectOf(keyFile)}});

    auto* editPassword = passwordWidget->findChild<QLineEdit*>("passwordEdit");
    editPassword->setFocus();
    QTest::keyClicks(editPassword, "a");
    QTest::keyClick(editPassword, Qt::Key_Enter);
    QTRY_VERIFY(!m_dbWidget->isLocked());
    // Unlocking reloads the database from disk
    m_db = m_dbWidget->database();
    m_workGroup = m_db->rootGroup()->findGroupByPath("/Work");
    m_github = m_db->rootGroup()->findEntryByPath("/Work/GitHub");
    QVERIFY(m_workGroup && m_github);
}

void TestDocsScreenshots::newDatabase()
{
    bool done = false;
    whileOpen(this, [&] {
        auto* modal = m_mainWindow->findChild<VaultNewDatabaseModal*>("vaultNewDatabaseModal");
        if (!modal || !modal->isOpen()) {
            return;
        }
        auto* next = modal->findChild<QPushButton*>("vaultNewDatabaseContinue");
        QTest::keyClicks(modal->findChild<QLineEdit*>("databaseName"), "Passwords");
        saveCard("new_db_wizard_1", modal);
        QTest::mouseClick(next, Qt::LeftButton);
        saveCard("new_db_wizard_2", modal);
        QTest::mouseClick(next, Qt::LeftButton);
        auto* password = modal->findChild<PasswordWidget*>("enterPasswordEdit");
        QTest::keyClicks(password->findChild<QLineEdit*>("passwordEdit"), "correct horse battery staple");
        QTest::keyClicks(modal->findChild<PasswordWidget*>("repeatPasswordEdit")->findChild<QLineEdit*>("passwordEdit"),
                         "correct horse battery staple");
        saveCard("new_db_wizard_3", modal);
        QTest::mouseClick(modal->findChild<QPushButton*>("vaultNewDatabaseCancel"), Qt::LeftButton);
        done = true;
    });
    m_mainWindow->findChild<QAction*>("actionDatabaseNew")->trigger();
    QTRY_VERIFY(done);
    QTRY_VERIFY(!m_mainWindow->findChild<VaultNewDatabaseModal*>());
}

void TestDocsScreenshots::entryEditor()
{
    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* card = m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard");
    QVERIFY(card);
    auto* editor = card->editor();

    save("edit_entry");

    QVERIFY(selectPage(editor, "Advanced"));
    auto* attributes = editor->findChild<QWidget*>("attributesView");
    auto* attachments = editor->findChild<QWidget*>("attachmentsWidget");
    if (attributes) {
        reveal(attributes);
    }
    save("edit_entry_attributes", {{"A", rectOf(attributes), true}});
    if (attachments) {
        reveal(attachments);
    }
    save("edit_entry_attachments", {{"A", rectOf(attachments), true}});
    if (auto* colors = editor->findChild<QWidget*>("fgColorCheckBox")) {
        reveal(colors);
        save("edit_entry_colors");
    }

    if (selectPage(editor, "Icon")) {
        save("edit_entry_icons");
    }
    if (selectPage(editor, "Auto-Type")) {
        save("autotype_entrylevel");
        if (auto* add = editor->findChild<QAbstractButton*>("assocAddButton")) {
            QTest::mouseClick(add, Qt::LeftButton);
            if (auto* window = editor->findChild<QComboBox*>("windowTitleCombo")) {
                window->setEditText("*GitHub*");
                reveal(window);
            }
            save("autotype_entry_sequences");
        }
    }
    if (selectPage(editor, "Browser Integration")) {
        save("browser_entry_settings");
    }
    if (selectPage(editor, "SSH Agent")) {
        save("sshagent_entry_settings");
    }
    if (selectPage(editor, "Properties")) {
        save("edit_entry_properties");
    }
    if (selectPage(editor, "History")) {
        save("edit_entry_history");
    }

    // Leave without keeping the association added above
    MessageBox::setNextAnswer(MessageBox::Discard);
    QTest::mouseClick(editor->buttonBox()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    MessageBox::setNextAnswer(MessageBox::NoButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestDocsScreenshots::groupEditor()
{
    m_mainWindow->findChild<QAction*>("actionGroupEdit")->trigger();
    auto* modal = m_mainWindow->findChild<VaultModal*>("vaultGroupModal");
    QVERIFY(modal);
    QTRY_VERIFY(modal->isOpen());
    auto* editor = m_mainWindow->findChild<QWidget*>("editGroupWidget");
    if (selectPage(editor, "Browser Integration")) {
        saveCard("browser_group_settings", modal);
    }
    if (selectPage(editor, "KeeShare")) {
        saveCard("keeshare_group_settings", modal);
    }
    closeModal(modal);
}

void TestDocsScreenshots::applicationSettings()
{
    auto* modal = openModal("actionSettings", "vaultSettingsModal");
    QVERIFY(modal);
    auto* settings = m_mainWindow->findChild<ApplicationSettingsWidget*>();

    selectPage(settings, "General");
    if (auto* theme = settings->findChild<QWidget*>("appThemeComboBox")) {
        reveal(theme);
        saveCard("theme_selection", modal, {{"A", rectOf(theme)}});
    }
    if (auto* saveGroup = settings->findChild<QWidget*>("saveGroup")) {
        reveal(saveGroup);
        saveCard("save_options",
                 modal,
                 {{"1", controlRect(settings, "autoSaveAfterEveryChangeCheckBox")},
                  {"2", controlRect(settings, "useAlternativeSaveCheckBox")}});
    }
    if (auto* backup = settings->findChild<QWidget*>("backupBeforeSaveCheckBox")) {
        reveal(backup);
        saveCard("save_database_backup",
                 modal,
                 {{"1", controlRect(settings, "backupBeforeSaveCheckBox")}, {"2", controlRect(settings, "backupFilePath")}});
    }

    if (selectPage(settings, "Auto-Type")) {
        saveCard("autotype_settings", modal);
    }
    if (selectPage(settings, "Browser Integration")) {
        auto* page = settings->findChild<QStackedWidget*>("stackedWidget")->currentWidget();
        saveCard("browser_settings",
                 modal,
                 {{"1", navRect(modal, "Browser Integration")},
                  {"2", controlRect(page, "enableBrowserSupport")},
                  {"3", controlRect(page, "firefoxSupport")}});
        if (auto* tabs = page->findChild<QTabWidget*>("tabWidget"); tabs && tabs->count() > 1) {
            tabs->setCurrentIndex(1);
            saveCard("browser_advanced_settings", modal);
            tabs->setCurrentIndex(0);
        }
    }
    if (selectPage(settings, "SSH Agent")) {
        saveCard("sshagent_application_settings", modal);
    }
    if (selectPage(settings, "KeeShare")) {
        saveCard("keeshare_application_settings", modal);
    }
    selectPage(settings, "General");
    closeModal(modal);
}

void TestDocsScreenshots::databaseSettings()
{
    auto* modal = openModal("actionDatabaseSettings", "vaultDatabaseSettingsModal");
    QVERIFY(modal);
    auto* dialog = m_mainWindow->findChild<QWidget*>("databaseSettingsDialog");

    selectPage(dialog, "General");
    saveCard("database_settings", modal);
    if (selectPage(dialog, "Security")) {
        auto* tabs = dialog->findChild<QTabWidget*>("securityTabWidget");
        saveCard("database_security", modal);
        saveCard("database_security_credentials", modal);
        if (tabs && tabs->count() > 1) {
            tabs->setCurrentIndex(1);
            saveCard("database_security_encryption", modal);
            tabs->setCurrentIndex(0);
        }
    }
    if (selectPage(dialog, "Remote Sync")) {
        saveCard("sync_remote_settings", modal);
    }
    if (selectPage(dialog, "Browser Integration")) {
        saveCard("browser_database_settings", modal);
    }
    if (selectPage(dialog, "Maintenance")) {
        saveCard("database_maintenance", modal);
    }
    selectPage(dialog, "General");
    closeModal(modal);
    m_dbWidget->switchToMainView();
}

void TestDocsScreenshots::passwordGenerator()
{
    auto* modal = openModal("actionPasswordGenerator", "vaultPasswordGeneratorModal");
    QVERIFY(modal);
    auto* generator = m_mainWindow->findChild<PasswordGeneratorWidget*>("passwordGeneratorWidget");
    auto* tabs = generator->findChild<QTabWidget*>("tabWidget");

    tabs->setCurrentIndex(PasswordGeneratorWidget::Password);
    saveCard("password_generator", modal);
    if (auto* advanced = generator->findChild<QAbstractButton*>("advancedModeToggle")) {
        advanced->setChecked(true);
        saveCard("password_generator_advanced", modal);
        advanced->setChecked(false);
    }
    tabs->setCurrentIndex(PasswordGeneratorWidget::Diceware);
    saveCard("passphrase_generator", modal);
    tabs->setCurrentIndex(PasswordGeneratorWidget::Password);
    closeModal(modal);
}

void TestDocsScreenshots::totpSetup()
{
    auto* modal = openModal("actionEntrySetupTotp", "vaultTotpSetupModal");
    QVERIFY(modal);
    saveCard("totp_setup", modal);
    closeModal(modal);
}

void TestDocsScreenshots::importWizard()
{
    auto* modal = qobject_cast<VaultImportModal*>(openModal("actionImport", "vaultImportModal"));
    QVERIFY(modal);
    auto* sources = modal->findChild<QListWidget*>("importTypeList");
    saveCard("import_wizard", modal);

    // The CSV import step, with a sample file loaded
    for (int i = 0; i < sources->count(); ++i) {
        if (sources->item(i)->data(Qt::UserRole) == ImportWizard::IMPORT_CSV) {
            sources->setCurrentRow(i);
        }
    }
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultImportContinue"), Qt::LeftButton);
    if (modal->currentStep() == int(VaultImportModal::DetailsStep)) {
        saveCard("csv_import", modal);
    }
    closeModal(modal);
}

// Light and dark side by side: the left half of the light window, the right half of the dark one
void TestDocsScreenshots::themeComparison()
{
    const auto light = capture({});
    config()->set(Config::GUI_ApplicationTheme, "dark");
    kpxcApp->applyTheme();
    const auto dark = capture({});

    QImage combined(light.size(), QImage::Format_ARGB32);
    QPainter painter(&combined);
    const int half = light.width() / 2;
    painter.drawImage(QPoint(0, 0), light, QRect(0, 0, half, light.height()));
    painter.drawImage(QPoint(half, 0), dark, QRect(half, 0, dark.width() - half, dark.height()));
    painter.end();

    const auto file = m_outputDir.absoluteFilePath("theme_comparison.png");
    QVERIFY2(combined.convertToFormat(QImage::Format_RGB888).save(file), qPrintable(file));
    m_saved << "theme_comparison.png";
}
