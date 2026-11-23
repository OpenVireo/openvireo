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

#include "AccountVaults.h"

#include "account/AccountManager.h"

#include <QMap>

namespace
{
    QString roleText(const QString& role)
    {
        if (role == "manage") {
            return QObject::tr("Can manage");
        }
        if (role == "write") {
            return QObject::tr("Can edit");
        }
        return QObject::tr("Read only");
    }
} // namespace

QList<AccountVaults::Section> AccountVaults::sections(AccountManager* account)
{
    auto vaults = account->keyring().vaults();
    std::sort(vaults.begin(), vaults.end(), [](const VaultKeyring::Vault& a, const VaultKeyring::Vault& b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    QMap<QString, QList<VaultKeyring::Vault>> byOrg;
    for (const auto& vault : vaults) {
        byOrg[vault.orgId] << vault;
    }
    QList<AccountManager::Org> orgs;
    for (const auto& org : account->orgs()) {
        if (byOrg.contains(org.id) || !org.isConfirmed()) {
            orgs << org;
        }
    }
    for (const auto& orgId : byOrg.keys()) {
        if (!orgId.isEmpty() && account->org(orgId).id.isEmpty()) {
            // Not in /me (yet): still listed, under a generic heading
            AccountManager::Org unknown;
            unknown.id = orgId;
            orgs << unknown;
        }
    }
    std::sort(orgs.begin(), orgs.end(), [](const AccountManager::Org& a, const AccountManager::Org& b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });

    QList<Section> result;
    if (byOrg.contains({})) {
        // Without any org the list stays as plain as it always was
        result << Section{{}, orgs.isEmpty() ? QString() : tr("Personal"), {}, byOrg.value({})};
    }
    for (const auto& org : orgs) {
        const auto name = org.name.isEmpty() ? tr("Organization") : org.name;
        if (!org.isConfirmed()) {
            result << Section{
                org.id,
                tr("%1 (waiting for confirmation)").arg(name),
                tr("An owner or admin of %1 has to confirm you before you can open its vaults.").arg(name),
                {}};
            continue;
        }
        result << Section{org.id, name, {}, byOrg.value(org.id)};
    }
    return result;
}

QString AccountVaults::vaultName(const VaultKeyring::Vault& vault)
{
    return vault.name.isEmpty() ? vault.id : vault.name;
}

QString AccountVaults::vaultToolTip(const VaultKeyring::Vault& vault)
{
    auto tip = roleText(vault.role);
    if (vault.rotationRequired) {
        tip += "\n" + tr("A member was removed: rotate the key from the Share dialog.");
    }
    return tip;
}
