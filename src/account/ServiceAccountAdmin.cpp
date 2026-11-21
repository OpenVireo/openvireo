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

#include "ServiceAccountAdmin.h"

#include "AccountManager.h"
#include "VaultKeyring.h"
#include "VaultSharing.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QUrl>

using namespace AccountCrypto;

namespace
{
    using CoreError = AccountManager::CoreError;

    QDateTime timestamp(const rust::String& value)
    {
        const auto text = CoreHost::toQString(value);
        return text.isEmpty() ? QDateTime() : QDateTime::fromString(text, Qt::ISODate);
    }

    rust::String timestampText(const QDateTime& time)
    {
        const auto text = time.isValid() ? time.toUTC().toString(Qt::ISODate).toUtf8() : QByteArray();
        return rust::String(text.constData(), static_cast<size_t>(text.size()));
    }

    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }

    QByteArray toBytes(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
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

    QString orgRoleName(ov::OrgRole role)
    {
        switch (role) {
        case ov::OrgRole::Owner:
            return QStringLiteral("owner");
        case ov::OrgRole::Admin:
            return QStringLiteral("admin");
        default:
            return QStringLiteral("member");
        }
    }

    ov::OrgRole orgRoleOf(const QString& role)
    {
        return role == "owner" ? ov::OrgRole::Owner : role == "admin" ? ov::OrgRole::Admin : ov::OrgRole::Member;
    }

    QString statusName(ov::MemberStatus status)
    {
        switch (status) {
        case ov::MemberStatus::Invited:
            return QStringLiteral("invited");
        case ov::MemberStatus::Accepted:
            return QStringLiteral("accepted");
        default:
            return QStringLiteral("confirmed");
        }
    }

    ov::MemberStatus statusOf(const QString& status)
    {
        return status == "invited"    ? ov::MemberStatus::Invited
               : status == "accepted" ? ov::MemberStatus::Accepted
                                      : ov::MemberStatus::Confirmed;
    }

    QString shareLinksName(ov::ShareLinkPolicy policy)
    {
        switch (policy) {
        case ov::ShareLinkPolicy::Off:
            return QStringLiteral("off");
        case ov::ShareLinkPolicy::Managers:
            return QStringLiteral("managers");
        default:
            return QStringLiteral("on");
        }
    }

    ov::VaultRole vaultRoleOf(const QString& role)
    {
        return role == "manage" ? ov::VaultRole::Manage : role == "write" ? ov::VaultRole::Write : ov::VaultRole::Read;
    }

    PublicKeys publicKeysOf(const ov::PublicKeys& keys)
    {
        PublicKeys out;
        out.encPk = toBytes(keys.enc_pk);
        out.signPk = toBytes(keys.sign_pk);
        out.sig = toBytes(keys.sig);
        return out;
    }

    ov::PublicKeys corePublicKeys(const PublicKeys& keys)
    {
        ov::PublicKeys out;
        out.enc_pk = bytesOf(keys.encPk);
        out.sign_pk = bytesOf(keys.signPk);
        out.sig = bytesOf(keys.sig);
        out.fingerprint = rs(keys.isNull() ? QString() : fingerprint(keys));
        return out;
    }

    AccountPlan::Plan planOf(bool has, const ov::Plan& plan)
    {
        AccountPlan::Plan out;
        if (!has) {
            return out;
        }
        out.id = CoreHost::toQString(plan.id);
        out.hasBilling = plan.has_billing;
        if (plan.has_billing) {
            out.billing.source = CoreHost::toQString(plan.billing.source);
            out.billing.interval = CoreHost::toQString(plan.billing.interval);
            out.billing.status = CoreHost::toQString(plan.billing.status);
            out.billing.manage = CoreHost::toQString(plan.billing.manage);
            out.billing.renewsAt = timestamp(plan.billing.renews_at);
            out.billing.endsAt = timestamp(plan.billing.ends_at);
        }
        return out;
    }

    AccountManager::Org orgOf(const ov::Org& org)
    {
        AccountManager::Org out;
        out.id = CoreHost::toQString(org.id);
        out.name = CoreHost::toQString(org.name);
        out.role = orgRoleName(org.role);
        out.shareLinks = org.has_item_share_links ? shareLinksName(org.item_share_links) : QStringLiteral("on");
        out.plan = planOf(org.has_plan, org.plan);
        return out;
    }

    ServiceAccountAdmin::Member memberOf(const ov::Member& member)
    {
        ServiceAccountAdmin::Member out;
        out.userId = CoreHost::toQString(member.user_id);
        out.email = CoreHost::toQString(member.email);
        out.role = orgRoleName(member.role);
        out.status = statusName(member.status);
        if (member.has_public_keys) {
            out.publicKeys = publicKeysOf(member.public_keys);
            out.verified = verifyPublicKeys(out.publicKeys);
        }
        return out;
    }

    ov::Member coreMember(const ServiceAccountAdmin::Member& member)
    {
        ov::Member out;
        out.user_id = rs(member.userId);
        out.email = rs(member.email);
        out.role = orgRoleOf(member.role);
        out.status = statusOf(member.status);
        out.has_public_keys = !member.publicKeys.isNull();
        out.public_keys = corePublicKeys(member.publicKeys);
        return out;
    }

    ServiceAccountAdmin::ServiceAccount serviceAccountOf(const ov::ServiceAccountInfo& info)
    {
        ServiceAccountAdmin::ServiceAccount account;
        account.id = CoreHost::toQString(info.id);
        account.orgId = CoreHost::toQString(info.org_id);
        account.name = CoreHost::toQString(info.name);
        if (info.has_public_keys) {
            account.publicKeys = publicKeysOf(info.public_keys);
        }
        account.createdBy = CoreHost::toQString(info.created_by);
        account.createdAt = timestamp(info.created_at);
        account.expiresAt = timestamp(info.expires_at);
        account.disabled = info.disabled;
        return account;
    }

    ov::ExpiryChange expiryChange(const QDateTime& expiresAt)
    {
        ov::ExpiryChange change;
        change.kind = expiresAt.isValid() ? ov::ExpiryKind::At : ov::ExpiryKind::Never;
        change.at = timestampText(expiresAt);
        return change;
    }

    QString tokenText(rust::Vec<uint8_t>& token)
    {
        const auto text = QString::fromUtf8(reinterpret_cast<const char*>(token.data()), static_cast<int>(token.size()));
        ov::wipe(token);
        return text;
    }
} // namespace

ServiceAccountAdmin::ServiceAccountAdmin(AccountManager* account, QObject* parent)
    : QObject(parent)
    , m_account(account)
{
}

QString ServiceAccountAdmin::messageFor(const CoreError& error) const
{
    return m_account ? m_account->errorMessage(error) : error.message;
}

void ServiceAccountAdmin::call(AccountManager::AccountWork work, AccountManager::AccountDone done)
{
    if (!m_account) {
        CoreError error;
        error.code = QStringLiteral("signed_out");
        error.message = tr("Not signed in.");
        done(error);
        return;
    }
    m_account->callAccount(this, std::move(work), std::move(done));
}

void ServiceAccountAdmin::simple(AccountManager::AccountWork work, Done done)
{
    QPointer<ServiceAccountAdmin> guard(this);
    call(std::move(work), [guard, done](const CoreError& error) {
        done(error.ok(), error.ok() ? QString() : (guard ? guard->messageFor(error) : error.message));
    });
}

void ServiceAccountAdmin::createOrg(const QString& name, OrgCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, tr("Unlock your account first."));
        return;
    }
    if (name.trimmed().isEmpty()) {
        done({}, tr("Give the organization a name."));
        return;
    }
    auto org = std::make_shared<Org>();
    QPointer<ServiceAccountAdmin> guard(this);
    const auto trimmed = name.trimmed();
    call(
        [trimmed, org](const ov::Account& account, const ov::CancelToken& cancel) {
            *org = orgOf(account.create_org(rs(trimmed), cancel));
        },
        [guard, org, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            if (org->role.isEmpty()) {
                org->role = QStringLiteral("owner");
            }
            if (guard && guard->m_account) {
                guard->m_account->reloadVaults();
            }
            done(*org, {});
        });
}

void ServiceAccountAdmin::loadOrgs(OrgsCallback done)
{
    auto orgs = std::make_shared<QList<Org>>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgs](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& membership : account.orgs(cancel)) {
                // Invited or accepted members hold no keys yet
                if (membership.status != ov::MemberStatus::Confirmed) {
                    continue;
                }
                Org org;
                org.id = CoreHost::toQString(membership.id);
                org.name = CoreHost::toQString(membership.name);
                org.role = orgRoleName(membership.role);
                org.status = statusName(membership.status);
                org.shareLinks = membership.has_item_share_links ? shareLinksName(membership.item_share_links)
                                                                  : QStringLiteral("on");
                org.plan = planOf(membership.has_plan, membership.plan);
                *orgs << org;
            }
        },
        [guard, orgs, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*orgs, {});
        });
}

void ServiceAccountAdmin::listMembers(const QString& orgId, MembersCallback done)
{
    auto members = std::make_shared<QList<Member>>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, members](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& member : account.org_members(rs(orgId), cancel)) {
                *members << memberOf(member);
            }
        },
        [guard, members, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*members, {});
        });
}

QList<QPair<QString, QString>> ServiceAccountAdmin::orgVaults(const QString& orgId) const
{
    QList<QPair<QString, QString>> vaults;
    if (!m_account) {
        return vaults;
    }
    for (const auto& vault : m_account->keyring().vaults()) {
        // Only vaults this account manages can be granted (SPEC §12)
        if (vault.orgId == orgId && vault.role == "manage") {
            vaults << qMakePair(vault.id, vault.name.isEmpty() ? vault.id : vault.name);
        }
    }
    return vaults;
}

void ServiceAccountAdmin::confirmMember(const QString& orgId,
                                        const Member& member,
                                        const QHash<QString, QString>& vaults,
                                        Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    if (!member.verified) {
        done(false,
             tr("The public keys of %1 don't carry a valid signature. Nothing was sealed to them.").arg(member.email));
        return;
    }
    // Owners and admins get every org vault this account manages with manage, and the org's SA master keys (the core
    // does both); a regular member what the admin picked
    QList<QPair<QString, QString>> grants;
    if (!member.isAdmin()) {
        for (const auto& vault : orgVaults(orgId)) {
            const auto role = vaults.value(vault.first);
            if (role.isEmpty()) {
                continue;
            }
            if (role != "read" && role != "write" && role != "manage") {
                done(false, tr("Unknown role: %1").arg(role));
                return;
            }
            grants << qMakePair(vault.first, role);
        }
    }
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, member, grants](const ov::Account& account, const ov::CancelToken& cancel) {
            rust::Vec<ov::VaultGrantRequest> requests;
            for (const auto& grant : grants) {
                ov::VaultGrantRequest request;
                request.vault_id = rs(grant.first);
                request.role = vaultRoleOf(grant.second);
                requests.push_back(std::move(request));
            }
            account.confirm_member(rs(orgId), coreMember(member), std::move(requests), cancel);
        },
        [guard, done](const CoreError& error) {
            if (!guard || !guard->m_account) {
                return;
            }
            if (!error.ok()) {
                done(false, guard->messageFor(error));
                return;
            }
            guard->m_account->reloadVaults();
            done(true, {});
        });
}

QStringList ServiceAccountAdmin::assignableRoles(const QString& role)
{
    if (role == "owner") {
        return {"owner", "admin", "member"};
    }
    if (role == "admin") {
        return {"admin", "member"};
    }
    return {};
}

bool ServiceAccountAdmin::isManagerRole(const QString& role)
{
    return role == "owner" || role == "admin";
}

void ServiceAccountAdmin::setMemberRole(const QString& orgId, const Member& member, const QString& role, Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done(false, tr("Unlock your account first."));
        return;
    }
    if (!QStringList{"owner", "admin", "member"}.contains(role)) {
        done(false, tr("Unknown role: %1").arg(role));
        return;
    }
    // A member who isn't confirmed yet gets their keys when they are confirmed
    const bool promote = isManagerRole(role) && member.status == "confirmed" && !member.publicKeys.isNull();
    if (promote && !member.verified) {
        done(false,
             tr("The public keys of %1 don't carry a valid signature. Nothing was sealed to them.").arg(member.email));
        return;
    }
    // Promoting hands over the SA master keys and manage on every org vault this account manages, in the core
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, member, role](const ov::Account& account, const ov::CancelToken& cancel) {
            account.set_member_role(rs(orgId), coreMember(member), orgRoleOf(role), cancel);
        },
        [guard, done](const CoreError& error) {
            if (!guard || !guard->m_account) {
                return;
            }
            guard->m_account->reloadVaults();
            if (!error.ok()) {
                done(false,
                     error.status == 409 ? tr("An organization needs at least one owner.") : guard->messageFor(error));
                return;
            }
            done(true, {});
        });
}

void ServiceAccountAdmin::removeMember(const QString& orgId, const QString& userId, RemoveCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    // The core removes the member, then rotates the vaults the server flagged that this account manages
    struct Report
    {
        QStringList rotated;
        QList<QPair<QString, QString>> failed;
    };
    auto report = std::make_shared<Report>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, userId, report](const ov::Account& account, const ov::CancelToken& cancel) {
            auto result = account.remove_member(rs(orgId), rs(userId), cancel);
            for (const auto& id : result.rotated) {
                report->rotated << CoreHost::toQString(id);
            }
            for (const auto& failure : result.failed) {
                report->failed << qMakePair(CoreHost::toQString(failure.vault_id),
                                            CoreHost::toQString(failure.error.message));
            }
        },
        [guard, report, done](const CoreError& error) {
            if (!guard || !guard->m_account) {
                return;
            }
            if (!error.ok()) {
                done({},
                     {},
                     error.status == 409 ? tr("An organization needs at least one owner.") : guard->messageFor(error));
                return;
            }
            QStringList failed;
            for (const auto& failure : report->failed) {
                const auto& keyring = guard->m_account->keyring();
                const auto name = keyring.contains(failure.first) ? keyring.vault(failure.first).name : failure.first;
                failed << QStringLiteral("%1: %2").arg(name, failure.second);
            }
            guard->m_account->reloadVaults();
            done(report->rotated, failed, {});
        });
}

void ServiceAccountAdmin::listInvitations(const QString& orgId, InvitationsCallback done)
{
    auto invitations = std::make_shared<QList<Invitation>>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, invitations](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& row : account.invitations(rs(orgId), cancel)) {
                Invitation invitation;
                invitation.id = CoreHost::toQString(row.id);
                invitation.email = CoreHost::toQString(row.email);
                invitation.role = orgRoleName(row.role);
                invitation.expiresAt = timestamp(row.expires_at);
                invitation.invitedBy = CoreHost::toQString(row.invited_by);
                *invitations << invitation;
            }
        },
        [guard, invitations, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*invitations, {});
        });
}

void ServiceAccountAdmin::invite(const QString& orgId, const QString& email, const QString& role, Done done)
{
    if (!email.contains('@')) {
        done(false, tr("Enter the email address to send the invitation to."));
        return;
    }
    if (!QStringList{"owner", "admin", "member"}.contains(role)) {
        done(false, tr("Unknown role: %1").arg(role));
        return;
    }
    const auto trimmed = email.trimmed();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, trimmed, role](const ov::Account& account, const ov::CancelToken& cancel) {
            account.invite(rs(orgId), rs(trimmed), orgRoleOf(role), cancel);
        },
        [guard, done](const CoreError& error) {
            if (error.status == 409) {
                done(false, tr("That account is already a member of the organization."));
                return;
            }
            done(error.ok(), error.ok() ? QString() : (guard ? guard->messageFor(error) : error.message));
        });
}

void ServiceAccountAdmin::cancelInvitation(const QString& orgId, const QString& invitationId, Done done)
{
    simple(
        [orgId, invitationId](const ov::Account& account, const ov::CancelToken& cancel) {
            account.cancel_invitation(rs(orgId), rs(invitationId), cancel);
        },
        done);
}

QString ServiceAccountAdmin::invitationToken(const QString& linkOrToken)
{
    auto text = linkOrToken.trimmed();
    // The link carries the token in its fragment: OPENVIREO_INVITATION_URL#token=ov_inv_… (SPEC §12)
    const auto marker = text.indexOf(QStringLiteral("token="));
    if (marker >= 0) {
        text = text.mid(marker + 6);
        text = text.left(text.indexOf('&') < 0 ? text.size() : text.indexOf('&'));
        text = QUrl::fromPercentEncoding(text.toUtf8()).trimmed();
    }
    static const QRegularExpression pattern(QStringLiteral("^ov_inv_[A-Za-z0-9_-]+={0,2}$"));
    return pattern.match(text).hasMatch() ? text : QString();
}

void ServiceAccountAdmin::acceptInvitation(const QString& linkOrToken, Done done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done(false, tr("Unlock your account first."));
        return;
    }
    const auto token = invitationToken(linkOrToken);
    if (token.isEmpty()) {
        done(false, tr("This isn't an invitation link or code. Paste the link from the invitation email."));
        return;
    }
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [token](const ov::Account& account, const ov::CancelToken& cancel) {
            account.accept_invitation(bytesOf(token.toUtf8()), cancel);
        },
        [guard, done](const CoreError& error) {
            if (error.status == 410) {
                done(false, tr("The invitation was already used or has expired. Ask for a new one."));
                return;
            }
            if (error.status == 403) {
                done(false, tr("The invitation is for another email address. Sign in with that account to accept it."));
                return;
            }
            if (error.status == 409) {
                done(false, tr("You are already a member of this organization."));
                return;
            }
            if (!error.ok()) {
                done(false, guard ? guard->messageFor(error) : error.message);
                return;
            }
            if (guard && guard->m_account) {
                guard->m_account->reloadVaults();
            }
            done(true, {});
        });
}

void ServiceAccountAdmin::list(const QString& orgId, ListCallback done)
{
    auto accounts = std::make_shared<QList<ServiceAccount>>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, accounts](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& info : account.service_accounts(rs(orgId), cancel)) {
                *accounts << serviceAccountOf(info);
            }
        },
        [guard, accounts, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*accounts, {});
        });
}

void ServiceAccountAdmin::create(const QString& orgId,
                                 const QString& name,
                                 const QDateTime& expiresAt,
                                 TokenCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked || m_account->isServiceAccount()) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    if (name.trimmed().isEmpty()) {
        done({}, {}, tr("Give the service account a name."));
        return;
    }
    // The core makes the keyset and master key, seals the master key to every owner and admin and issues the first
    // secret, whose token is shown once
    struct Created
    {
        ServiceAccount account;
        QString token;
    };
    auto created = std::make_shared<Created>();
    const auto trimmed = name.trimmed();
    const auto secretName = tr("First secret");
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, trimmed, expiresAt, secretName, created](const ov::Account& account, const ov::CancelToken& cancel) {
            ov::NewServiceAccount input;
            input.name = rs(trimmed);
            input.expires_at = timestampText(expiresAt);
            input.secret_name = rs(secretName);
            auto result = account.create_service_account(rs(orgId), input, cancel);
            created->account = serviceAccountOf(result.service_account);
            created->token = tokenText(result.token);
        },
        [guard, created, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, {}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(created->account, created->token, {});
        });
}

void ServiceAccountAdmin::setExpiry(const QString& saId, const QDateTime& expiresAt, Done done)
{
    simple(
        [saId, expiresAt](const ov::Account& account, const ov::CancelToken& cancel) {
            ov::ServiceAccountUpdate update;
            update.expires_at = expiryChange(expiresAt);
            account.update_service_account(rs(saId), update, cancel);
        },
        done);
}

void ServiceAccountAdmin::setDisabled(const QString& saId, bool disabled, Done done)
{
    simple(
        [saId, disabled](const ov::Account& account, const ov::CancelToken& cancel) {
            ov::ServiceAccountUpdate update;
            update.expires_at.kind = ov::ExpiryKind::Keep;
            update.has_disabled = true;
            update.disabled = disabled;
            account.update_service_account(rs(saId), update, cancel);
        },
        done);
}

void ServiceAccountAdmin::remove(const QString& saId, Done done)
{
    simple([saId](const ov::Account& account,
                  const ov::CancelToken& cancel) { account.delete_service_account(rs(saId), cancel); },
           done);
}

void ServiceAccountAdmin::listSecrets(const QString& saId, SecretsCallback done)
{
    auto secrets = std::make_shared<QList<Secret>>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [saId, secrets](const ov::Account& account, const ov::CancelToken& cancel) {
            for (const auto& row : account.service_account_secrets(rs(saId), cancel)) {
                Secret secret;
                secret.id = CoreHost::toQString(row.id);
                secret.name = CoreHost::toQString(row.name);
                secret.prefix = CoreHost::toQString(row.prefix);
                secret.createdAt = timestamp(row.created_at);
                secret.lastUsedAt = timestamp(row.last_used_at);
                secret.expiresAt = timestamp(row.expires_at);
                *secrets << secret;
            }
        },
        [guard, secrets, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*secrets, {});
        });
}

void ServiceAccountAdmin::issueSecret(const QString& saId,
                                      const QString& name,
                                      const QDateTime& expiresAt,
                                      TokenCallback done)
{
    if (!m_account || m_account->state() != AccountManager::State::Unlocked) {
        done({}, {}, tr("Unlock your account first."));
        return;
    }
    auto token = std::make_shared<QString>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [saId, name, expiresAt, token](const ov::Account& account, const ov::CancelToken& cancel) {
            auto issued = account.issue_service_account_secret(rs(saId), rs(name), timestampText(expiresAt), cancel);
            *token = tokenText(issued.token);
        },
        [guard, saId, token, done](const CoreError& error) {
            if (!error.ok()) {
                done({},
                     {},
                     error.code == "decryption_failed" || error.code == "no_vault_key"
                         ? tr("You don't hold this service account's master key. Ask another owner or admin.")
                         : (guard ? guard->messageFor(error) : error.message));
                return;
            }
            ServiceAccount account;
            account.id = saId;
            done(account, *token, {});
        });
}

void ServiceAccountAdmin::setSecretExpiry(const QString& saId,
                                          const QString& secretId,
                                          const QDateTime& expiresAt,
                                          Done done)
{
    simple(
        [saId, secretId, expiresAt](const ov::Account& account, const ov::CancelToken& cancel) {
            account.set_secret_expiry(rs(saId), rs(secretId), expiryChange(expiresAt), cancel);
        },
        done);
}

void ServiceAccountAdmin::revokeSecret(const QString& saId, const QString& secretId, Done done)
{
    simple(
        [saId, secretId](const ov::Account& account, const ov::CancelToken& cancel) {
            account.revoke_secret(rs(saId), rs(secretId), cancel);
        },
        done);
}

void ServiceAccountAdmin::grantVault(const ServiceAccount& account,
                                     const QString& vaultId,
                                     const QString& role,
                                     Done done)
{
    if (role != "read" && role != "write") {
        done(false, tr("Service accounts can read or write a vault, but not manage it."));
        return;
    }
    VaultSharing::Recipient recipient;
    recipient.principal = "sa:" + account.id;
    recipient.name = account.name;
    recipient.publicKeys = account.publicKeys;
    recipient.verified = !account.publicKeys.isNull() && verifyPublicKeys(account.publicKeys);
    auto* sharing = new VaultSharing(m_account, this);
    sharing->grant(vaultId, recipient, role, [sharing, done](bool ok, const QString& error) {
        sharing->deleteLater();
        done(ok, error);
    });
}

void ServiceAccountAdmin::vaultAccess(const ServiceAccount& account, AccessCallback done)
{
    if (!m_account) {
        done({}, tr("Not signed in."));
        return;
    }
    // Only managers can list who has access, so only the vaults this account manages are asked
    QStringList vaultIds;
    for (const auto& vault : m_account->keyring().vaults()) {
        if (vault.role == "manage") {
            vaultIds << vault.id;
        }
    }
    if (vaultIds.isEmpty()) {
        done({}, {});
        return;
    }
    struct Found
    {
        QHash<QString, QString> roles;
        QString error;
    };
    auto found = std::make_shared<Found>();
    const auto principal = "sa:" + account.id;
    call(
        [vaultIds, principal, found](const ov::Account& core, const ov::CancelToken& cancel) {
            for (const auto& vaultId : vaultIds) {
                try {
                    for (const auto& member : core.members(rs(vaultId), cancel)) {
                        if (CoreHost::toQString(member.principal) == principal) {
                            found->roles.insert(vaultId,
                                                member.role == ov::VaultRole::Write ? QStringLiteral("write")
                                                : member.role == ov::VaultRole::Manage ? QStringLiteral("manage")
                                                                                         : QStringLiteral("read"));
                        }
                    }
                } catch (const std::exception& e) {
                    found->error = QString::fromUtf8(e.what());
                }
            }
        },
        [found, done](const CoreError& error) {
            done(found->roles, error.ok() ? found->error : error.message);
        });
}

void ServiceAccountAdmin::revokeVault(const ServiceAccount& account, const QString& vaultId, Done done)
{
    auto* sharing = new VaultSharing(m_account, this);
    sharing->revoke(vaultId, {"sa:" + account.id}, [sharing, done](bool ok, const QString& error) {
        sharing->deleteLater();
        done(ok, error);
    });
}

void ServiceAccountAdmin::setShareLinks(const QString& orgId, const QString& policy, OrgCallback done)
{
    auto org = std::make_shared<Org>();
    const auto value = policy == "off" ? ov::ShareLinkPolicy::Off
                       : policy == "managers" ? ov::ShareLinkPolicy::Managers
                                              : ov::ShareLinkPolicy::On;
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, value, org](const ov::Account& account, const ov::CancelToken& cancel) {
            *org = orgOf(account.set_org_share_links(rs(orgId), value, cancel));
        },
        [guard, org, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(*org, {});
        });
}

void ServiceAccountAdmin::auditLog(const QString& orgId, const QString& before, AuditCallback done)
{
    struct Page
    {
        QList<AuditEvent> events;
        QString next;
    };
    auto page = std::make_shared<Page>();
    QPointer<ServiceAccountAdmin> guard(this);
    call(
        [orgId, before, page](const ov::Account& account, const ov::CancelToken& cancel) {
            auto result = account.audit_log(rs(orgId), rs(before), true, AuditPageSize, cancel);
            for (const auto& row : result.events) {
                AuditEvent event;
                event.id = CoreHost::toQString(row.id);
                event.at = timestamp(row.at);
                event.actor = CoreHost::toQString(row.actor);
                event.action = CoreHost::toQString(row.action);
                event.target = CoreHost::toQString(row.target);
                event.ip = CoreHost::toQString(row.ip);
                event.details = QJsonDocument::fromJson(CoreHost::toQString(row.details).toUtf8()).object();
                page->events << event;
            }
            page->next = CoreHost::toQString(result.next);
        },
        [guard, page, done](const CoreError& error) {
            if (!error.ok()) {
                done({}, {}, guard ? guard->messageFor(error) : error.message);
                return;
            }
            done(page->events, page->next, {});
        });
}
