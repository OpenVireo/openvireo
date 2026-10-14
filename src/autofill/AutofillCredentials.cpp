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

#include "AutofillCredentials.h"

#include "browser/BrowserEntryConfig.h"
#include "browser/BrowserService.h"
#include "browser/PasskeyUtils.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Tools.h"

#include <QSet>
#include <QUrl>

bool AutofillIdentity::operator==(const AutofillIdentity& other) const
{
    return type == other.type && service == other.service && user == other.user && recordId == other.recordId
           && credentialId == other.credentialId && userHandle == other.userHandle;
}

namespace
{
    bool isGroupHidden(const Group* group)
    {
        return group->isRecycled()
               || group->resolveCustomDataTriState(BrowserService::OPTION_HIDE_ENTRY) == Group::Enable;
    }

    QList<Entry*> exposedEntries(const Database* db)
    {
        QList<Entry*> entries;
        if (!db || !db->rootGroup()) {
            return entries;
        }

        for (const auto* group : db->rootGroup()->groupsRecursive(true)) {
            if (isGroupHidden(group)) {
                continue;
            }
            for (auto* entry : group->entries()) {
                if (AutofillCredentials::isEntryExposed(entry)) {
                    entries << entry;
                }
            }
        }
        return entries;
    }

    // Hosts of the entry's URLs, its additional URLs and its passkey's relying party, without duplicates
    QStringList entryHosts(const Entry* entry)
    {
        QStringList hosts;
        for (const auto& url : entry->getAllUrls()) {
            const auto host = AutofillCredentials::normalizedHost(url);
            if (!host.isEmpty() && !hosts.contains(host)) {
                hosts << host;
            }
        }
        return hosts;
    }

    bool isDenied(const Entry* entry, const QString& host)
    {
        BrowserEntryConfig config;
        return config.load(entry) && config.isDenied(host);
    }

    QString username(const Entry* entry)
    {
        return entry->resolveMultiplePlaceholders(entry->username());
    }
} // namespace

namespace AutofillCredentials
{
    QUuid databaseId(const Database* db)
    {
        // The root group's uuid is stored in the file, unlike the Database object's own uuid
        return db && db->rootGroup() ? db->rootGroup()->uuid() : QUuid();
    }

    QString recordId(const Database* db, const Entry* entry)
    {
        return Tools::uuidToHex(databaseId(db)) + ":" + entry->uuidToHex();
    }

    bool parseRecordId(const QString& recordId, QUuid* databaseId, QUuid* entryId)
    {
        const auto parts = recordId.split(':');
        if (parts.size() != 2 || !Tools::isHex(parts[0].toLatin1()) || !Tools::isHex(parts[1].toLatin1())) {
            return false;
        }
        const auto dbUuid = Tools::hexToUuid(parts[0]);
        const auto entryUuid = Tools::hexToUuid(parts[1]);
        if (dbUuid.isNull() || entryUuid.isNull()) {
            return false;
        }
        if (databaseId) {
            *databaseId = dbUuid;
        }
        if (entryId) {
            *entryId = entryUuid;
        }
        return true;
    }

    bool isEntryExposed(const Entry* entry)
    {
        if (!entry || entry->isRecycled() || entry->isExpired()) {
            return false;
        }
        const auto hidden = entry->customData()->value(BrowserService::OPTION_HIDE_ENTRY) == TRUE_STR;
        if (!hidden) {
            return true;
        }
        // An entry's own setting applies only while its group inherits
        return entry->group()
               && entry->group()->resolveCustomDataTriState(BrowserService::OPTION_HIDE_ENTRY) == Group::Disable;
    }

    QList<AutofillIdentity> identitiesForDatabase(const Database* db)
    {
        QList<AutofillIdentity> identities;
        for (const auto* entry : exposedEntries(db)) {
            const auto id = recordId(db, entry);
            const auto user = username(entry);
            const auto hasPassword = !entry->password().isEmpty();
            const auto hasTotp = entry->hasValidTotp();

            if (hasPassword || hasTotp) {
                for (const auto& host : entryHosts(entry)) {
                    if (isDenied(entry, host)) {
                        continue;
                    }
                    if (hasPassword) {
                        AutofillIdentity identity;
                        identity.type = AutofillIdentity::Type::Password;
                        identity.service = host;
                        identity.user = user;
                        identity.recordId = id;
                        identities << identity;
                    }
                    if (hasTotp) {
                        AutofillIdentity identity;
                        identity.type = AutofillIdentity::Type::OneTimeCode;
                        identity.service = host;
                        // The system shows the label next to the code, so name the account
                        identity.user = user.isEmpty() ? entry->title() : user;
                        identity.recordId = id;
                        identities << identity;
                    }
                }
            }

            if (entry->hasPasskey()) {
                AutofillIdentity identity;
                identity.type = AutofillIdentity::Type::Passkey;
                identity.service = entry->attributes()->value(EntryAttributes::KPEX_PASSKEY_RELYING_PARTY);
                identity.user = passkeyUtils()->getUsernameFromEntry(entry);
                identity.recordId = id;
                identity.credentialId = passkeyCredentialId(entry);
                identity.userHandle = passkeyUserHandle(entry);
                if (!identity.service.isEmpty() && !identity.credentialId.isEmpty()) {
                    identities << identity;
                }
            }
        }
        return identities;
    }

    QString normalizedHost(const QString& url)
    {
        const auto trimmed = url.trimmed();
        if (trimmed.isEmpty()) {
            return {};
        }

        // Entries often hold a bare host, which QUrl would read as a path
        const auto qurl = trimmed.contains("://") ? QUrl(trimmed) : QUrl::fromUserInput(trimmed);
        const auto scheme = qurl.scheme().toLower();
        if (scheme != "http" && scheme != "https") {
            return {};
        }

        auto host = qurl.host().toLower();
        if (host.startsWith("www.")) {
            host.remove(0, 4);
        }
        return host;
    }

    bool hostMatches(const QString& entryHost, const QString& requestHost)
    {
        if (entryHost.isEmpty() || requestHost.isEmpty()) {
            return false;
        }
        return requestHost == entryHost || requestHost.endsWith("." + entryHost);
    }

    bool entryMatchesHost(const Entry* entry, const QString& requestHost)
    {
        const auto hosts = entryHosts(entry);
        return std::any_of(
                   hosts.begin(), hosts.end(), [&](const QString& host) { return hostMatches(host, requestHost); })
               && !isDenied(entry, requestHost);
    }

    Entry* findEntry(const QList<QSharedPointer<Database>>& databases, const QString& recordId, Database** db)
    {
        QUuid dbUuid;
        QUuid entryUuid;
        if (!parseRecordId(recordId, &dbUuid, &entryUuid)) {
            return nullptr;
        }

        for (const auto& database : databases) {
            if (databaseId(database.data()) != dbUuid) {
                continue;
            }
            auto* entry = database->rootGroup()->findEntryByUuid(entryUuid);
            if (!entry || !isEntryExposed(entry) || (entry->group() && isGroupHidden(entry->group()))) {
                return nullptr;
            }
            if (db) {
                *db = database.data();
            }
            return entry;
        }
        return nullptr;
    }

    QList<Entry*> matchingEntries(const QList<QSharedPointer<Database>>& databases, const QStringList& requestHosts)
    {
        QStringList hosts;
        for (const auto& requestHost : requestHosts) {
            const auto host = normalizedHost(requestHost);
            if (!host.isEmpty()) {
                hosts << host;
            }
        }

        QList<Entry*> entries;
        for (const auto& db : databases) {
            for (auto* entry : exposedEntries(db.data())) {
                if (hosts.isEmpty() || std::any_of(hosts.begin(), hosts.end(), [&](const QString& host) {
                        return entryMatchesHost(entry, host);
                    })) {
                    entries << entry;
                }
            }
        }
        return entries;
    }

    QList<Entry*> passkeyEntries(const QList<QSharedPointer<Database>>& databases, const QString& rpId)
    {
        QList<Entry*> entries;
        for (const auto& db : databases) {
            for (auto* entry : exposedEntries(db.data())) {
                if (entry->hasPasskey()
                    && (rpId.isEmpty()
                        || entry->attributes()->value(EntryAttributes::KPEX_PASSKEY_RELYING_PARTY) == rpId)) {
                    entries << entry;
                }
            }
        }
        return entries;
    }

    Entry* findExcludedPasskey(const QList<QSharedPointer<Database>>& databases,
                               const QString& rpId,
                               const QList<QByteArray>& excludedCredentialIds)
    {
        if (excludedCredentialIds.isEmpty()) {
            return nullptr;
        }
        for (auto* entry : passkeyEntries(databases, rpId)) {
            if (excludedCredentialIds.contains(passkeyCredentialId(entry))) {
                return entry;
            }
        }
        return nullptr;
    }

    QByteArray passkeyCredentialId(const Entry* entry)
    {
        return QByteArray::fromBase64(passkeyUtils()->getCredentialIdFromEntry(entry).toUtf8(),
                                      QByteArray::Base64UrlEncoding);
    }

    QByteArray passkeyUserHandle(const Entry* entry)
    {
        return QByteArray::fromBase64(entry->attributes()->value(EntryAttributes::KPEX_PASSKEY_USER_HANDLE).toUtf8(),
                                      QByteArray::Base64UrlEncoding);
    }
} // namespace AutofillCredentials
