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

#ifndef KEEPASSXC_ITEMSHARE_H
#define KEEPASSXC_ITEMSHARE_H

#include "AccountCrypto.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <functional>

class AccountManager;
class Entry;

/**
 * Share links (SPEC §22): a copy of an entry's chosen fields, or some text, that anyone with the link can
 * open in a browser without an account. The content is encrypted here with a fresh key that only the link
 * holds (after the #); the account server keeps the ciphertext and counts opens.
 */
class ItemShare : public QObject
{
    Q_OBJECT

public:
    using SecureBytes = AccountCrypto::SecureBytes;

    // A field of an entry that can be shared. `name` is the entry's attribute name, or "otp" for its TOTP
    struct Field
    {
        QString name;
        // username, password, url, notes, totp or custom
        QString kind;
        QString label;
        QString value;
        bool isProtected = false;
    };

    struct Options
    {
        // Seconds, one hour to 30 days (and the plan's share_link_days)
        int expiresIn = 7 * 24 * 3600;
        // 0: as often as people like until it expires
        int maxOpens = 0;
        // The recipient types it as well; empty for none
        QString passcode;
        // The vault and entry it came from; empty for text
        QString vaultId;
        QString entryId;
    };

    struct Created
    {
        QString id;
        QString url;
        QDateTime expiresAt;
    };

    // A link in its owner's list
    struct Listing
    {
        QString id;
        QString vaultId;
        // From the owner reference; empty when it can't be read
        QString title;
        QString entryId;
        bool passcode = false;
        int maxOpens = 0;
        int opens = 0;
        QList<QDateTime> openedAt;
        QDateTime createdAt;
        QDateTime expiresAt;
        // pending, active or ended
        QString status;
        // expired, opened, revoked, passcode, access_lost or policy
        QString endReason;

        bool isEnded() const
        {
            return status == "ended";
        }
        static Listing fromJson(const QJsonObject& json, const SecureBytes& encSk);
    };

    using CreatedCallback = std::function<void(const Created& share, const QString& error)>;
    using ListCallback = std::function<void(const QList<Listing>& shares, const QString& error)>;
    using Done = std::function<void(bool ok, const QString& error)>;

    // The fields of `entry` that can be shared, in display order (standard ones, then TOTP, then custom)
    static QList<Field> shareableFields(const Entry* entry);
    // Field names shared unless the user chooses otherwise
    static QStringList defaultFields();
    // The payload JSON for the chosen fields of an entry, and for some text
    static QJsonObject entryPayload(const Entry* entry, const QStringList& names);
    static QJsonObject textPayload(const QString& text, const QString& title = {});
    // Why this account can't make links from `vaultId` (its org's policy), or empty when it can
    QString blockedReason(const QString& vaultId) const;

    explicit ItemShare(AccountManager* account, QObject* parent = nullptr);

    void create(const QJsonObject& payload, const Options& options, CreatedCallback done);
    void list(ListCallback done);
    void revoke(const QString& shareId, Done done);

private:
    QPointer<AccountManager> m_account;
};

#endif // KEEPASSXC_ITEMSHARE_H
