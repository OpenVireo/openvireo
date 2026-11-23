/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#ifndef OPENVIREO_TESTACCOUNTMANAGER_H
#define OPENVIREO_TESTACCOUNTMANAGER_H

#include <QObject>

class TestAccountManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void testRegisterAndSignIn();
    void testSignInWithMfa();
    void testFifthWrongMfaCodeEndsSignIn();
    void testWrongPasswordOrSecretKey();
    void testRefreshRotatesTokens();
    void testRefreshesExpiringTokens();
    void testReusedRefreshTokenSignsOut();
    void testServiceAccountExchange();
    void testKeyringOpensNewestKeyWithFallback();
    void testDownloadKeepsTheWholeVault();
    void testListsAndOpensVaultVersions();
    void testAccessEventRefreshesAndReloads();
    void testLockClearsKeys();
    void testDeleteAccountAndRestore();
    void testDeletionEventsSignOut();
    void testRestoreSessionAfterRestart();
    void testSwitchingKeepsTheOtherAccountSignedIn();
    void testChecksSessionsSetAside();
    void testAccountVaultUsesTokenProvider();
    void testParsesAccountEvents();
    void testServerConfigWebUrl();
    void testFollowsAnEmailChangedElsewhere();
    void testExplainsAPlanLimit();
    void testOfflineUnlockFromCache();
    void testOfflineUnlockNeedsACache();
    void testRefusedUnlockNeverUsesTheCache();
    void testGoesOfflineAndDropsRevokedVaultsOnReconnect();
    void testSessionEndWipesTheOfflineCache();
    void testOfflineUnlockCanBeTurnedOff();
};

#endif // OPENVIREO_TESTACCOUNTMANAGER_H
