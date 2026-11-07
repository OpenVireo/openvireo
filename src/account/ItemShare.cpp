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

#include "ItemShare.h"

#include "AccountManager.h"
#include "VaultKeyring.h"

#include "core/Entry.h"
#include "core/Totp.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace AccountCrypto;

namespace
{
    // KeePass attributes that hold TOTP settings; shared together as one "otp" field
    const QStringList TotpAttributes{"otp", "TOTP Seed", "TOTP Settings"};

    QDateTime parseTime(const QJsonValue& value)
    {
        return QDateTime::fromString(value.toString(), Qt::ISODate);
    }
} // namespace

ItemShare::ItemShare(AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_account(account)
{
}

QStringList ItemShare::defaultFields()
{
    return {EntryAttributes::UserNameKey, EntryAttributes::PasswordKey, EntryAttributes::URLKey};
}

QList<ItemShare::Field> ItemShare::shareableFields(const Entry* entry)
{
    QList<Field> fields;
    if (!entry) {
        return fields;
    }
    const auto add = [&](const QString& name, const QString& kind, const QString& label, const QString& value, bool prot) {
        if (!value.isEmpty()) {
            fields << Field{name, kind, label, value, prot};
        }
    };
    add(EntryAttributes::UserNameKey, "username", tr("Username"), entry->username(), false);
    add(EntryAttributes::PasswordKey, "password", tr("Password"), entry->password(), true);
    add(EntryAttributes::URLKey, "url", tr("Website"), entry->url(), false);
    add(EntryAttributes::NotesKey, "notes", tr("Notes"), entry->notes(), false);
    if (entry->hasTotp()) {
        add("otp", "totp", tr("One-time code"), Totp::writeSettings(entry->totpSettings(), entry->title(), entry->username(), true), true);
    }
    const auto* attributes = entry->attributes();
    for (const auto& key : attributes->customKeys()) {
        if (!TotpAttributes.contains(key)) {
            add(key, "custom", key, attributes->value(key), attributes->isProtected(key));
        }
    }
    return fields;
}

QJsonObject ItemShare::entryPayload(const Entry* entry, const QStringList& names)
{
    QJsonArray fields;
    for (const auto& field : shareableFields(entry)) {
        if (!names.contains(field.name)) {
            continue;
        }
        QJsonObject json{{"k", field.kind}, {"label", field.label}, {"value", field.value}};
        if (field.isProtected) {
            json.insert("protected", true);
        }
        fields.append(json);
    }
    return {{"v", 1}, {"kind", "entry"}, {"title", entry ? entry->title() : QString()}, {"fields", fields}};
}

QJsonObject ItemShare::textPayload(const QString& text, const QString& title)
{
    QJsonObject payload{{"v", 1},
                        {"kind", "text"},
                        {"fields", QJsonArray{QJsonObject{{"k", "text"}, {"label", tr("Text")}, {"value", text}}}}};
    if (!title.isEmpty()) {
        payload.insert("title", title);
    }
    return payload;
}

QString ItemShare::blockedReason(const QString& vaultId) const
{
    if (!m_account || vaultId.isEmpty()) {
        return {};
    }
    const auto orgId = m_account->keyring().vault(vaultId).orgId;
    if (orgId.isEmpty()) {
        return {};
    }
    const auto org = m_account->org(orgId);
    if (org.canShareLinks()) {
        return {};
    }
    return org.shareLinks == "off" ? tr("Your organization has turned share links off.")
                                   : tr("In your organization only owners and admins can make share links.");
}

void ItemShare::create(const QJsonObject& payload, const Options& options, CreatedCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, tr("Unlock your account first."));
        return;
    }
    const auto base = m_account->server().shareBase();
    if (base.isEmpty()) {
        done({}, tr("This account server has no share page."));
        return;
    }

    QJsonObject body{{"expires_in", options.expiresIn}};
    body.insert("vault_id", options.vaultId.isEmpty() ? QJsonValue() : QJsonValue(options.vaultId));
    body.insert("max_opens", options.maxOpens > 0 ? QJsonValue(options.maxOpens) : QJsonValue());
    SecureBytes passcodeKey;
    if (!options.passcode.isEmpty()) {
        const auto kdf = KdfParams::generate();
        passcodeKey = sharePasscodeKey(options.passcode, kdf);
        body.insert("kdf", kdf.toJson());
        body.insert("auth", toBase64Url(shareAuth(passcodeKey)));
    }
    const auto payloadJson = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const auto refJson = QJsonDocument(QJsonObject{{"vault", options.vaultId.isEmpty() ? QJsonValue() : QJsonValue(options.vaultId)},
                                                   {"entry", options.entryId.isEmpty() ? QJsonValue() : QJsonValue(options.entryId)},
                                                   {"title", payload.value("title").toString()}})
                             .toJson(QJsonDocument::Compact);

    QPointer<ItemShare> guard(this);
    m_account->client()->createShare(body, [guard, base, payloadJson, refJson, passcodeKey, done](const AuthClient::Response& response) {
        if (!guard || !guard->m_account) {
            return;
        }
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        const auto json = response.json();
        const auto id = json.value("id").toString();
        const auto linkKey = randomSecure(KeySize);
        const auto ct = encryptSharePayload(linkKey, id, payloadJson, passcodeKey);
        if (ct.isEmpty()) {
            done({}, tr("This is too much to share in one link. Choose fewer fields."));
            return;
        }
        const auto ref = toBase64Url(encryptShareRef(guard->m_account->keyset().encSk, id, refJson));
        const Created created{id, shareLink(base, id, linkKey), parseTime(json.value("expires_at"))};
        guard->m_account->client()->uploadShare(id, ct, ref, [created, done](const AuthClient::Response& upload) {
            if (!upload.isSuccess()) {
                done({}, AuthClient::errorMessage(upload));
                return;
            }
            done(created, {});
        });
    });
}

ItemShare::Listing ItemShare::Listing::fromJson(const QJsonObject& json, const SecureBytes& encSk)
{
    Listing listing;
    listing.id = json.value("id").toString();
    listing.vaultId = json.value("vault_id").toString();
    listing.passcode = json.value("passcode").toBool();
    listing.maxOpens = json.value("max_opens").toInt();
    listing.opens = json.value("opens").toInt();
    for (const auto& value : json.value("opened_at").toArray()) {
        listing.openedAt << parseTime(value);
    }
    listing.createdAt = parseTime(json.value("created_at"));
    listing.expiresAt = parseTime(json.value("expires_at"));
    listing.status = json.value("status").toString();
    listing.endReason = json.value("end_reason").toString();
    bool ok = false;
    const auto ref = fromBase64Url(json.value("ref").toString(), &ok);
    QByteArray refJson;
    if (ok && !encSk.empty() && decryptShareRef(encSk, listing.id, ref, &refJson)) {
        const auto refObject = QJsonDocument::fromJson(refJson).object();
        listing.title = refObject.value("title").toString();
        listing.entryId = refObject.value("entry").toString();
    }
    return listing;
}

void ItemShare::list(ListCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, tr("Unlock your account first."));
        return;
    }
    QPointer<ItemShare> guard(this);
    m_account->client()->listShares([guard, done](const AuthClient::Response& response) {
        if (!guard || !guard->m_account) {
            return;
        }
        if (!response.isSuccess()) {
            done({}, AuthClient::errorMessage(response));
            return;
        }
        QList<Listing> shares;
        for (const auto& value : response.json().value("shares").toArray()) {
            shares << Listing::fromJson(value.toObject(), guard->m_account->keyset().encSk);
        }
        done(shares, {});
    });
}

void ItemShare::revoke(const QString& shareId, Done done)
{
    if (!m_account) {
        done(false, tr("Unlock your account first."));
        return;
    }
    m_account->client()->revokeShare(shareId, [done](const AuthClient::Response& response) {
        done(response.isSuccess(), response.isSuccess() ? QString() : AuthClient::errorMessage(response));
    });
}
