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

#ifndef KEEPASSXC_TESTREMOTESYNCSERVER_H
#define KEEPASSXC_TESTREMOTESYNCSERVER_H

#include <QObject>

class TestRemoteSyncServer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testSettingsRoundTrip();
    void testTokenStaysOnThisDevice();
    void testMovesTokenOutOfOlderDatabases();
    void testForgetsTokensOfRemovedRemotes();
    void testTokenFileWithoutSystemStore();
    void testTokenMovesFromFileToSystemStore();
    void testTokensAreReadOnce();
    void testPlatformTokenStore();
    void testSyncServerCycle();
    void testSyncServerRejectsBadToken();
    void testSyncEngineTwoDevices();
    void testHistoryAndEvents();
    void testRecognisesPairingCodes();
    void testPairsAnotherDevice();
};

#endif // KEEPASSXC_TESTREMOTESYNCSERVER_H
