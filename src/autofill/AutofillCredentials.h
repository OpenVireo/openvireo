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

#ifndef KEEPASSXC_AUTOFILLCREDENTIALS_H
#define KEEPASSXC_AUTOFILLCREDENTIALS_H

#include <QByteArray>
#include <QList>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QUuid>

class Database;
class Entry;

// One record for the system's credential identity store. It holds no secrets.
struct AutofillIdentity
{
    enum class Type
    {
        Password,
        Passkey,
        OneTimeCode
    };

    Type type = Type::Password;
    // Host for passwords and one-time codes, relying party for passkeys
    QString service;
    QString user;
    QString recordId;
    // Passkeys only, raw bytes
    QByteArray credentialId;
    QByteArray userHandle;

    bool operator==(const AutofillIdentity& other) const;
};

/**
 * Maps database entries to AutoFill identities and finds the entries a request
 * asks for. Free of AppKit so that it can be tested on its own.
 */
namespace AutofillCredentials
{
    // "<database root group uuid>:<entry uuid>", both as hex
    QString recordId(const Database* db, const Entry* entry);
    bool parseRecordId(const QString& recordId, QUuid* databaseId, QUuid* entryId);
    QUuid databaseId(const Database* db);

    // Entries left out of AutoFill: recycled, expired, or hidden from the browser
    bool isEntryExposed(const Entry* entry);

    QList<AutofillIdentity> identitiesForDatabase(const Database* db);

    // Lower-case host of an entry or request URL without a leading "www.", or empty when there is none
    QString normalizedHost(const QString& url);
    // A request for login.example.com matches an entry for example.com, but not the other way around
    bool hostMatches(const QString& entryHost, const QString& requestHost);
    bool entryMatchesHost(const Entry* entry, const QString& requestHost);

    Entry*
    findEntry(const QList<QSharedPointer<Database>>& databases, const QString& recordId, Database** db = nullptr);
    // Matching entries for the requested hosts across the databases, or all exposed entries when no host is given
    QList<Entry*> matchingEntries(const QList<QSharedPointer<Database>>& databases, const QStringList& requestHosts);
    QList<Entry*> passkeyEntries(const QList<QSharedPointer<Database>>& databases, const QString& rpId);

    // Passkey attributes are stored base64url encoded; these return raw bytes
    QByteArray passkeyCredentialId(const Entry* entry);
    QByteArray passkeyUserHandle(const Entry* entry);
} // namespace AutofillCredentials

#endif // KEEPASSXC_AUTOFILLCREDENTIALS_H
