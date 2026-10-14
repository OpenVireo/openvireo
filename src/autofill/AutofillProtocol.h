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

#ifndef KEEPASSXC_AUTOFILLPROTOCOL_H
#define KEEPASSXC_AUTOFILLPROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>

/**
 * Wire format between KeePassXC and its AutoFill extension.
 *
 * Each frame is a 4-byte big-endian length followed by a UTF-8 JSON object.
 * The extension sends one request and reads one response. Requests name
 * their kind in "type"; responses carry "ok" and, when it is false, "error".
 * The extension implements the same format in KPXAutofillClient.m.
 */
namespace AutofillProtocol
{
    // Frames larger than this are refused, which bounds what a peer can make the app buffer
    constexpr quint32 MaxFrameSize = 1024 * 1024;

    namespace Request
    {
        constexpr auto Status = "status";
        constexpr auto ListCredentials = "listCredentials";
        constexpr auto GetCredential = "getCredential";
        constexpr auto GetOneTimeCode = "getOneTimeCode";
        constexpr auto PasskeyAssert = "passkeyAssert";
        constexpr auto PasskeyRegister = "passkeyRegister";
        constexpr auto RequestUnlock = "requestUnlock";
    } // namespace Request

    namespace Error
    {
        constexpr auto Locked = "locked";
        constexpr auto NotFound = "notFound";
        constexpr auto Denied = "denied";
        constexpr auto Canceled = "canceled";
        constexpr auto Disabled = "disabled";
        constexpr auto BadRequest = "badRequest";
        constexpr auto Failed = "failed";
    } // namespace Error

    QByteArray encodeFrame(const QJsonObject& message);

    QJsonObject success(const QJsonObject& fields = {});
    QJsonObject failure(const char* error);

    // Splits a byte stream into messages, keeping a partial frame until the rest arrives
    class FrameReader
    {
    public:
        void append(const QByteArray& data);
        // Next complete message, or an empty object when none is ready yet
        QJsonObject next();
        // A frame was too large or not a JSON object; the connection should be dropped
        bool hasError() const;

    private:
        QByteArray m_buffer;
        bool m_error = false;
    };
} // namespace AutofillProtocol

#endif // KEEPASSXC_AUTOFILLPROTOCOL_H
