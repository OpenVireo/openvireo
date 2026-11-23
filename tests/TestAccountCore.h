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

#ifndef OPENVIREO_TESTACCOUNTCORE_H
#define OPENVIREO_TESTACCOUNTCORE_H

#include <QObject>

class QTcpServer;

/*
 * openvireo-core on the desktop: the Qt Host (CoreHost) against a local HTTP server, the core facade through
 * it, sign-in to an account that libopaque registered (the build before openvireo-core), and the account-vault
 * merge, which runs in the core.
 */
class TestAccountCore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testTransport();
    void testTransportOffline();
    void testStream();
    void testDiscoveryThroughTheCore();
    void testSecretStore();
    void testFileCache();
    void testLibopaqueAccountSignsIn();
    void testAccountVaultMergeKeepsObjects();
    void testAccountOnTestServers();

private:
    QTcpServer* m_server = nullptr;
    QString m_base;
};

#endif // OPENVIREO_TESTACCOUNTCORE_H
