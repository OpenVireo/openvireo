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

#ifndef KEEPASSXC_TESTACCOUNTCRYPTO_H
#define KEEPASSXC_TESTACCOUNTCRYPTO_H

#include <QJsonObject>
#include <QObject>

class TestAccountCrypto : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void testBase64Url();
    void testSecretKey();
    void testSecretKeyParsing();
    void testAuk();
    void testKeyset();
    void testPublicKeys();
    void testSeal();
    void testSealNegative();
    void testSealRoundTrip();
    void testVaultName();
    void testServiceAccount();
    void testRefreshTokenHash();
    void testJwtClaims();
    void testVaultFixture();
    void testVaultKeyRoundTrip();

private:
    QJsonObject m_vectors;
};

#endif // KEEPASSXC_TESTACCOUNTCRYPTO_H
