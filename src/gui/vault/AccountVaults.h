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

#ifndef OPENVIREO_ACCOUNTVAULTS_H
#define OPENVIREO_ACCOUNTVAULTS_H

#include "account/VaultKeyring.h"

#include <QCoreApplication>

class AccountManager;

/**
 * The signed-in account's vaults as the database picker lists them: personal vaults first, then each org's under
 * the org's name.
 */
class AccountVaults
{
    Q_DECLARE_TR_FUNCTIONS(AccountVaults)

public:
    struct Section
    {
        QString orgId;
        // Empty for the personal vaults of an account without orgs, which need no heading
        QString title;
        QString toolTip;
        // Vaults of an org the user isn't confirmed in yet can't be opened, so none are listed
        QList<VaultKeyring::Vault> vaults;
    };

    // Personal vaults first, then one section per org in the order of the org names
    static QList<Section> sections(AccountManager* account);
    static QString vaultName(const VaultKeyring::Vault& vault);
    static QString vaultToolTip(const VaultKeyring::Vault& vault);
};

#endif // OPENVIREO_ACCOUNTVAULTS_H
