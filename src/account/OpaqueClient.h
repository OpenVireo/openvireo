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

#ifndef KEEPASSXC_OPAQUECLIENT_H
#define KEEPASSXC_OPAQUECLIENT_H

#include "AccountCrypto.h"

/**
 * The client half of OPAQUE (SPEC §8), a thin wrapper over the libopaque C core.
 *
 * The password never leaves the device: registration sends a blinded request and then a record the
 * server can't run a dictionary attack on without first answering each guess online, and login proves
 * knowledge of the password without sending it.
 *
 * Builds without libopaque (KPXC_FEATURE_ACCOUNTS off) get a client whose calls all fail; isAvailable()
 * tells them apart. The calls are virtual so tests can replace the protocol.
 */
class OpaqueClient
{
public:
    using SecureBytes = AccountCrypto::SecureBytes;

    struct Ids
    {
        // The account email, lowercased and trimmed
        QByteArray user;
        // The issuer host, e.g. accounts.openvireo.com
        QByteArray server;
    };

    static const QByteArray Context;

    virtual ~OpaqueClient() = default;

    static bool isAvailable();
    static QByteArray userId(const QString& email);

    // Registration step 1: `request` goes to register/start, `sec` stays here
    virtual bool createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request);
    // Registration step 2: turns the server's response into the record for register/finish
    virtual bool
    finalizeRegistration(const SecureBytes& sec, const QByteArray& response, const Ids& ids, QByteArray* record);

    // Login step 1: `ke1` goes to login/start, `sec` stays here
    virtual bool createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1);
    // Login step 2: fails for a wrong password (or an unknown email); otherwise `authU` goes to login/finish
    virtual bool recoverCredentials(const QByteArray& ke2,
                                    const SecureBytes& sec,
                                    const Ids& ids,
                                    QByteArray* authU,
                                    SecureBytes* sessionKey = nullptr);
};

#endif // KEEPASSXC_OPAQUECLIENT_H
