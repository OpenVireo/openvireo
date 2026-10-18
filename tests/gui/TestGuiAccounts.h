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

#ifndef KEEPASSXC_TESTGUIACCOUNTS_H
#define KEEPASSXC_TESTGUIACCOUNTS_H

#include <QObject>
#include <QScopedPointer>
#include <QSharedPointer>

#include "gui/MainWindow.h"

namespace FakeAccount
{
    struct Server;
}

class TestGuiAccounts : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void testStartScreenShowsUntilAModeIsChosen();
    void testUseWithoutAnAccountMakesNoRequests();
    void testSignsInOnANewDeviceWithMfa();
    void testOtherMfaErrorsReturnToSignIn();
    void testPairsWithASelfHostedServer();
    void testSignUpShowsTheSecretKeyOnce();
    void testUnlockOpensEveryVaultAndLockClearsKeys();
    void testShareDialogRevokeRotatesTheKey();
    void testMovesADatabaseToTheAccount();
    void testServiceAccountsDialog();
    void testSignOutReturnsToTheStartScreen();

private:
    void createWindow(const QString& mode);
    // Register an account the way another device would, so this one hasn't seen its Secret Key
    QString registerElsewhere(const QString& email, bool mfa = false);
    // Sign in on the start screen
    void signIn(const QString& email, const QString& secretKey = {});

    QSharedPointer<FakeAccount::Server> m_server;
    QScopedPointer<MainWindow> m_mainWindow;
};

#endif // KEEPASSXC_TESTGUIACCOUNTS_H
