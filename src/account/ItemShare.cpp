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

    QDateTime parseTime(const QString& value)
    {
        return QDateTime::fromString(value, Qt::ISODate);
    }

    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    rust::Vec<uint8_t> bytesOf(const QByteArray& data)
    {
        rust::Vec<uint8_t> out;
        out.reserve(static_cast<size_t>(data.size()));
        for (auto c : data) {
            out.push_back(static_cast<uint8_t>(c));
        }
        return out;
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
    if (m_account->server().shareBase().isEmpty()) {
        done({}, tr("This account server has no share page."));
        return;
    }

    // openvireo-core registers the link, encrypts the payload under a fresh link key (and the passcode), keeps the
    // owner reference sealed to this account and uploads it (SPEC §22)
    ov::SharePayload content;
    content.v = static_cast<uint32_t>(payload.value("v").toInt(1));
    content.kind = rs(payload.value("kind").toString());
    content.title = rs(payload.value("title").toString());
    content.shared_by = rs(payload.value("shared_by").toString());
    for (const auto& value : payload.value("fields").toArray()) {
        const auto json = value.toObject();
        ov::ShareField field;
        field.k = rs(json.value("k").toString());
        field.label = rs(json.value("label").toString());
        field.value = bytesOf(json.value("value").toString().toUtf8());
        field.is_protected = json.value("protected").toBool();
        content.fields.push_back(std::move(field));
    }
    ov::ShareOptions opts;
    opts.expires_in = static_cast<uint32_t>(qMax(0, options.expiresIn));
    opts.has_max_opens = options.maxOpens > 0;
    opts.max_opens = static_cast<uint32_t>(qMax(0, options.maxOpens));
    opts.has_passcode = !options.passcode.isEmpty();
    opts.passcode = bytesOf(options.passcode.toUtf8());
    opts.vault_id = rs(options.vaultId);
    opts.entry_id = rs(options.entryId);

    auto created = std::make_shared<Created>();
    auto input = std::make_shared<std::pair<ov::SharePayload, ov::ShareOptions>>(std::move(content), std::move(opts));
    QPointer<ItemShare> guard(this);
    m_account->callAccount(
        this,
        [input, created](const ov::Account& account, const ov::CancelToken& cancel) {
            auto share = account.create_share(std::move(input->first), std::move(input->second), cancel);
            created->id = CoreHost::toQString(share.id);
            created->url = QString::fromUtf8(reinterpret_cast<const char*>(share.url.data()),
                                             static_cast<int>(share.url.size()));
            ov::wipe(share.url);
            created->expiresAt = parseTime(CoreHost::toQString(share.expires_at));
        },
        [guard, input, created, done](const AccountManager::CoreError& error) {
            // The payload is wiped with the request
            ov::wipe(input->second.passcode);
            if (!error.ok()) {
                done({}, guard && guard->m_account ? guard->m_account->errorMessage(error) : error.message);
                return;
            }
            done(*created, {});
        });
}

ItemShare::Listing ItemShare::Listing::fromCore(const ov::ShareListing& share)
{
    Listing listing;
    listing.id = CoreHost::toQString(share.id);
    listing.vaultId = CoreHost::toQString(share.vault_id);
    listing.passcode = share.passcode;
    listing.maxOpens = share.has_max_opens ? static_cast<int>(share.max_opens) : 0;
    listing.opens = static_cast<int>(share.opens);
    for (const auto& when : share.opened_at) {
        listing.openedAt << parseTime(CoreHost::toQString(when));
    }
    listing.createdAt = parseTime(CoreHost::toQString(share.created_at));
    listing.expiresAt = parseTime(CoreHost::toQString(share.expires_at));
    listing.status = CoreHost::toQString(share.status);
    listing.endReason = CoreHost::toQString(share.end_reason);
    if (share.has_reference) {
        listing.title = CoreHost::toQString(share.reference.title);
        listing.entryId = CoreHost::toQString(share.reference.entry);
    }
    return listing;
}

void ItemShare::list(ListCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, tr("Unlock your account first."));
        return;
    }
    auto shares = std::make_shared<QList<Listing>>();
    QPointer<ItemShare> guard(this);
    m_account->callAccount(
        this,
        [shares](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& share : account.list_shares(cancel)) {
                *shares << Listing::fromCore(share);
            }
        },
        [guard, shares, done](const AccountManager::CoreError& error) {
            if (!error.ok()) {
                done({}, guard && guard->m_account ? guard->m_account->errorMessage(error) : error.message);
                return;
            }
            done(*shares, {});
        });
}

void ItemShare::revoke(const QString& shareId, Done done)
{
    if (!m_account) {
        done(false, tr("Unlock your account first."));
        return;
    }
    QPointer<ItemShare> guard(this);
    m_account->callAccount(
        this,
        [shareId](const ov::Account& account, const ov::CancelToken& cancel) {
            account.revoke_share(rs(shareId), cancel);
        },
        [guard, done](const AccountManager::CoreError& error) {
            done(error.ok(),
                 error.ok() ? QString()
                            : (guard && guard->m_account ? guard->m_account->errorMessage(error) : error.message));
        });
}
