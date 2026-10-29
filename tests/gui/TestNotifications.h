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

#ifndef KEEPASSXC_TESTNOTIFICATIONS_H
#define KEEPASSXC_TESTNOTIFICATIONS_H

#include "gui/MainWindow.h"

#include <QObject>
#include <QScopedPointer>

class TestNotifications : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();

    void testPostAndRead();
    void testDuplicateBumpsEntry();
    void testHighestUnreadType();
    void testPinnedEntries();
    void testCap();
    void testDismissLatestFrom();
    void testMessageWidgetKeepsApi();

    // These share one main window of a nightly build, as the app can't create a second one in a process
    void testNightlyNotice();
    void testGlobalMessagesGoToBell();
    void testErrorPeeks();
    void testFlyoutMarksRead();
    void testYubiKeyPromptStaysInline();
    void testDismissNoticeForThisVersion();
    void testStableBuild();
    void testBetaBuild();

private:
    MainWindow* window();

    QScopedPointer<MainWindow> m_mainWindow;
};

#endif // KEEPASSXC_TESTNOTIFICATIONS_H
