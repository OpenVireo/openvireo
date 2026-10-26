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

#ifndef KEEPASSXC_TESTACCOUNTMANAGER_H
#define KEEPASSXC_TESTACCOUNTMANAGER_H

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
    void testAccessEventRefreshesAndReloads();
    void testLockClearsKeys();
    void testRestoreSessionAfterRestart();
    void testAccountVaultUsesTokenProvider();
    void testParsesAccountEvents();
    void testServerConfigWebUrl();
    void testFollowsAnEmailChangedElsewhere();
    void testExplainsAPlanLimit();
};

#endif // KEEPASSXC_TESTACCOUNTMANAGER_H
