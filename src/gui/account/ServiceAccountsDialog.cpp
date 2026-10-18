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

#include "ServiceAccountsDialog.h"
#include "ui_ServiceAccountsDialog.h"

#include "account/AccountManager.h"
#include "account/VaultKeyring.h"
#include "gui/Clipboard.h"
#include "gui/MessageBox.h"
#include "gui/vault/VaultControls.h"

#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
    constexpr int IdRole = Qt::UserRole;

    QString formatTime(const QDateTime& time, const QString& none)
    {
        return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : none;
    }
} // namespace

ServiceAccountsDialog::ServiceAccountsDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::ServiceAccountsDialog())
    , m_account(account)
    , m_admin(new ServiceAccountAdmin(account, this))
{
    m_ui->setupUi(this);
    setObjectName("serviceAccountsDialog");
    m_ui->messageWidget->setHidden(true);
    m_ui->vaultRoleCombo->addItem(tr("Read only"), "read");
    m_ui->vaultRoleCombo->addItem(tr("Can edit"), "write");
    m_ui->expiryEdit->setDateTime(QDateTime::currentDateTime().addDays(90));
    m_ui->secretsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_ui->auditTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_ui->loadMoreButton->setEnabled(false);

    connect(m_ui->orgCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        loadServiceAccounts();
        loadMembers();
        loadAudit();
    });
    connect(m_ui->confirmMemberButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askToConfirmMember);
    connect(m_ui->membersTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto items = m_ui->membersTree->selectedItems();
        m_ui->confirmMemberButton->setEnabled(!m_busy && !items.isEmpty()
                                              && items.first()->data(0, Qt::UserRole + 1).toString() == "accepted");
    });
    m_ui->confirmMemberButton->setEnabled(false);
    connect(m_ui->accountList, &QListWidget::currentRowChanged, this, &ServiceAccountsDialog::showDetails);
    connect(m_ui->newButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewServiceAccount);
    connect(m_ui->disableButton, &QPushButton::clicked, this, &ServiceAccountsDialog::toggleDisabled);
    connect(m_ui->deleteButton, &QPushButton::clicked, this, &ServiceAccountsDialog::deleteSelected);
    connect(m_ui->applyExpiryButton, &QPushButton::clicked, this, &ServiceAccountsDialog::applyExpiry);
    connect(m_ui->neverExpiresCheckBox, &QCheckBox::toggled, m_ui->expiryEdit, &QWidget::setDisabled);
    connect(m_ui->issueButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewSecret);
    connect(m_ui->revokeButton, &QPushButton::clicked, this, &ServiceAccountsDialog::revokeSelectedSecret);
    connect(m_ui->secretExpiryButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForSecretExpiry);
    connect(m_ui->grantButton, &QPushButton::clicked, this, &ServiceAccountsDialog::grantSelectedVault);
    connect(m_ui->loadMoreButton, &QPushButton::clicked, this, [this] { loadAudit(true); });
    connect(m_ui->secretsTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_ui->revokeButton->setEnabled(!m_busy && !m_ui->secretsTree->selectedItems().isEmpty());
        m_ui->secretExpiryButton->setEnabled(!m_busy && !m_ui->secretsTree->selectedItems().isEmpty());
    });
    connect(m_ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Vaults this account manages are the ones it can grant
    if (m_account) {
        for (const auto& vault : m_account->keyring().vaults()) {
            if (vault.role == "manage") {
                m_ui->vaultCombo->addItem(vault.name.isEmpty() ? vault.id : vault.name, vault.id);
            }
        }
    }
    showDetails();
    reload();
}

ServiceAccountsDialog::~ServiceAccountsDialog() = default;

QString ServiceAccountsDialog::currentOrg() const
{
    return m_ui->orgCombo->currentData().toString();
}

QString ServiceAccountsDialog::currentServiceAccount() const
{
    return selected().id;
}

bool ServiceAccountsDialog::isBusy() const
{
    return m_busy;
}

QDialog* ServiceAccountsDialog::tokenDialog() const
{
    return m_tokenDialog;
}

ServiceAccountAdmin::ServiceAccount ServiceAccountsDialog::selected() const
{
    const auto row = m_ui->accountList->currentRow();
    return row >= 0 && row < m_accounts.size() ? m_accounts.at(row) : ServiceAccountAdmin::ServiceAccount();
}

void ServiceAccountsDialog::setBusy(const QString& message)
{
    m_busy = !message.isEmpty();
    m_ui->statusLabel->setText(message);
    const bool hasOrg = !currentOrg().isEmpty();
    m_ui->newButton->setEnabled(!m_busy && hasOrg);
    showDetails();
}

void ServiceAccountsDialog::showError(const QString& error)
{
    setBusy({});
    m_ui->messageWidget->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
}

void ServiceAccountsDialog::reload()
{
    setBusy(tr("Loading organizations…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->loadOrgs([guard](const QList<ServiceAccountAdmin::Org>& orgs, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->showError(error);
            return;
        }
        auto* combo = guard->m_ui->orgCombo;
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto& org : orgs) {
            if (org.canAdminister()) {
                combo->addItem(org.name, org.id);
            }
        }
        guard->setBusy({});
        if (combo->count() == 0) {
            guard->m_ui->messageWidget->showMessage(
                tr("Service accounts belong to an organization. You need to be an owner or admin of one."),
                MessageWidget::Information,
                MessageWidget::DisableAutoHide);
            return;
        }
        combo->setCurrentIndex(0);
        guard->loadServiceAccounts();
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::loadServiceAccounts(const QString& select)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    const auto keep = select.isEmpty() ? currentServiceAccount() : select;
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->list(
        orgId, [guard, keep](const QList<ServiceAccountAdmin::ServiceAccount>& accounts, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showError(error);
                return;
            }
            guard->m_accounts = accounts;
            auto* list = guard->m_ui->accountList;
            const QSignalBlocker blocker(list);
            list->clear();
            int row = accounts.isEmpty() ? -1 : 0;
            for (int i = 0; i < accounts.size(); ++i) {
                const auto& account = accounts.at(i);
                auto* item =
                    new QListWidgetItem(account.disabled ? tr("%1 (disabled)").arg(account.name) : account.name);
                item->setData(IdRole, account.id);
                list->addItem(item);
                if (account.id == keep) {
                    row = i;
                }
            }
            list->setCurrentRow(row);
            guard->showDetails();
        });
}

void ServiceAccountsDialog::showDetails()
{
    const auto account = selected();
    const bool has = !account.id.isEmpty();
    const bool enabled = has && !m_busy;
    m_ui->detailsWidget->setEnabled(has);
    m_ui->disableButton->setEnabled(enabled);
    m_ui->disableButton->setText(account.disabled ? tr("Enable") : tr("Disable"));
    m_ui->deleteButton->setEnabled(enabled);
    m_ui->applyExpiryButton->setEnabled(enabled);
    m_ui->issueButton->setEnabled(enabled);
    m_ui->revokeButton->setEnabled(enabled && !m_ui->secretsTree->selectedItems().isEmpty());
    m_ui->secretExpiryButton->setEnabled(enabled && !m_ui->secretsTree->selectedItems().isEmpty());
    m_ui->grantButton->setEnabled(enabled && m_ui->vaultCombo->count() > 0);
    m_ui->newButton->setEnabled(!m_busy && !currentOrg().isEmpty());
    if (!has) {
        m_ui->detailsLabel->setText(tr("Select a service account, or create one for a CI job, a server or a "
                                       "Kubernetes cluster."));
        m_ui->secretsTree->clear();
        m_ui->secretsTree->setProperty("account", QString());
        return;
    }
    m_ui->detailsLabel->setText(
        tr("<b>%1</b><br>Created %2<br>Fingerprint: %3")
            .arg(account.name.toHtmlEscaped(),
                 formatTime(account.createdAt, tr("unknown")),
                 account.publicKeys.isNull() ? tr("unknown") : AccountCrypto::fingerprint(account.publicKeys)));
    {
        const QSignalBlocker blocker(m_ui->neverExpiresCheckBox);
        m_ui->neverExpiresCheckBox->setChecked(!account.expiresAt.isValid());
        m_ui->expiryEdit->setDisabled(!account.expiresAt.isValid());
        if (account.expiresAt.isValid()) {
            m_ui->expiryEdit->setDateTime(account.expiresAt.toLocalTime());
        }
    }
    // The secrets are fetched again only for another service account, or after they changed
    if (m_ui->secretsTree->property("account").toString() == account.id) {
        return;
    }
    m_ui->secretsTree->setProperty("account", account.id);
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->listSecrets(
        account.id, [guard, id = account.id](const QList<ServiceAccountAdmin::Secret>& secrets, const QString& error) {
            if (!guard || guard->currentServiceAccount() != id) {
                return;
            }
            if (!error.isEmpty()) {
                guard->m_ui->messageWidget->showMessage(error, MessageWidget::Error);
                return;
            }
            guard->m_secrets = secrets;
            auto* tree = guard->m_ui->secretsTree;
            tree->clear();
            for (const auto& secret : secrets) {
                auto* item = new QTreeWidgetItem(tree);
                item->setData(0, IdRole, secret.id);
                item->setText(0, secret.name.isEmpty() ? tr("(unnamed)") : secret.name);
                item->setText(1, secret.prefix + "…");
                item->setFont(1, QFontDatabase::systemFont(QFontDatabase::FixedFont));
                item->setText(2, formatTime(secret.lastUsedAt, tr("never")));
                item->setText(3, formatTime(secret.expiresAt, tr("never")));
            }
        });
}

void ServiceAccountsDialog::askForNewServiceAccount()
{
    bool ok = false;
    const auto name = QInputDialog::getText(
        this, tr("New service account"), tr("Name, e.g. the job or server that uses it:"), QLineEdit::Normal, {}, &ok);
    if (ok && !name.trimmed().isEmpty()) {
        createServiceAccount(name.trimmed());
    }
}

void ServiceAccountsDialog::createServiceAccount(const QString& name, const QDateTime& expiresAt)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    setBusy(tr("Creating %1…").arg(name));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->create(
        orgId,
        name,
        expiresAt,
        [guard](const ServiceAccountAdmin::ServiceAccount& account, const QString& token, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showError(error);
                return;
            }
            guard->setBusy({});
            guard->showToken(tr("Token for %1").arg(account.name), token);
            guard->loadServiceAccounts(account.id);
            guard->loadAudit();
        });
}

void ServiceAccountsDialog::askForNewSecret()
{
    bool ok = false;
    const auto name = QInputDialog::getText(
        this, tr("Issue a secret"), tr("Name, e.g. where it will be used:"), QLineEdit::Normal, {}, &ok);
    if (ok) {
        issueSecret(name.trimmed());
    }
}

void ServiceAccountsDialog::issueSecret(const QString& name, const QDateTime& expiresAt)
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    setBusy(tr("Issuing a secret…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->issueSecret(
        account.id,
        name,
        expiresAt,
        [guard, account](const ServiceAccountAdmin::ServiceAccount&, const QString& token, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showError(error);
                return;
            }
            guard->setBusy({});
            guard->showToken(tr("New token for %1").arg(account.name), token);
            guard->m_ui->secretsTree->setProperty("account", QString());
            guard->showDetails();
        });
}

void ServiceAccountsDialog::revokeSelectedSecret()
{
    const auto account = selected();
    const auto items = m_ui->secretsTree->selectedItems();
    if (account.id.isEmpty() || items.isEmpty()) {
        return;
    }
    const auto secretId = items.first()->data(0, IdRole).toString();
    auto answer = MessageBox::question(this,
                                       tr("Revoke this secret?"),
                                       tr("Anything that uses a token with this secret stops working at once."),
                                       MessageBox::Remove | MessageBox::Cancel,
                                       MessageBox::Cancel);
    if (answer != MessageBox::Remove) {
        return;
    }
    setBusy(tr("Revoking…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->revokeSecret(account.id, secretId, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->m_ui->secretsTree->setProperty("account", QString());
        guard->showDetails();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askForSecretExpiry()
{
    const auto items = m_ui->secretsTree->selectedItems();
    if (selected().id.isEmpty() || items.isEmpty()) {
        return;
    }
    const auto secretId = items.first()->data(0, IdRole).toString();
    QDateTime current;
    for (const auto& secret : m_secrets) {
        if (secret.id == secretId) {
            current = secret.expiresAt;
        }
    }

    QDialog picker(this);
    picker.setWindowTitle(tr("Change expiry"));
    auto* label = new QLabel(tr("The secret keeps its token; only the time it stops working changes. "
                                "A time that has passed stops it at once."),
                             &picker);
    label->setWordWrap(true);
    auto* never = new VaultCheckBox(&picker);
    never->setObjectName("secretNeverExpiresCheckBox");
    never->setText(tr("Never expires"));
    never->setChecked(!current.isValid());
    auto* edit = new QDateTimeEdit(&picker);
    edit->setObjectName("secretExpiryEdit");
    edit->setCalendarPopup(true);
    edit->setDateTime(current.isValid() ? current.toLocalTime() : QDateTime::currentDateTime().addDays(90));
    edit->setDisabled(!current.isValid());
    connect(never, &QCheckBox::toggled, edit, &QWidget::setDisabled);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &picker);
    connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    auto* layout = new QVBoxLayout(&picker);
    layout->addWidget(label);
    layout->addWidget(never);
    layout->addWidget(edit);
    layout->addWidget(buttons);
    if (picker.exec() != QDialog::Accepted) {
        return;
    }
    setSelectedSecretExpiry(never->isChecked() ? QDateTime() : edit->dateTime().toUTC());
}

void ServiceAccountsDialog::setSelectedSecretExpiry(const QDateTime& expiresAt)
{
    const auto account = selected();
    const auto items = m_ui->secretsTree->selectedItems();
    if (account.id.isEmpty() || items.isEmpty()) {
        return;
    }
    const auto secretId = items.first()->data(0, IdRole).toString();
    setBusy(tr("Saving…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->setSecretExpiry(account.id, secretId, expiresAt, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->m_ui->secretsTree->setProperty("account", QString());
        guard->showDetails();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::grantSelectedVault()
{
    const auto account = selected();
    const auto vaultId = m_ui->vaultCombo->currentData().toString();
    if (account.id.isEmpty() || vaultId.isEmpty()) {
        return;
    }
    const auto role = m_ui->vaultRoleCombo->currentData().toString();
    setBusy(tr("Granting access…"));
    QPointer<ServiceAccountsDialog> guard(this);
    const auto vaultName = m_ui->vaultCombo->currentText();
    m_admin->grantVault(account, vaultId, role, [guard, account, vaultName](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->m_ui->messageWidget->showMessage(tr("%1 can now open \"%2\".").arg(account.name, vaultName),
                                                MessageWidget::Positive);
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::applyExpiry()
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    const auto expiresAt = m_ui->neverExpiresCheckBox->isChecked() ? QDateTime() : m_ui->expiryEdit->dateTime().toUTC();
    setBusy(tr("Saving…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->setExpiry(account.id, expiresAt, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->loadServiceAccounts();
    });
}

void ServiceAccountsDialog::toggleDisabled()
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    setBusy(account.disabled ? tr("Enabling…") : tr("Disabling…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->setDisabled(account.id, !account.disabled, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->loadServiceAccounts();
    });
}

void ServiceAccountsDialog::deleteSelected()
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    auto answer =
        MessageBox::question(this,
                             tr("Delete %1?").arg(account.name),
                             tr("Its secrets stop working and it loses access to every vault. Those vaults should "
                                "get a new key afterwards (Share, then Rotate key)."),
                             MessageBox::Delete | MessageBox::Cancel,
                             MessageBox::Cancel);
    if (answer != MessageBox::Delete) {
        return;
    }
    setBusy(tr("Deleting…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->remove(account.id, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->loadServiceAccounts();
        if (guard->m_account) {
            guard->m_account->reloadVaults();
        }
    });
}

void ServiceAccountsDialog::loadAudit(bool more)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    if (!more) {
        m_auditCursor.clear();
    }
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->auditLog(
        orgId,
        m_auditCursor,
        [guard, more](const QList<ServiceAccountAdmin::AuditEvent>& events, const QString& next, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->m_ui->messageWidget->showMessage(error, MessageWidget::Error);
                return;
            }
            auto* tree = guard->m_ui->auditTree;
            if (!more) {
                tree->clear();
            }
            for (const auto& event : events) {
                auto* item = new QTreeWidgetItem(tree);
                item->setText(0, formatTime(event.at, {}));
                item->setText(1, event.actor);
                item->setText(2, event.action);
                item->setText(3, event.target);
                item->setText(4, event.ip);
            }
            guard->m_auditCursor = next;
            guard->m_ui->loadMoreButton->setEnabled(!next.isEmpty());
        });
}

void ServiceAccountsDialog::loadMembers()
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->listMembers(orgId, [guard](const QList<ServiceAccountAdmin::Member>& members, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->m_ui->messageWidget->showMessage(error, MessageWidget::Error);
            return;
        }
        guard->m_members = members;
        auto* tree = guard->m_ui->membersTree;
        tree->clear();
        for (const auto& member : members) {
            auto* item = new QTreeWidgetItem(tree);
            item->setData(0, IdRole, member.userId);
            item->setData(0, Qt::UserRole + 1, member.status);
            item->setText(0, member.email);
            item->setText(1, member.role);
            item->setText(2, member.status);
            item->setText(3,
                          member.publicKeys.isNull() ? tr("unknown")
                          : member.verified          ? AccountCrypto::fingerprint(member.publicKeys)
                                                     : tr("invalid signature"));
            item->setFont(3, QFontDatabase::systemFont(QFontDatabase::FixedFont));
        }
        guard->m_ui->confirmMemberButton->setEnabled(false);
    });
}

void ServiceAccountsDialog::askToConfirmMember()
{
    const auto items = m_ui->membersTree->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    const auto userId = items.first()->data(0, IdRole).toString();
    ServiceAccountAdmin::Member member;
    for (const auto& candidate : m_members) {
        if (candidate.userId == userId) {
            member = candidate;
        }
    }
    if (member.isAdmin()) {
        auto answer = MessageBox::question(
            this,
            tr("Confirm %1?").arg(member.email),
            tr("As %1 of the organization, %2 gets every vault with manage access and can issue secrets for its "
               "service accounts. Check that their fingerprint is %3.")
                .arg(member.role, member.email, AccountCrypto::fingerprint(member.publicKeys)),
            MessageBox::Ok | MessageBox::Cancel,
            MessageBox::Cancel);
        if (answer == MessageBox::Ok) {
            confirmMember(userId);
        }
        return;
    }

    // A regular member gets the vaults picked here
    QDialog picker(this);
    picker.setWindowTitle(tr("Confirm %1").arg(member.email));
    auto* label = new QLabel(tr("Choose the vaults %1 can open. Check that their fingerprint is %2.")
                                 .arg(member.email, AccountCrypto::fingerprint(member.publicKeys)),
                             &picker);
    label->setWordWrap(true);
    auto* tree = new QTreeWidget(&picker);
    tree->setRootIsDecorated(false);
    tree->setHeaderLabels({tr("Vault"), tr("Access")});
    QHash<QTreeWidgetItem*, QComboBox*> roles;
    for (const auto& vault : m_admin->orgVaults(currentOrg())) {
        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, vault.second);
        item->setData(0, IdRole, vault.first);
        item->setCheckState(0, Qt::Unchecked);
        auto* combo = new VaultComboBox(tree);
        combo->addItem(tr("Read only"), "read");
        combo->addItem(tr("Can edit"), "write");
        combo->addItem(tr("Can manage"), "manage");
        tree->setItemWidget(item, 1, combo);
        roles.insert(item, combo);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
    connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    auto* layout = new QVBoxLayout(&picker);
    layout->addWidget(label);
    layout->addWidget(tree);
    layout->addWidget(buttons);
    if (picker.exec() != QDialog::Accepted) {
        return;
    }
    QHash<QString, QString> vaults;
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        if (it.key()->checkState(0) == Qt::Checked) {
            vaults.insert(it.key()->data(0, IdRole).toString(), it.value()->currentData().toString());
        }
    }
    confirmMember(userId, vaults);
}

void ServiceAccountsDialog::confirmMember(const QString& userId, const QHash<QString, QString>& vaults)
{
    ServiceAccountAdmin::Member member;
    for (const auto& candidate : m_members) {
        if (candidate.userId == userId) {
            member = candidate;
        }
    }
    if (member.userId.isEmpty()) {
        return;
    }
    setBusy(tr("Confirming %1…").arg(member.email));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->confirmMember(currentOrg(), member, vaults, [guard, member](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->m_ui->messageWidget->showMessage(tr("%1 is confirmed.").arg(member.email), MessageWidget::Positive);
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::showToken(const QString& title, const QString& token)
{
    delete m_tokenDialog.data();
    auto* dialog = new QDialog(this);
    dialog->setObjectName("serviceAccountTokenDialog");
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* label = new QLabel(tr("Copy this token now and store it where it will be used, e.g. as a CI secret "
                                "named KPX_SERVICE_ACCOUNT_TOKEN. It is shown only once and can't be recovered; "
                                "issue a new secret if it is lost."),
                             dialog);
    label->setWordWrap(true);
    auto* field = new VaultLineEdit(dialog);
    field->setObjectName("serviceAccountToken");
    field->setReadOnly(true);
    field->setText(token);
    field->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    field->setMinimumWidth(480);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, dialog, [field] { clipboard()->setText(field->text()); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, [dialog, field] {
        field->clear();
        dialog->close();
    });
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(label);
    layout->addWidget(field);
    layout->addWidget(buttons);
    m_tokenDialog = dialog;
    dialog->open();
}
