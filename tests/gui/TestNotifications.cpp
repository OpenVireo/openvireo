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

#include "TestNotifications.h"

#include "config-keepassx.h"
#include "core/BuildInfo.h"
#include "core/Config.h"
#include "crypto/Crypto.h"
#include "gui/Application.h"
#include "gui/MainWindow.h"
#include "gui/notifications/NotificationCenter.h"
#include "gui/notifications/NotificationWidgets.h"
#include "util/TemporaryFile.h"

#include <QDir>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

int main(int argc, char* argv[])
{
    Application app(argc, argv);
    app.setApplicationName("OpenVireo");
    app.setApplicationVersion(KEEPASSXC_VERSION);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    app.applyTheme();
    QTEST_DISABLE_KEYPAD_NAVIGATION
    TestNotifications tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

namespace
{
    // Saves a picture of the widget when OPENVIREO_VAULT_SCREENSHOT_DIR is set, as the Vault tests do
    void saveScreenshot(QWidget* widget, const QString& name)
    {
        const auto dir = qEnvironmentVariable("OPENVIREO_VAULT_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QTest::qWait(300);
            widget->grab().save(QDir(dir).absoluteFilePath(name));
        }
    }

    QWidget* visibleChild(QWidget* parent, const QString& name)
    {
        for (auto* child : parent->findChildren<QWidget*>(name)) {
            if (child->isVisible()) {
                return child;
            }
        }
        return nullptr;
    }
} // namespace

void TestNotifications::initTestCase()
{
    QVERIFY(Crypto::init());
    Config::createConfigFromFile(TemporaryFile::createTempConfigFile(), TemporaryFile::createTempConfigFile());
    Application::bootstrap();
    config()->set(Config::UpdateCheckMessageShown, true);
    config()->set(Config::Account_StartMode, "local");
    config()->remove(Config::Messages_HidePreReleaseWarning);
    BuildInfo::setKindForTests(BuildInfo::Kind::Nightly);
}

void TestNotifications::cleanup()
{
    if (m_mainWindow) {
        m_mainWindow->notificationCenter()->clearAll();
        m_mainWindow->notificationCenter()->markAllRead();
    }
}

void TestNotifications::cleanupTestCase()
{
    m_mainWindow.reset();
    BuildInfo::setKindForTests({});
}

MainWindow* TestNotifications::window()
{
    if (!m_mainWindow) {
        m_mainWindow.reset(new MainWindow());
        // Screenshots need capture allowed, and offscreen windows can't block it anyway
        m_mainWindow->setAllowScreenCapture(true);
        m_mainWindow->resize(1000, 700);
        m_mainWindow->show();
        m_mainWindow->activateWindow();
        QApplication::processEvents();
    }
    return m_mainWindow.data();
}

void TestNotifications::testPostAndRead()
{
    NotificationCenter center;
    QSignalSpy posted(&center, &NotificationCenter::posted);
    QVERIFY(center.isEmpty());

    center.post("first", MessageWidget::Information);
    center.post("second", MessageWidget::Positive);
    QCOMPARE(posted.count(), 2);
    QCOMPARE(center.unreadCount(), 2);
    // Newest first
    QCOMPARE(center.notifications().first().text, QString("second"));

    center.markAllRead();
    QCOMPARE(center.unreadCount(), 0);
    QCOMPARE(center.notifications().size(), 2);
}

void TestNotifications::testDuplicateBumpsEntry()
{
    NotificationCenter center;
    const auto id = center.post("Sync failed", MessageWidget::Error);
    center.post("Other", MessageWidget::Information);
    center.markAllRead();

    // The same message again refreshes the first entry: unread again and back on top
    QCOMPARE(center.post("Sync failed", MessageWidget::Error), id);
    QCOMPARE(center.notifications().size(), 2);
    QCOMPARE(center.notifications().first().id, id);
    QCOMPARE(center.unreadCount(), 1);

    // The same text with another type is a different message
    center.post("Sync failed", MessageWidget::Warning);
    QCOMPARE(center.notifications().size(), 3);
}

void TestNotifications::testHighestUnreadType()
{
    NotificationCenter center;
    QVERIFY(!center.highestUnreadType());
    center.post("a", MessageWidget::Information);
    QCOMPARE(*center.highestUnreadType(), MessageWidget::Information);
    center.post("b", MessageWidget::Error);
    center.post("c", MessageWidget::Warning);
    QCOMPARE(*center.highestUnreadType(), MessageWidget::Error);
    center.markAllRead();
    QVERIFY(!center.highestUnreadType());
}

void TestNotifications::testPinnedEntries()
{
    NotificationCenter center;
    int dismissed = 0;
    NotificationCenter::Notification pinned;
    pinned.text = "pinned";
    pinned.type = MessageWidget::Warning;
    pinned.pinned = true;
    pinned.onDismiss = [&dismissed] { ++dismissed; };
    const auto pinnedId = center.add(pinned);
    center.post("newer", MessageWidget::Information);

    // Pinned entries stay on top, however new the others are
    QCOMPARE(center.notifications().first().id, pinnedId);

    // Clear all leaves pinned entries, and isn't the user dismissing them
    center.clearAll();
    QCOMPARE(center.notifications().size(), 1);
    QCOMPARE(dismissed, 0);

    center.dismiss(pinnedId);
    QVERIFY(center.isEmpty());
    QCOMPARE(dismissed, 1);
}

void TestNotifications::testCap()
{
    NotificationCenter center;
    NotificationCenter::Notification pinned;
    pinned.text = "pinned";
    pinned.pinned = true;
    center.add(pinned);
    for (int i = 0; i < NotificationCenter::MaxEntries + 10; ++i) {
        center.post(QString("message %1").arg(i), MessageWidget::Information);
    }
    const auto notifications = center.notifications();
    QCOMPARE(notifications.size(), NotificationCenter::MaxEntries);
    QVERIFY(notifications.first().pinned);
    // The oldest messages went
    QCOMPARE(notifications.last().text, QString("message %1").arg(11));
}

void TestNotifications::testDismissLatestFrom()
{
    NotificationCenter center;
    center.post("from a, older", MessageWidget::Error, "a");
    center.post("from b", MessageWidget::Error, "b");
    center.post("from a, newer", MessageWidget::Error, "a");

    center.dismissLatestFrom("a");
    QStringList texts;
    for (const auto& notification : center.notifications()) {
        texts << notification.text;
    }
    QCOMPARE(texts, QStringList({"from b", "from a, older"}));
}

void TestNotifications::testNightlyNotice()
{
    auto* center = window()->notificationCenter();
    const auto notifications = center->notifications();
    QCOMPARE(notifications.size(), 1);
    QVERIFY(notifications.first().pinned);
    QCOMPARE(notifications.first().type, MessageWidget::Warning);
    QCOMPARE(notifications.first().title, QString("You're using a nightly build"));
    QVERIFY(notifications.first().text.contains(KEEPASSXC_VERSION));
    QCOMPARE(center->unreadCount(), 1);

    // No database open, so no header: the pill and the bell sit in the corner bar
    auto* cornerBar = window()->findChild<QWidget*>("notificationCornerBar");
    QVERIFY(cornerBar);
    QTRY_VERIFY(cornerBar->isVisible());
    auto* badge = qobject_cast<BuildBadge*>(visibleChild(cornerBar, "buildBadge"));
    QVERIFY(badge);
    QCOMPARE(badge->text(), QString("Nightly"));
    QCOMPARE(badge->accessibleName(), QString("Nightly build"));
    auto* bell = visibleChild(cornerBar, "notificationButton");
    QVERIFY(bell);
    QCOMPARE(bell->accessibleName(), QString("Notifications, 1 unread"));
    saveScreenshot(window(), "notifications-corner.png");
}

void TestNotifications::testGlobalMessagesGoToBell()
{
    auto* center = window()->notificationCenter();
    center->markAllRead();
    window()->displayGlobalMessage("First message", MessageWidget::Information);
    window()->displayGlobalMessage("Second message", MessageWidget::Positive);
    // A second message no longer replaces the first; both sit under the pinned notice
    const auto notifications = center->notifications();
    QCOMPARE(notifications.size(), 3);
    QVERIFY(notifications.at(0).pinned);
    QCOMPARE(notifications.at(1).text, QString("Second message"));
    QCOMPARE(notifications.at(2).text, QString("First message"));

    auto* bell = visibleChild(window()->findChild<QWidget*>("notificationCornerBar"), "notificationButton");
    QVERIFY(bell);
    QCOMPARE(bell->accessibleName(), QString("Notifications, 2 unread"));

    // The old shared bar is gone
    QVERIFY(!window()->findChild<MessageWidget*>("globalMessageWidget"));
}

void TestNotifications::testErrorPeeks()
{
    auto* peek = window()->findChild<NotificationPeek*>("notificationPeek");
    QVERIFY(peek);
    window()->displayGlobalMessage("Just so you know", MessageWidget::Information);
    QApplication::processEvents();
    QVERIFY(!peek->isOpen());

    window()->displayGlobalMessage("Something broke", MessageWidget::Error);
    QTRY_VERIFY(peek->isOpen());
    saveScreenshot(window(), "notifications-peek.png");
    // It doesn't take focus from what the user is doing
    QVERIFY(!peek->isAncestorOf(QApplication::focusWidget()));
    peek->close();
    QTRY_VERIFY(!peek->isOpen());
}

void TestNotifications::testFlyoutMarksRead()
{
    auto* center = window()->notificationCenter();
    window()->displayGlobalMessage("Read me", MessageWidget::Information);
    QCOMPARE(center->unreadCount(), 1);

    auto* flyout = window()->findChild<NotificationFlyout*>("notificationFlyout");
    QVERIFY(flyout);
    window()->showNotifications();
    QTRY_VERIFY(flyout->isOpen());
    // The pinned notice and the message
    QCOMPARE(flyout->findChildren<QWidget*>("notificationRow").size(), 2);
    saveScreenshot(window(), "notifications-flyout.png");

    flyout->close();
    QTRY_COMPARE(center->unreadCount(), 0);
    QCOMPARE(center->notifications().size(), 2);
}

void TestNotifications::testYubiKeyPromptStaysInline()
{
    auto* center = window()->notificationCenter();
    const int before = center->notifications().size();
    auto* prompt = window()->findChild<MessageWidget*>("promptBar");
    QVERIFY(prompt);
    QVERIFY(!prompt->isVisibleTo(window()));

    window()->showYubiKeyPopup();
    QTRY_VERIFY(prompt->isVisibleTo(window()));
    QVERIFY(prompt->text().contains("YubiKey"));
    QVERIFY(!window()->isEnabled());
    QTRY_VERIFY(!prompt->isShowAnimationRunning());
    saveScreenshot(window(), "notifications-prompt.png");
    // A prompt to act on now isn't filed away behind the bell
    QCOMPARE(center->notifications().size(), before);

    window()->hideYubiKeyPopup();
    QTRY_VERIFY(!prompt->isVisibleTo(window()));
    QVERIFY(window()->isEnabled());
}

void TestNotifications::testDismissNoticeForThisVersion()
{
    auto* center = window()->notificationCenter();
    QCOMPARE(center->notifications().size(), 1);
    center->dismiss(center->notifications().first().id);
    QVERIFY(center->isEmpty());
    QCOMPARE(config()->get(Config::Messages_HidePreReleaseWarning).toString(), QString(KEEPASSXC_VERSION));

    // Not again for this version, as at the next launch, though the pill still says what the build is
    QMetaObject::invokeMethod(window(), "postPreReleaseNotice");
    QVERIFY(center->isEmpty());
    auto* cornerBar = window()->findChild<QWidget*>("notificationCornerBar");
    QTRY_VERIFY(cornerBar->isVisible());
    QVERIFY(visibleChild(cornerBar, "buildBadge"));

    // A new version shows it again, once
    config()->set(Config::Messages_HidePreReleaseWarning, "0.0.1");
    QMetaObject::invokeMethod(window(), "postPreReleaseNotice");
    QMetaObject::invokeMethod(window(), "postPreReleaseNotice");
    QCOMPARE(center->notifications().size(), 1);
}

void TestNotifications::testStableBuild()
{
    BuildInfo::setKindForTests(BuildInfo::Kind::Stable);
    BuildBadge badge;
    QVERIFY(badge.isHidden());
    QVERIFY(BuildInfo::shortLabel().isEmpty());

    // With nothing to show, a stable build has no corner bar
    auto* center = window()->notificationCenter();
    for (const auto& notification : center->notifications()) {
        center->dismiss(notification.id);
    }
    auto* cornerBar = window()->findChild<QWidget*>("notificationCornerBar");
    QTRY_VERIFY(!cornerBar->isVisible());
    // A message brings the bell back
    window()->displayGlobalMessage("Hello", MessageWidget::Information);
    QTRY_VERIFY(cornerBar->isVisible());

    BuildInfo::setKindForTests(BuildInfo::Kind::Nightly);
}

void TestNotifications::testMessageWidgetKeepsApi()
{
    // Restyled with Fluent colors, but callers use it as before
    MessageWidget widget;
    widget.setAnimate(false);
    widget.resize(400, 60);
    widget.showMessage("Hello", MessageWidget::Error, MessageWidget::DisableAutoHide);
    QCOMPARE(widget.text(), QString("Hello"));
    QCOMPARE(widget.messageType(), MessageWidget::Error);
    QVERIFY(widget.isVisible());
    widget.setCloseButtonVisible(false);
    QVERIFY(!widget.isCloseButtonVisible());
    widget.hideMessage();
    QVERIFY(!widget.isVisible());

    // Every type, for a look at the colors
    QWidget sheet;
    auto* layout = new QVBoxLayout(&sheet);
    const QList<QPair<MessageWidget::MessageType, QString>> types = {
        {MessageWidget::Information, "Select a vault of your account to share."},
        {MessageWidget::Positive, "Remote sync 'Work' completed successfully!"},
        {MessageWidget::Warning, "This build of OpenVireo is not signed for AutoFill, so AutoFill stays off."},
        {MessageWidget::Error, "Failed to open Taxes.kdbx. It either does not exist or is not accessible."}};
    for (const auto& [type, text] : types) {
        auto* message = new MessageWidget(&sheet);
        message->setAnimate(false);
        message->showMessage(text, type, MessageWidget::DisableAutoHide);
        layout->addWidget(message);
    }
    sheet.resize(640, 260);
    sheet.show();
    saveScreenshot(&sheet, "message-widgets.png");
}
