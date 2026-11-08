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

#ifndef KEEPASSXC_SERVICEACCOUNTSDIALOG_H
#define KEEPASSXC_SERVICEACCOUNTSDIALOG_H

#include "account/ServiceAccountAdmin.h"

#include <QDialog>
#include <QPointer>

class AccountManager;
class QButtonGroup;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class VaultButton;
class VaultComboBox;
class VaultLineEdit;

namespace fluent::basicinput
{
    class ToggleSwitch;
}
namespace fluent::status_info
{
    class InfoBar;
    class ProgressBar;
} // namespace fluent::status_info

/**
 * The Organization window, for owners and admins: creating an org, its members (confirm, change roles,
 * remove), invitations, service accounts (create, disable, expire, delete) with their secrets (issue,
 * revoke) and vault access (grant, remove), who can make share links, and the org's audit log.
 *
 * A sidebar picks the org and one of its sections. In the Vault Index layout the window sits in a card
 * over the main window (OrgAdminModal) and its prompts are Fluent content dialogs.
 *
 * A new service account's keys and every secret are made on this device. The token `ov_sa_…` is shown
 * once, in a window of its own, and isn't kept anywhere.
 */
class ServiceAccountsDialog : public QDialog
{
    Q_OBJECT

public:
    // The sidebar's sections, in order
    enum Section
    {
        Members,
        Invitations,
        ServiceAccounts,
        ShareLinks,
        AuditLog,
    };
    Q_ENUM(Section)

    explicit ServiceAccountsDialog(AccountManager* account, QWidget* parent = nullptr);
    ~ServiceAccountsDialog() override;

    // Shown inside another window's card, which brings its own title and buttons
    void setEmbedded(bool embedded);

    QString currentOrg() const;
    // This account's role in the current org
    QString currentRole() const;
    QString currentServiceAccount() const;
    Section currentSection() const;
    bool isBusy() const;
    // The window that shows the last token, while it is open
    QDialog* tokenDialog() const;
    QListWidget* navigation() const;
    // Members who accepted an invitation and wait for an owner or admin to confirm them
    int waitingCount() const;
    // Vault id -> role of the selected service account, for the vaults this account manages
    QHash<QString, QString> vaultAccess() const;
    // Who can make share links in the current org: on, managers or off
    QString shareLinkPolicy() const;
    // The audit log's action, such as org.member.confirm, in words
    static QString auditActionName(const QString& action);

public slots:
    void reload();
    void setCurrentSection(Section section);
    void createServiceAccount(const QString& name, const QDateTime& expiresAt = {});
    void issueSecret(const QString& name, const QDateTime& expiresAt = {});
    void revokeSelectedSecret();
    // When the selected secret stops working (invalid: never); its token stays the same
    void setSelectedSecretExpiry(const QDateTime& expiresAt);
    // When the selected service account stops working (invalid: never)
    void setExpiry(const QDateTime& expiresAt);
    void grantSelectedVault();
    // Take the selected service account out of a vault, which gets a new key
    void revokeVault(const QString& vaultId);
    void toggleDisabled();
    void deleteSelected();
    void loadAudit(bool more = false);
    void loadMembers();
    // Confirm an accepted member; `vaults` (vault id -> role) only matters for regular members
    void confirmMember(const QString& userId, const QHash<QString, QString>& vaults = {});
    void setMemberRole(const QString& userId, const QString& role);
    // Removing a member rotates the keys of the org vaults they could open
    void removeMember(const QString& userId);
    void createOrg(const QString& name);
    void loadInvitations();
    void invite(const QString& email, const QString& role);
    void cancelSelectedInvitation();
    void setShareLinkPolicy(const QString& policy);
    // Select the first member who waits to be confirmed and ask to confirm them
    void reviewWaitingMembers();

private slots:
    void askForNewServiceAccount();
    void askForNewSecret();
    void askForSecretExpiry();
    void askForExpiry();
    void askToRevokeVault();
    void askToConfirmMember();
    void askForMemberRole();
    void askToRemoveMember();
    void askForNewOrg();
    void askToInvite();
    void askForShareLinkPolicy(int id);
    void updateMemberButtons();
    void updateServiceAccountButtons();
    void showDetails();
    void showMemberMenu(const QPoint& pos);
    void filterAudit();
    void showAudit();

private:
    QWidget* buildSidebar();
    QWidget* buildMembersPage();
    QWidget* buildInvitationsPage();
    QWidget* buildServiceAccountsPage();
    QWidget* buildShareLinksPage();
    QWidget* buildAuditPage();
    QWidget* buildEmptyPage();
    void applyTheme();

    void setBusy(const QString& message);
    void showMessage(const QString& message, int severity, bool autoHide = true);
    void showError(const QString& error);
    void hideMessage();
    void showToken(const QString& title, const QString& token);
    void loadServiceAccounts(const QString& select = {});
    void loadVaultAccess();
    void showOrgs();
    void updateNavigation();
    void updateShareLinks();
    // The dates and times a prompt asks for: a "Never expires" box and a date
    bool askForDate(const QString& title, const QString& text, QDateTime& expiresAt);
    ServiceAccountAdmin::ServiceAccount selected() const;
    ServiceAccountAdmin::Member member(const QString& userId) const;
    QString selectedMember() const;
    bool isSelf(const ServiceAccountAdmin::Member& member) const;
    // An email, service account or vault name for a principal or id the audit log names, where known
    QString displayName(const QString& id) const;
    static QString roleName(const QString& role);
    static QString vaultRoleName(const QString& role);

    QPointer<AccountManager> m_account;
    ServiceAccountAdmin* m_admin;
    QList<ServiceAccountAdmin::ServiceAccount> m_accounts;
    QList<ServiceAccountAdmin::Secret> m_secrets;
    QList<ServiceAccountAdmin::Member> m_members;
    QList<ServiceAccountAdmin::Org> m_orgs;
    QList<ServiceAccountAdmin::Invitation> m_invitations;
    QHash<QString, QString> m_vaultAccess;
    // The org to select once the orgs are loaded again, e.g. a new one
    QString m_selectOrg;
    QString m_auditCursor;
    QList<ServiceAccountAdmin::AuditEvent> m_auditEvents;
    QPointer<QDialog> m_tokenDialog;
    bool m_busy = false;
    bool m_embedded = false;

    // Sidebar
    VaultComboBox* m_orgCombo;
    QLabel* m_orgRole;
    QListWidget* m_navigation;
    VaultButton* m_newOrgButton;
    QStackedWidget* m_pages;
    QWidget* m_emptyPage;
    QLabel* m_emptyText;
    fluent::status_info::InfoBar* m_infoBar;
    QTimer* m_infoTimer;
    fluent::status_info::ProgressBar* m_progress;
    QLabel* m_statusLabel;
    QDialogButtonBox* m_buttonBox;

    // Members
    fluent::status_info::InfoBar* m_waitingBar;
    QTreeWidget* m_membersTree;
    QPushButton* m_confirmMemberButton;
    QPushButton* m_changeRoleButton;
    QPushButton* m_removeMemberButton;
    QLabel* m_memberHint;

    // Invitations
    QTreeWidget* m_invitationsTree;
    QLabel* m_invitationsEmpty;
    VaultButton* m_inviteButton;
    QPushButton* m_cancelInvitationButton;

    // Service accounts
    QListWidget* m_accountList;
    QLabel* m_accountsEmpty;
    VaultButton* m_newButton;
    QStackedWidget* m_detailsStack;
    QLabel* m_detailsLabel;
    QScrollArea* m_details;
    QLabel* m_saName;
    QLabel* m_saCreated;
    QLabel* m_saFingerprint;
    QLabel* m_saExpiry;
    QPushButton* m_expiryButton;
    fluent::basicinput::ToggleSwitch* m_enabledSwitch;
    QLabel* m_enabledLabel;
    QTreeWidget* m_secretsTree;
    VaultButton* m_issueButton;
    QPushButton* m_secretExpiryButton;
    QPushButton* m_revokeButton;
    QTreeWidget* m_vaultAccessTree;
    QLabel* m_vaultAccessEmpty;
    QPushButton* m_revokeVaultButton;
    VaultComboBox* m_vaultCombo;
    VaultComboBox* m_vaultRoleCombo;
    QPushButton* m_grantButton;
    QPushButton* m_deleteButton;

    // Share links
    QButtonGroup* m_shareLinks;
    QLabel* m_shareLinksHint;

    // Audit log
    VaultLineEdit* m_auditFilter;
    QTreeWidget* m_auditTree;
    QLabel* m_auditEmpty;
    VaultButton* m_loadMoreButton;
};

#endif // KEEPASSXC_SERVICEACCOUNTSDIALOG_H
