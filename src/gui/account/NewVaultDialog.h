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

#ifndef OPENVIREO_NEWVAULTDIALOG_H
#define OPENVIREO_NEWVAULTDIALOG_H

#include <QDialog>

#include <functional>

#include "account/AccountManager.h"

class QButtonGroup;
class QLabel;
class QPushButton;
class VaultLineEdit;
class VaultPill;
class NewVaultOwnerRow;

// Account › New Vault: the vault's name and who it belongs to, the account itself or an org the user owns or
// administers. Over the main window it is a card (VaultModal). The owners are rows to pick one of, and a note under
// them says who can open the vault.
class NewVaultDialog : public QDialog
{
    Q_OBJECT

public:
    // `orgs` are the orgs the user may make a vault in; with none, the dialog only asks for a name
    NewVaultDialog(AccountManager* account, const QList<AccountManager::Org>& orgs, QWidget* parent = nullptr);

    // Opens the dialog as a card over the parent's window, or as a window when it can't be one. `created` gets the
    // name and the org id (empty for a personal vault) once the user presses Create.
    static NewVaultDialog* present(AccountManager* account,
                                   const QList<AccountManager::Org>& orgs,
                                   QWidget* parent,
                                   std::function<void(const QString& name, const QString& orgId)> created);

    QString name() const;
    // Empty for a personal vault
    QString orgId() const;
    void setOrgId(const QString& orgId);

public slots:
    void accept() override;

private:
    void refresh();
    void applyColors();

    QList<AccountManager::Org> m_orgs;
    // Lower-cased names of the vaults each owner has already, by org id ("" = personal)
    QHash<QString, QStringList> m_vaultNames;
    QLabel* m_intro;
    QLabel* m_nameCaption;
    VaultLineEdit* m_nameEdit;
    QLabel* m_nameNote;
    QLabel* m_ownerCaption;
    QWidget* m_ownerPanel;
    QButtonGroup* m_ownerGroup;
    QList<NewVaultOwnerRow*> m_ownerRows;
    QWidget* m_whoPanel;
    QLabel* m_whoIcon;
    QLabel* m_whoTitle;
    QLabel* m_whoBody;
    QWidget* m_buttonRow;
    QLabel* m_billedTo;
    QPushButton* m_cancelButton;
    QPushButton* m_createButton;
};

#endif // OPENVIREO_NEWVAULTDIALOG_H
