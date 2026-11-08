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

#include "DeleteAccountDialog.h"

#include "account/AccountManager.h"
#include "account/AuthClient.h"
#include "account/ServiceAccountAdmin.h"
#include "gui/MessageBox.h"
#include "gui/MessageWidget.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
    QString longDate(const QString& iso)
    {
        const auto at = QDateTime::fromString(iso, Qt::ISODate);
        return at.isValid() ? QLocale().toString(at.toLocalTime().date(), QLocale::LongFormat) : iso;
    }

    bool isUser(const QString& principal)
    {
        return principal.startsWith("user:");
    }
} // namespace

DeleteAccountDialog::DeleteAccountDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_account(account)
    , m_message(new MessageWidget(this))
    , m_intro(new QLabel(this))
    , m_summary(new QLabel(this))
    , m_actions(new QWidget(this))
    , m_actionsLayout(new QVBoxLayout(m_actions))
    , m_confirm(new QLineEdit(this))
    , m_password(new QLineEdit(this))
    , m_delete(nullptr)
{
    setWindowTitle(tr("Delete Account"));
    resize(560, 560);
    auto* layout = new QVBoxLayout(this);
    m_message->setHidden(true);
    layout->addWidget(m_message);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_intro->setWordWrap(true);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    bodyLayout->addWidget(m_intro);
    bodyLayout->addWidget(m_summary);
    m_actionsLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->addWidget(m_actions);

    auto* billing = new QLabel(tr("Plans are billed by OpenVireo through Stripe: yours stops renewing, with no refund "
                                  "for time left. Receipts stay on link.com; ask Link to delete them. Local database "
                                  "files you opened yourself are not touched. Export anything you want to keep first."),
                               this);
    billing->setWordWrap(true);
    bodyLayout->addWidget(billing);

    auto* confirmBox = new QGroupBox(tr("Confirm"), this);
    auto* form = new QFormLayout(confirmBox);
    m_confirm->setPlaceholderText(m_account ? m_account->email() : QString());
    m_confirm->setAccessibleName(tr("Account email"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(tr("Master password"));
    form->addRow(tr("Account email:"), m_confirm);
    form->addRow(tr("Master password:"), m_password);
    bodyLayout->addWidget(confirmBox);
    bodyLayout->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_delete = buttons->addButton(tr("Delete Account"), QDialogButtonBox::DestructiveRole);
    m_delete->setObjectName("deleteAccountButton");
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_delete, &QPushButton::clicked, this, &DeleteAccountDialog::deleteAccount);
    connect(m_confirm, &QLineEdit::textChanged, this, &DeleteAccountDialog::updateDeleteButton);
    connect(m_password, &QLineEdit::textChanged, this, &DeleteAccountDialog::updateDeleteButton);
    layout->addWidget(buttons);

    m_intro->setText(tr("Checking what will be deleted…"));
    updateDeleteButton();
    reload();
}

QStringList DeleteAccountDialog::summary(const QJsonObject& preview)
{
    QStringList lines;
    for (const auto& value : preview.value("vaults").toArray()) {
        const auto vault = value.toObject();
        QStringList who;
        for (const auto& s : vault.value("shared_with").toArray()) {
            const auto shared = s.toObject();
            who << shared.value("display").toString(shared.value("principal").toString());
        }
        const auto name = vault.value("name").toString(tr("A vault"));
        lines << (who.isEmpty() ? tr("%1 is deleted.").arg(name)
                                : tr("%1 is deleted. %2 lose(s) access too.").arg(name, who.join(", ")));
    }
    for (const auto& value : preview.value("orgs").toArray()) {
        const auto org = value.toObject();
        const auto outcome = org.value("outcome").toString();
        if (outcome == "leave") {
            lines << tr("You leave %1.").arg(org.value("name").toString());
        } else if (outcome == "delete") {
            lines << tr("%1 is deleted with your account, with its vaults.").arg(org.value("name").toString());
        }
    }
    if (preview.value("subscription").isObject()) {
        lines << tr("Your %1 plan stops renewing.").arg(preview.value("subscription").toObject().value("plan").toString());
    }
    const int links = preview.value("share_links").toInt();
    if (links > 0) {
        lines << tr("%n share link(s) stop working now.", "", links);
    }
    return lines;
}

void DeleteAccountDialog::reload()
{
    if (!m_account) {
        return;
    }
    setBusy(true);
    QPointer<DeleteAccountDialog> guard(this);
    m_account->deletionPreview([guard](const QJsonObject& preview, const QString& error) {
        if (!guard) {
            return;
        }
        guard->setBusy(false);
        if (!error.isEmpty()) {
            guard->m_message->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
            return;
        }
        guard->m_preview = preview;
        guard->fill();
    });
}

void DeleteAccountDialog::fill()
{
    // Vault names are encrypted on the server; this device knows them from the vault list
    auto preview = m_preview;
    QJsonArray vaults;
    for (const auto& value : preview.value("vaults").toArray()) {
        auto vault = value.toObject();
        const auto name = m_account->keyring().vault(vault.value("id").toString()).name;
        if (!name.isEmpty()) {
            vault.insert("name", name);
        }
        vaults << vault;
    }
    preview.insert("vaults", vaults);

    m_intro->setText(tr("Your account is locked as soon as you confirm, and permanently deleted with its vaults on "
                        "<b>%1</b>. Until then you can sign in and restore it.")
                         .arg(longDate(preview.value("would_erase_on").toString())));
    QString html = "<ul>";
    for (const auto& line : summary(preview)) {
        html += "<li>" + line.toHtmlEscaped() + "</li>";
    }
    m_summary->setText(html + "</ul>");

    while (auto* item = m_actionsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const auto& value : preview.value("orgs").toArray()) {
        const auto org = value.toObject();
        if (org.value("outcome").toString() != "blocked") {
            continue;
        }
        const auto orgId = org.value("id").toString();
        const auto orgName = org.value("name").toString();
        auto* box = new QGroupBox(orgName, m_actions);
        auto* boxLayout = new QVBoxLayout(box);
        auto* note = new QLabel(tr("You're the only owner. Make someone else an owner, or delete the organization, "
                                   "before you delete your account."),
                                box);
        note->setWordWrap(true);
        boxLayout->addWidget(note);
        auto* row = new QHBoxLayout();
        auto* members = new QComboBox(box);
        for (const auto& m : org.value("members").toArray()) {
            const auto member = m.toObject();
            const auto role = member.value("role").toString();
            members->addItem(role == "admin" ? tr("%1 (admin)").arg(member.value("email").toString())
                                             : member.value("email").toString(),
                             member.value("id").toString());
        }
        auto* makeOwner = new QPushButton(tr("Make Owner"), box);
        auto* deleteOrg = new QPushButton(tr("Delete Organization…"), box);
        row->addWidget(members, 1);
        row->addWidget(makeOwner);
        row->addWidget(deleteOrg);
        boxLayout->addLayout(row);
        m_actionsLayout->addWidget(box);

        connect(makeOwner, &QPushButton::clicked, this, [this, orgId, members] {
            const auto userId = members->currentData().toString();
            if (userId.isEmpty() || !m_account) {
                return;
            }
            setBusy(true);
            // Seals the service account master keys to a new owner who didn't have them, as Organization does
            auto* admin = new ServiceAccountAdmin(m_account, this);
            QPointer<DeleteAccountDialog> guard(this);
            auto finished = [guard, admin](bool ok, const QString& error) {
                admin->deleteLater();
                if (!guard) {
                    return;
                }
                guard->setBusy(false);
                if (!ok) {
                    guard->m_message->showMessage(error, MessageWidget::Error);
                    return;
                }
                guard->reload();
            };
            admin->listMembers(orgId, [admin, orgId, userId, finished](const QList<ServiceAccountAdmin::Member>& list,
                                                                      const QString& error) {
                for (const auto& member : list) {
                    if (member.userId == userId) {
                        admin->setMemberRole(orgId, member, "owner", finished);
                        return;
                    }
                }
                finished(false, error.isEmpty() ? DeleteAccountDialog::tr("That member has left the organization.") : error);
            });
        });
        connect(deleteOrg, &QPushButton::clicked, this, [this, orgId, orgName] {
            const auto answer = MessageBox::question(
                this,
                tr("Delete %1?").arg(orgName),
                tr("Its vaults and service accounts are deleted now, and its members lose access. This can't be undone."),
                MessageBox::Delete | MessageBox::Cancel,
                MessageBox::Cancel);
            if (answer != MessageBox::Delete || !m_account) {
                return;
            }
            setBusy(true);
            QPointer<DeleteAccountDialog> guard(this);
            m_account->client()->deleteOrg(orgId, [guard](const AuthClient::Response& response) {
                if (!guard) {
                    return;
                }
                guard->setBusy(false);
                if (!response.isSuccess()) {
                    guard->m_message->showMessage(AuthClient::errorMessage(response), MessageWidget::Error);
                    return;
                }
                guard->reload();
            });
        });
    }

    for (const auto& value : preview.value("vaults").toArray()) {
        const auto vault = value.toObject();
        QList<QJsonObject> people;
        for (const auto& s : vault.value("shared_with").toArray()) {
            if (isUser(s.toObject().value("principal").toString())) {
                people << s.toObject();
            }
        }
        if (people.isEmpty()) {
            continue;
        }
        const auto vaultId = vault.value("id").toString();
        auto* box = new QGroupBox(vault.value("name").toString(tr("Shared vault")), m_actions);
        auto* boxLayout = new QVBoxLayout(box);
        auto* note = new QLabel(tr("Shared vaults are deleted with your account. Give it to someone who has it, so "
                                   "they keep it."),
                                box);
        note->setWordWrap(true);
        boxLayout->addWidget(note);
        auto* row = new QHBoxLayout();
        auto* recipients = new QComboBox(box);
        for (const auto& person : people) {
            recipients->addItem(person.value("display").toString(person.value("principal").toString()),
                                person.value("principal").toString());
        }
        auto* give = new QPushButton(tr("Give Vault"), box);
        row->addWidget(recipients, 1);
        row->addWidget(give);
        boxLayout->addLayout(row);
        m_actionsLayout->addWidget(box);

        connect(give, &QPushButton::clicked, this, [this, vaultId, recipients] {
            if (!m_account) {
                return;
            }
            setBusy(true);
            QPointer<DeleteAccountDialog> guard(this);
            m_account->transferVault(vaultId, recipients->currentData().toString(), [guard](const QString& error) {
                if (!guard) {
                    return;
                }
                guard->setBusy(false);
                if (!error.isEmpty()) {
                    guard->m_message->showMessage(error, MessageWidget::Error);
                    return;
                }
                guard->reload();
            });
        });
    }
    updateDeleteButton();
}

void DeleteAccountDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_actions->setEnabled(!busy);
    updateDeleteButton();
}

void DeleteAccountDialog::updateDeleteButton()
{
    if (!m_delete) {
        return;
    }
    const bool confirmed = m_account && !m_password->text().isEmpty()
                           && m_confirm->text().trimmed().compare(m_account->email(), Qt::CaseInsensitive) == 0;
    m_delete->setEnabled(!m_busy && !m_preview.isEmpty() && !m_preview.value("blocked").toBool() && confirmed);
}

void DeleteAccountDialog::deleteAccount()
{
    if (!m_account) {
        return;
    }
    setBusy(true);
    m_message->hideMessage();
    QPointer<DeleteAccountDialog> guard(this);
    m_account->deleteAccount(
        m_password->text(), m_confirm->text(), [guard](const QString& scheduledFor, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->setBusy(false);
                guard->m_message->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
                return;
            }
            Q_UNUSED(scheduledFor)
            guard->m_password->clear();
            guard->accept();
        });
}

PendingDeletionDialog::PendingDeletionDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_account(account)
    , m_message(new MessageWidget(this))
    , m_password(new QLineEdit(this))
    , m_restore(nullptr)
{
    setWindowTitle(tr("Scheduled for Deletion"));
    auto* layout = new QVBoxLayout(this);
    m_message->setHidden(true);
    layout->addWidget(m_message);
    auto* text = new QLabel(tr("<b>%1</b> will be permanently deleted, with its vaults, on <b>%2</b>.<br><br>Until then "
                               "it is locked. Restore it to use it again, or sign out and let the deletion go ahead. "
                               "Plans that haven't ended yet start renewing again; share links don't come back.")
                                .arg(m_account->email().toHtmlEscaped(), longDate(m_account->deletionScheduledFor())),
                            this);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* form = new QFormLayout();
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(tr("Master password"));
    form->addRow(tr("Master password:"), m_password);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(this);
    m_restore = buttons->addButton(tr("Restore Account"), QDialogButtonBox::AcceptRole);
    auto* signOut = buttons->addButton(tr("Sign Out"), QDialogButtonBox::RejectRole);
    m_restore->setEnabled(false);
    connect(m_password, &QLineEdit::textChanged, this, [this] { m_restore->setEnabled(!m_password->text().isEmpty()); });
    connect(signOut, &QPushButton::clicked, this, [this] {
        if (m_account) {
            m_account->signOut();
        }
        reject();
    });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!m_account) {
            return;
        }
        m_restore->setEnabled(false);
        QPointer<PendingDeletionDialog> guard(this);
        m_account->restoreAccount(m_password->text(), [guard](const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->m_restore->setEnabled(true);
                guard->m_message->showMessage(error, MessageWidget::Error, MessageWidget::DisableAutoHide);
                return;
            }
            guard->m_password->clear();
            guard->accept();
        });
    });
    layout->addWidget(buttons);
}
