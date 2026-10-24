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
#include "gui/account/AccountStartWidget.h"
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
#include "keeshare/KeeShare.h"
#include "keeshare/KeeShareSettings.h"
#include "mock/MockClock.h"

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

#include <functional>

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
    // The website frames its screenshots at roughly 4:3
    const QSize kSiteWindowSize(1180, 900);
    // Slots named website* capture the website's product screenshots
    const auto kSitePrefix = QLatin1String("website");
    const QColor kCalloutColor(0x3c, 0x7d, 0x20); // KeePassXC brand green

    // Everything shown is fixed, so re-running only changes images whose UI changed: the clock
    // (timestamps, expiry, TOTP codes), entry and group ids, generated passwords, and the
    // KeeShare certificate all come from here.
    const QUuid kUuidNamespace("{6b3a2e1c-5d4f-4c8e-9a7b-1f2e3d4c5b6a}");
    const auto kPassword = QStringLiteral("q7#Vt2!mWz9$kLp4Rx8@");
    const auto kPassphrase = QStringLiteral("orbit-candle-maple-river-signal-velvet");
    const auto kKeeShareFixture = QStringLiteral("DocsScreenshotsKeeShare.txt");

    QUuid uuidFor(const QString& name)
    {
        return QUuid::createUuidV5(kUuidNamespace, name);
    }

    // Whether an existing screenshot already shows the same thing. A handful of pixels can still
    // vary between runs (hover highlights follow wherever the real mouse pointer is), so up to
    // 0.2% of pixels may differ before the file counts as changed.
    bool sameScreenshot(const QString& file, const QImage& image)
    {
        auto existing = QImage(file);
        if (existing.isNull() || existing.size() != image.size()) {
            return false;
        }
        existing = existing.convertToFormat(QImage::Format_RGB888);
        qint64 differing = 0;
        const qint64 allowed = qint64(image.width()) * image.height() / 500;
        for (int y = 0; y < image.height(); ++y) {
            const auto* a = existing.constScanLine(y);
            const auto* b = image.constScanLine(y);
            for (int x = 0; x < image.width() * 3; x += 3) {
                if (a[x] != b[x] || a[x + 1] != b[x + 1] || a[x + 2] != b[x + 2]) {
                    if (++differing > allowed) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // Lets a modal's nested event loop run a block, as the new database wizard needs
    template <typename F> void whileOpen(QObject* context, F&& body)
    {
        QTimer::singleShot(0, context, std::forward<F>(body));
    }
} // namespace

// Setup -----------------------------------------------------------------------------------------

void TestDocsScreenshots::initTestCase()
{
    m_docsDir = qEnvironmentVariable("KPXC_DOCS_SCREENSHOT_DIR");
    m_siteDir = qEnvironmentVariable("KPXC_SITE_SCREENSHOT_DIR");
    if (m_docsDir.isEmpty() && m_siteDir.isEmpty()) {
        QSKIP("Set KPXC_DOCS_SCREENSHOT_DIR (docs/images) and/or KPXC_SITE_SCREENSHOT_DIR (website) to save "
              "screenshots, or build the docs_screenshots target");
    }
    QVERIFY(m_tempDir.isValid());

    QVERIFY(Crypto::init());
    Config::createConfigFromFile(m_tempDir.filePath("config.ini"), m_tempDir.filePath("local.ini"));
    QLocale::setDefault(QLocale::c());
    Application::bootstrap();
    MockClock::setup(new MockClock(2026, 1, 15, 9, 30, 0));
    QApplication::setCursorFlashTime(0); // a steady caret, not one caught mid-blink

    // KeeShare signs with a generated certificate, so keep one as a fixture. The first run
    // creates it; commit it so later runs (and other machines) show the same one.
    const auto fixture = QDir(KEEPASSX_TEST_DATA_DIR).absoluteFilePath(kKeeShareFixture);
    QFile keeShareFile(fixture);
    if (keeShareFile.open(QIODevice::ReadOnly)) {
        config()->set(Config::KeeShare_Own, QString::fromUtf8(keeShareFile.readAll()));
    } else {
        auto own = KeeShareSettings::Own::generate();
        own.certificate.signer = QStringLiteral("John Doe"); // not the account running the tool
        const auto serialized = KeeShareSettings::Own::serialize(own);
        config()->set(Config::KeeShare_Own, serialized);
        QVERIFY(keeShareFile.open(QIODevice::WriteOnly));
        keeShareFile.write(serialized.toUtf8());
        qInfo().noquote() << "Created" << fixture << "- commit it so screenshots stay the same";
    }

    config()->set(Config::GUI_ApplicationTheme, "light");
    config()->set(Config::AutoSaveAfterEveryChange, false);
    config()->set(Config::AutoSaveOnExit, false);
    config()->set(Config::UpdateCheckMessageShown, true);
    config()->set(Config::Security_QuickUnlock, false);
    config()->set(Config::GUI_ShowExpiredEntriesOnDatabaseUnlock, false);
    // Development builds show a pre-release banner that has no place in the docs
    config()->set(Config::Messages_HidePreReleaseWarning, KEEPASSXC_VERSION);
    kpxcApp->applyTheme();

    // Today's local experience, without the account start screen

    config()->set(Config::Account_StartMode, "local");

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
    // Docs and website screenshots each run only when their folder is set
    const bool site = QLatin1String(QTest::currentTestFunction()).startsWith(kSitePrefix);
    const auto& dir = site ? m_siteDir : m_docsDir;
    if (dir.isEmpty()) {
        QSKIP(site ? "KPXC_SITE_SCREENSHOT_DIR not set" : "KPXC_DOCS_SCREENSHOT_DIR not set");
    }
    m_outputDir = QDir(dir);
    QVERIFY(m_outputDir.mkpath("."));
    if (!m_dbWidget) {
        return;
    }
    for (auto* modal : m_mainWindow->findChildren<VaultModal*>()) {
        closeModal(modal);
    }
    m_dbWidget->switchToMainView();
    config()->set(Config::GUI_ApplicationTheme, "light");
    kpxcApp->applyTheme();
    m_mainWindow->resize(site ? kSiteWindowSize : kWindowSize);
    selectGroup(m_workGroup);
    selectEntry(m_github);
}

void TestDocsScreenshots::cleanupTestCase()
{
    MockClock::teardown();
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
        QProcess::execute(optipng, QStringList({"-quiet", "-o2", "-strip", "all"}) + m_saved);
    }
    qInfo().noquote() << QString("%1 screenshots updated, %2 unchanged").arg(m_saved.size()).arg(m_unchanged.size());
    if (!m_saved.isEmpty()) {
        qInfo().noquote() << "Updated:\n  " + m_saved.join("\n  ");
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
        group->setUuid(uuidFor(name));
        group->setName(name);
        group->setParent(root);
        return group;
    };
    auto addEntry = [](Group* group, const QString& title, const QString& username, const QString& url,
                       const QString& password) {
        auto* entry = new Entry();
        entry->setUuid(uuidFor(title));
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
    vpn->setExpiryTime(Clock::currentDateTimeUtc().addDays(-3));
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

    writeImage(m_outputDir.absoluteFilePath(name + ".png"), image);
}

// Saves a screenshot, leaving the file alone when it already shows the same thing, so re-runs
// only touch images whose UI changed
void TestDocsScreenshots::writeImage(const QString& file, const QImage& image)
{
    const auto rgb = image.convertToFormat(QImage::Format_RGB888);
    if (sameScreenshot(file, rgb)) {
        m_unchanged << file;
        return;
    }
    QVERIFY2(rgb.save(file), qPrintable(file));
    m_saved << file;
}

void TestDocsScreenshots::saveCard(const QString& name, VaultModal* modal, const QList<Callout>& callouts)
{
    QVERIFY(modal);
    save(name, callouts, cardRect(modal));
}

// A website screenshot: the whole window without callouts (or the given image), plus a half-size
// thumbnail in thumbs/
void TestDocsScreenshots::saveSite(const QString& name, QImage image)
{
    if (image.isNull()) {
        image = capture({});
    }
    writeImage(m_outputDir.absoluteFilePath(name + ".png"), image);
    QVERIFY(m_outputDir.mkpath("thumbs"));
    writeImage(m_outputDir.absoluteFilePath("thumbs/" + name + ".png"),
               image.scaledToWidth(image.width() / 2, Qt::SmoothTransformation));
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

// The start screen that asks how to use KeePassXC, with the account sign-in form, shown in place of
// the welcome screen as MainWindow::showStartScreen() does
void TestDocsScreenshots::showStartScreen(bool show)
{
    auto* start = m_mainWindow->startScreen();
    auto* welcome = m_mainWindow->findChild<QWidget*>("welcomeWidget");
    QVERIFY(start && welcome);
    if (show) {
        start->reset();
    }
    welcome->setVisible(!show);
    start->setVisible(show);
}

void TestDocsScreenshots::welcomeAndUnlock()
{
    auto* stack = m_mainWindow->findChild<QStackedWidget*>("stackedWidget");
    const auto index = stack->currentIndex();
    stack->setCurrentIndex(MainWindow::WelcomeScreen);
    save("welcome_screen");
    showStartScreen(true);
    save("account_start_screen");
    showStartScreen(false);
    stack->setCurrentIndex(index);

    QVERIFY(m_dbWidget->lock());
    auto* passwordWidget = m_dbWidget->findChild<PasswordWidget*>("editPassword");
    // The key file field stays folded away behind this link until it is needed
    auto* keyFile = m_dbWidget->findChild<QWidget*>("addKeyFileLinkLabel");
    save("unlock_database", {{"A", rectOf(passwordWidget)}, {"B", rectOf(keyFile)}});

    unlock();
}

void TestDocsScreenshots::unlock()
{
    auto* editPassword = m_dbWidget->findChild<PasswordWidget*>("editPassword")->findChild<QLineEdit*>("passwordEdit");
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
    walkNewDatabaseWizard([this](VaultModal* modal, const QString& name) { saveCard(name, modal); });
}

// Fills in the new database wizard page by page (new_db_wizard_1 to _3), handing each to shot,
// then cancels it
void TestDocsScreenshots::walkNewDatabaseWizard(const Shot& shot)
{
    bool done = false;
    whileOpen(this, [&] {
        auto* modal = m_mainWindow->findChild<VaultNewDatabaseModal*>("vaultNewDatabaseModal");
        if (!modal || !modal->isOpen()) {
            return;
        }
        auto* next = modal->findChild<QPushButton*>("vaultNewDatabaseContinue");
        QTest::keyClicks(modal->findChild<QLineEdit*>("databaseName"), "Passwords");
        shot(modal, "new_db_wizard_1");
        QTest::mouseClick(next, Qt::LeftButton);
        shot(modal, "new_db_wizard_2");
        QTest::mouseClick(next, Qt::LeftButton);
        auto* password = modal->findChild<PasswordWidget*>("enterPasswordEdit");
        QTest::keyClicks(password->findChild<QLineEdit*>("passwordEdit"), "correct horse battery staple");
        QTest::keyClicks(modal->findChild<PasswordWidget*>("repeatPasswordEdit")->findChild<QLineEdit*>("passwordEdit"),
                         "correct horse battery staple");
        shot(modal, "new_db_wizard_3");
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
    walkPasswordGenerator([this](VaultModal* modal, const QString& name) { saveCard(name, modal); });
}

// The password generator in its simple and advanced modes and the passphrase generator
// (password_generator, password_generator_advanced, passphrase_generator)
void TestDocsScreenshots::walkPasswordGenerator(const Shot& shot)
{
    auto* modal = openModal("actionPasswordGenerator", "vaultPasswordGeneratorModal");
    QVERIFY(modal);
    auto* generator = m_mainWindow->findChild<PasswordGeneratorWidget*>("passwordGeneratorWidget");
    auto* tabs = generator->findChild<QTabWidget*>("tabWidget");

    // Each change regenerates the password, so pin what it shows just before every capture
    auto* output = generator->findChild<PasswordWidget*>("editNewPassword");
    tabs->setCurrentIndex(PasswordGeneratorWidget::Password);
    output->setText(kPassword);
    shot(modal, "password_generator");
    if (auto* advanced = generator->findChild<QAbstractButton*>("advancedModeToggle")) {
        advanced->setChecked(true);
        output->setText(kPassword);
        shot(modal, "password_generator_advanced");
        advanced->setChecked(false);
    }
    tabs->setCurrentIndex(PasswordGeneratorWidget::Diceware);
    output->setText(kPassphrase);
    shot(modal, "passphrase_generator");
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
    walkImportWizard([this](VaultModal* modal, const QString& name) { saveCard(name, modal); });
}

// The import wizard's choice of source (import_wizard), then its CSV step with a sample file
// loaded (csv_import)
void TestDocsScreenshots::walkImportWizard(const Shot& shot)
{
    auto* modal = qobject_cast<VaultImportModal*>(openModal("actionImport", "vaultImportModal"));
    QVERIFY(modal);
    auto* sources = modal->findChild<QListWidget*>("importTypeList");
    shot(modal, "import_wizard");

    for (int i = 0; i < sources->count(); ++i) {
        if (sources->item(i)->data(Qt::UserRole) == ImportWizard::IMPORT_CSV) {
            sources->setCurrentRow(i);
        }
    }
    QTest::mouseClick(modal->findChild<QPushButton*>("vaultImportContinue"), Qt::LeftButton);
    if (modal->currentStep() == int(VaultImportModal::DetailsStep)) {
        shot(modal, "csv_import");
    }
    closeModal(modal);
}

void TestDocsScreenshots::themeComparison()
{
    writeImage(m_outputDir.absoluteFilePath("theme_comparison.png"), themeComparisonImage());
}

// Light and dark side by side: the left half of the light window, the right half of the dark one.
// Leaves the dark theme on; init() restores the light one.
QImage TestDocsScreenshots::themeComparisonImage()
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
    return combined;
}

// Website -----------------------------------------------------------------------------------------
// The product screenshots on the landing page, the Screenshots page and the download carousel:
// whole windows without callouts, each with a thumbnail (see saveSite)

void TestDocsScreenshots::websiteMainWindow()
{
    // The hero: the main window with a fully featured entry selected
    saveSite("database_view");

    auto* stack = m_mainWindow->findChild<QStackedWidget*>("stackedWidget");
    const auto index = stack->currentIndex();
    stack->setCurrentIndex(MainWindow::WelcomeScreen);
    saveSite("welcome_screen");
    showStartScreen(true);
    saveSite("account_start_screen");
    showStartScreen(false);
    stack->setCurrentIndex(index);
}

void TestDocsScreenshots::websiteEntryEditor()
{
    m_dbWidget->switchToEntryEdit();
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::EditEntryMode);
    auto* editor = m_mainWindow->findChild<VaultEntryEditCard*>("vaultEntryEditCard")->editor();

    saveSite("edit_entry");
    if (selectPage(editor, "Icon")) {
        saveSite("edit_entry_icons");
    }
    if (selectPage(editor, "Auto-Type")) {
        // A window association, so the page shows what Auto-Type can be tuned with
        if (auto* add = editor->findChild<QAbstractButton*>("assocAddButton")) {
            QTest::mouseClick(add, Qt::LeftButton);
            if (auto* window = editor->findChild<QComboBox*>("windowTitleCombo")) {
                window->setEditText("*GitHub*");
            }
        }
        saveSite("autotype_entrylevel");
    }
    if (selectPage(editor, "SSH Agent")) {
        saveSite("sshagent_entry_settings");
    }

    // Leave without keeping the association added above
    MessageBox::setNextAnswer(MessageBox::Discard);
    QTest::mouseClick(editor->buttonBox()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    MessageBox::setNextAnswer(MessageBox::NoButton);
    QTRY_COMPARE(m_dbWidget->currentMode(), DatabaseWidget::Mode::ViewMode);
}

void TestDocsScreenshots::websiteDialogs()
{
    // The site shows only some of what the walkthroughs offer
    const QStringList shown = {"new_db_wizard_1",
                               "new_db_wizard_2",
                               "import_wizard",
                               "csv_import",
                               "password_generator_advanced",
                               "passphrase_generator"};
    const auto shot = [this, &shown](VaultModal*, const QString& name) {
        if (shown.contains(name)) {
            saveSite(name);
        }
    };
    walkNewDatabaseWizard(shot);
    walkImportWizard(shot);
    walkPasswordGenerator(shot);

    // The health check, with its weak and expired passwords
    if (auto* reports = openModal("actionReports", "vaultReportsModal")) {
        auto* dialog = m_mainWindow->findChild<QWidget*>("reportsDialog");
        selectPage(dialog, "Health Check");
        for (auto* box : dialog->findChildren<QAbstractButton*>()) {
            if (box->isVisible() && box->text() == "Show expired entries") {
                box->click();
            }
        }
        saveSite("database_reports");
        closeModal(reports);
    }
}

// Last, since unlocking reloads the database
void TestDocsScreenshots::websiteUnlock()
{
    QVERIFY(m_dbWidget->lock());
    saveSite("unlock_database");
    unlock();
}

