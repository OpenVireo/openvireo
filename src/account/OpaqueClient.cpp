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

#include "OpaqueClient.h"

#include "config-keepassx.h"

#include <ovcore.h>

const QByteArray OpaqueClient::Context = QByteArrayLiteral("ov-opaque-v1");

namespace
{
    rust::Slice<const uint8_t> slice(const QByteArray& data)
    {
        return {reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size())};
    }

    rust::Slice<const uint8_t> slice(const OpaqueClient::SecureBytes& data)
    {
        return {data.data(), data.size()};
    }

    QByteArray toByteArray(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    // Moves a secret out of the core's buffer, wiping the buffer
    OpaqueClient::SecureBytes takeSecure(rust::Vec<uint8_t>& data)
    {
        OpaqueClient::SecureBytes secure(data.begin(), data.end());
        std::fill(data.begin(), data.end(), uint8_t(0));
        data.clear();
        return secure;
    }
} // namespace

bool OpaqueClient::isAvailable()
{
#ifdef KPXC_FEATURE_ACCOUNTS
    return true;
#else
    return false;
#endif
}

QByteArray OpaqueClient::userId(const QString& email)
{
    return email.trimmed().toLower().toUtf8();
}

bool OpaqueClient::createRegistrationRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* request)
{
    if (!isAvailable()) {
        return false;
    }
    try {
        auto start = ov::opaque_start_registration(slice(password));
        *request = toByteArray(start.message);
        *sec = takeSecure(start.state);
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

bool OpaqueClient::finalizeRegistration(const SecureBytes& sec,
                                        const QByteArray& response,
                                        const Ids& ids,
                                        QByteArray* record)
{
    if (!isAvailable()) {
        return false;
    }
    try {
        *record = toByteArray(
            ov::opaque_finish_registration(slice(sec), slice(response), slice(ids.user), slice(ids.server)));
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

bool OpaqueClient::createCredentialRequest(const SecureBytes& password, SecureBytes* sec, QByteArray* ke1)
{
    if (!isAvailable()) {
        return false;
    }
    try {
        auto start = ov::opaque_start_login(slice(password));
        *ke1 = toByteArray(start.message);
        *sec = takeSecure(start.state);
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}

bool OpaqueClient::recoverCredentials(const QByteArray& ke2,
                                      const SecureBytes& sec,
                                      const Ids& ids,
                                      QByteArray* authU,
                                      SecureBytes* sessionKey)
{
    if (!isAvailable()) {
        return false;
    }
    try {
        auto login =
            ov::opaque_finish_login(slice(sec), slice(ke2), slice(ids.user), slice(ids.server), slice(Context));
        *authU = toByteArray(login.auth_u);
        auto key = takeSecure(login.session_key);
        takeSecure(login.export_key);
        if (sessionKey) {
            *sessionKey = key;
        }
        return true;
    } catch (const rust::Error&) {
        return false;
    }
}
