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
#include "account/AccountPlan.h"
#include "account/ServiceAccountAdmin.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultInfoBar.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultWidgets.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

namespace
{
    rust::String rs(const QString& text)
    {
        const auto utf8 = text.toUtf8();
        return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
    }
} // namespace

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

    // The card's width over the main window, as in the mockup
    constexpr int DeleteCardWidth = 560;
    constexpr int PendingCardWidth = 480;
    // WinUI's content dialog padding, for the dialog when it opens as a window of its own
    constexpr int WindowPadding = 24;

    QFont glyphFont(int pixelSize)
    {
        QFont font(Typography::FontFamily::FluentIcons);
        font.setPixelSize(pixelSize);
        return font;
    }

    // A rounded tile with a glyph or a letter, in the accent or the neutral control fill
    VaultPill* tile(const QString& glyph, const QString& letter, int size, QWidget* parent)
    {
        auto* pill = new VaultPill(glyph.isEmpty() ? letter : glyph, parent);
        pill->setAlignment(Qt::AlignCenter);
        pill->setFixedSize(size, size);
        pill->setRadius(size * 10 / 32);
        Vault::followTheme(pill, [pill, glyph, size] {
            const auto p = FluentTheme::palette();
            if (glyph.isEmpty()) {
                pill->setColors(p.accentSubtle, p.accentText);
                pill->setFont(Vault::headingFont(size * 13 / 32));
            } else {
                pill->setColors(p.control, p.textSecondary);
                pill->setFont(glyphFont(size / 2));
            }
        });
        return pill;
    }

    QLabel*
    themedLabel(const QString& text, Typography::FontRole role, QColor FluentTheme::Palette::* color, QWidget* parent)
    {
        auto* label = new QLabel(text, parent);
        label->setWordWrap(true);
        Vault::followTheme(label, [label, role, color] {
            const auto p = FluentTheme::palette();
            Vault::styleLabel(label, role, p.*color);
        });
        return label;
    }

    VaultButton* pillButton(const QString& text, const QString& name, QWidget* parent)
    {
        auto* button = new VaultButton(text, parent);
        button->setObjectName(name);
        Vault::makePill(button);
        return button;
    }

    // Scrolls the body when the window is too short for it, but asks for the body's whole height so a card that
    // fits its content shows all of it when there is room
    class FittedScrollArea : public QScrollArea
    {
    public:
        using QScrollArea::QScrollArea;

        QSize sizeHint() const override
        {
            if (!widget()) {
                return QScrollArea::sizeHint();
            }
            const int width = viewport()->width() > 0 ? viewport()->width() : widget()->sizeHint().width();
            const int height =
                widget()->hasHeightForWidth() ? widget()->heightForWidth(width) : widget()->sizeHint().height();
            return {widget()->sizeHint().width(), height};
        }
        QSize minimumSizeHint() const override
        {
            return {0, 120};
        }
    };
} // namespace

DeleteAccountDialog::DeleteAccountDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_account(account)
    , m_message(new VaultInfoBar(this))
    , m_intro(new QLabel(this))
    , m_summaryCaption(nullptr)
    , m_summary(new VaultPanel(VaultPanel::Layer, this))
    , m_summaryLayout(new QVBoxLayout(m_summary))
    , m_billing(nullptr)
    , m_actions(new QWidget(this))
    , m_actionsLayout(new QVBoxLayout(m_actions))
    , m_confirmCaption(nullptr)
    , m_confirmLabel(nullptr)
    , m_passwordLabel(nullptr)
    , m_confirm(new VaultLineEdit(this))
    , m_password(new VaultLineEdit(this))
    , m_scroll(new FittedScrollArea(this))
    , m_buttonRow(new QWidget(this))
    , m_footHint(new QLabel(m_buttonRow))
    , m_cancel(nullptr)
    , m_delete(nullptr)
{
    setObjectName("deleteAccountDialog");
    setWindowTitle(tr("Delete account"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    layout->setSpacing(12);
    m_message->setHidden(true);
    layout->addWidget(m_message);

    auto* body = new QWidget(this);
    body->setObjectName("deleteAccountBody");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 4, 0);
    bodyLayout->setSpacing(16);
    m_intro->setWordWrap(true);
    m_intro->setTextFormat(Qt::RichText);
    bodyLayout->addWidget(m_intro);

    // Orgs to hand over and shared vaults to give away, before the summary since they block or change it
    m_actionsLayout->setContentsMargins(0, 0, 0, 0);
    m_actionsLayout->setSpacing(10);
    bodyLayout->addWidget(m_actions);

    auto* summaryBlock = new QVBoxLayout();
    summaryBlock->setSpacing(8);
    m_summaryCaption =
        themedLabel(tr("What gets deleted"), Typography::FontRole::BodyStrong, &FluentTheme::Palette::text, body);
    m_summary->setObjectName("deleteAccountSummary");
    m_summaryLayout->setContentsMargins(16, 0, 16, 0);
    m_summaryLayout->setSpacing(0);
    m_billing = themedLabel(tr("Plans billed through Stripe stop renewing, with no refund for time left; receipts stay "
                               "on link.com. Local database files you opened yourself aren't touched. Export anything "
                               "you want to keep first."),
                            Typography::FontRole::Caption,
                            &FluentTheme::Palette::textTertiary,
                            body);
    summaryBlock->addWidget(m_summaryCaption);
    summaryBlock->addWidget(m_summary);
    summaryBlock->addWidget(m_billing);
    bodyLayout->addLayout(summaryBlock);

    // Confirm with the typed email and the master password, each labelled above its field
    auto* confirmBlock = new QVBoxLayout();
    confirmBlock->setSpacing(6);
    m_confirmCaption =
        themedLabel(tr("Confirm it's you"), Typography::FontRole::BodyStrong, &FluentTheme::Palette::text, body);
    m_confirmLabel = themedLabel(
        tr("Type your account email"), Typography::FontRole::Body, &FluentTheme::Palette::textSecondary, body);
    m_passwordLabel =
        themedLabel(tr("Master password"), Typography::FontRole::Body, &FluentTheme::Palette::textSecondary, body);
    m_confirmLabel->setBuddy(m_confirm);
    m_passwordLabel->setBuddy(m_password);
    m_confirm->setObjectName("deleteAccountEmail");
    m_confirm->setPlaceholderText(m_account ? m_account->email() : QString());
    m_confirm->setAccessibleName(tr("Account email"));
    m_password->setObjectName("deleteAccountPassword");
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(tr("Master password"));
    confirmBlock->addWidget(m_confirmCaption);
    confirmBlock->addSpacing(4);
    confirmBlock->addWidget(m_confirmLabel);
    confirmBlock->addWidget(m_confirm);
    confirmBlock->addSpacing(6);
    confirmBlock->addWidget(m_passwordLabel);
    confirmBlock->addWidget(m_password);
    bodyLayout->addLayout(confirmBlock);

    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(body);
    m_scroll->viewport()->setAutoFillBackground(false);
    body->setAutoFillBackground(false);
    layout->addWidget(m_scroll, 1);

    // The buttons, with what's still needed on the left
    auto* buttons = new QHBoxLayout(m_buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_footHint->setObjectName("deleteAccountHint");
    m_footHint->setWordWrap(true);
    buttons->addWidget(m_footHint, 1);
    m_cancel = pillButton(tr("Cancel"), "deleteAccountCancel", m_buttonRow);
    m_delete = pillButton(tr("Delete account"), "deleteAccountButton", m_buttonRow);
    m_delete->setDestructive(true);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_delete);
    layout->addWidget(m_buttonRow);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_delete, &QPushButton::clicked, this, &DeleteAccountDialog::deleteAccount);
    connect(m_confirm, &QLineEdit::textChanged, this, &DeleteAccountDialog::updateDeleteButton);
    connect(m_password, &QLineEdit::textChanged, this, &DeleteAccountDialog::updateDeleteButton);

    m_intro->setText(tr("Checking what will be deleted…"));
    m_summaryCaption->hide();
    m_summary->hide();
    Vault::followTheme(this, [this] { applyColors(); });
    updateDeleteButton();
    setFixedWidth(DeleteCardWidth);
    reload();
}

DeleteAccountDialog* DeleteAccountDialog::present(AccountManager* account, QWidget* parent)
{
    auto* dialog = new DeleteAccountDialog(account, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* modal = VaultModal::forDialog(dialog, QStringLiteral("vaultDeleteAccount"), DeleteCardWidth);
    if (!modal) {
        dialog->resize(DeleteCardWidth, 640);
        dialog->open();
        return dialog;
    }
    dialog->layout()->setContentsMargins(12, 4, 8, 4);
    modal->addFooterWidget(dialog->m_buttonRow, 1);
    modal->setInitialFocus(dialog->m_confirm);
    modal->open();
    return dialog;
}

void DeleteAccountDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_footHint, Typography::FontRole::Caption, p.textTertiary);
}

QList<DeleteAccountDialog::SummaryRow> DeleteAccountDialog::summary(const QJsonObject& preview)
{
    QList<SummaryRow> rows;
    for (const auto& value : preview.value("vaults").toArray()) {
        const auto vault = value.toObject();
        QStringList who;
        for (const auto& s : vault.value("shared_with").toArray()) {
            const auto shared = s.toObject();
            who << shared.value("display").toString(shared.value("principal").toString());
        }
        SummaryRow row;
        row.title = vault.value("name").toString(tr("A vault"));
        row.letter = row.title.left(1).toUpper();
        row.shared = !who.isEmpty();
        row.detail = who.isEmpty() ? tr("Vault") : tr("Vault · %1 lose(s) access too").arg(who.join(", "));
        rows << row;
    }
    for (const auto& value : preview.value("orgs").toArray()) {
        const auto org = value.toObject();
        const auto outcome = org.value("outcome").toString();
        SummaryRow row;
        row.glyph = Typography::Icons::People;
        row.title = org.value("name").toString();
        if (outcome == "leave") {
            row.detail = tr("You leave this organization");
        } else if (outcome == "delete") {
            row.detail = tr("Deleted with your account, with its vaults");
        } else {
            continue;
        }
        rows << row;
    }
    if (preview.value("subscription").isObject()) {
        // SPEC §23: the server can't cancel an Apple subscription, so the user must, before it renews
        SummaryRow row;
        row.glyph = Typography::Icons::Calendar;
        row.title = tr("Your plan");
        row.detail = AccountPlan::deletionNote(preview.value("subscription").toObject());
        rows << row;
    }
    const int links = preview.value("share_links").toInt();
    if (links > 0) {
        SummaryRow row;
        row.glyph = Typography::Icons::Link;
        row.title = tr("%n share link(s)", "", links);
        row.detail = tr("Stop working as soon as you confirm");
        rows << row;
    }
    return rows;
}

QWidget* DeleteAccountDialog::summaryRow(const SummaryRow& data, bool first, QWidget* parent)
{
    auto* holder = new QWidget(parent);
    auto* holderLayout = new QVBoxLayout(holder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    holderLayout->setSpacing(0);
    if (!first) {
        holderLayout->addWidget(new VaultDivider(holder));
    }
    auto* row = new QWidget(holder);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 10, 0, 10);
    rowLayout->setSpacing(12);
    rowLayout->addWidget(tile(data.glyph, data.letter, 32, row), 0, Qt::AlignTop);
    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* title = themedLabel(data.title, Typography::FontRole::BodyStrong, &FluentTheme::Palette::text, row);
    title->setTextFormat(Qt::PlainText);
    auto* detail = themedLabel(data.detail, Typography::FontRole::Caption, &FluentTheme::Palette::textSecondary, row);
    detail->setTextFormat(Qt::PlainText);
    text->addWidget(title);
    text->addWidget(detail);
    rowLayout->addLayout(text, 1);
    if (data.shared) {
        auto* pill = new VaultPill(tr("Shared"), row);
        Vault::followTheme(pill, [pill] {
            const auto p = FluentTheme::palette();
            pill->setColors(p.cautionBg, p.caution);
        });
        rowLayout->addWidget(pill, 0, Qt::AlignVCenter);
    }
    holderLayout->addWidget(row);
    return holder;
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
            guard->m_message->showMessage(error, VaultInfoBar::Error, VaultInfoBar::DisableAutoHide);
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

    m_intro->setText(tr("Your account locks as soon as you confirm and is deleted for good, with its vaults, on "
                        "<b>%1</b>. Until then you can sign in and restore it.")
                         .arg(longDate(preview.value("would_erase_on").toString()).toHtmlEscaped()));

    for (auto* layout : {m_summaryLayout, m_actionsLayout}) {
        while (auto* item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    }
    const auto rows = summary(preview);
    for (int i = 0; i < rows.size(); ++i) {
        m_summaryLayout->addWidget(summaryRow(rows[i], i == 0, m_summary));
    }
    m_summaryCaption->setVisible(!rows.isEmpty());
    m_summary->setVisible(!rows.isEmpty());

    for (const auto& value : preview.value("orgs").toArray()) {
        const auto org = value.toObject();
        if (org.value("outcome").toString() != "blocked") {
            continue;
        }
        const auto orgId = org.value("id").toString();
        const auto orgName = org.value("name").toString();
        // A caution panel: who to hand the org to, or delete it
        auto* box = new VaultPanel(VaultPanel::Caution, m_actions);
        box->setObjectName(QStringLiteral("deleteAccountOrg-%1").arg(orgId));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(16, 14, 16, 14);
        boxLayout->setSpacing(10);
        auto* head = new QHBoxLayout();
        head->setSpacing(10);
        auto* warning = new QLabel(Typography::Icons::Warning, box);
        warning->setFixedWidth(20);
        Vault::followTheme(warning, [warning] {
            auto palette = warning->palette();
            palette.setColor(QPalette::WindowText, FluentTheme::palette().caution);
            warning->setPalette(palette);
            warning->setFont(glyphFont(16));
        });
        auto* headText = new QVBoxLayout();
        headText->setSpacing(2);
        auto* title = themedLabel(tr("You're the only owner of %1").arg(orgName),
                                  Typography::FontRole::BodyStrong,
                                  &FluentTheme::Palette::caution,
                                  box);
        title->setTextFormat(Qt::PlainText);
        auto* note = themedLabel(tr("Make someone else an owner, or delete the organization, first."),
                                 Typography::FontRole::Body,
                                 &FluentTheme::Palette::text,
                                 box);
        headText->addWidget(title);
        headText->addWidget(note);
        head->addWidget(warning, 0, Qt::AlignTop);
        head->addLayout(headText, 1);
        boxLayout->addLayout(head);
        auto* row = new QHBoxLayout();
        row->setContentsMargins(30, 0, 0, 0);
        row->setSpacing(8);
        auto* members = new VaultComboBox(box);
        members->setAccessibleName(tr("New owner"));
        for (const auto& m : org.value("members").toArray()) {
            const auto member = m.toObject();
            const auto role = member.value("role").toString();
            members->addItem(role == "admin" ? tr("%1 · Admin").arg(member.value("email").toString())
                                             : member.value("email").toString(),
                             member.value("id").toString());
        }
        auto* makeOwner = pillButton(tr("Make owner"), QStringLiteral("deleteAccountMakeOwner"), box);
        auto* deleteOrg =
            pillButton(tr("Delete the organization instead…"), QStringLiteral("deleteAccountDeleteOrg"), box);
        deleteOrg->setFluentStyle(fluent::basicinput::Button::Subtle);
        row->addWidget(members, 1);
        row->addWidget(makeOwner);
        boxLayout->addLayout(row);
        auto* deleteRow = new QHBoxLayout();
        deleteRow->setContentsMargins(22, 0, 0, 0);
        deleteRow->addWidget(deleteOrg);
        deleteRow->addStretch();
        boxLayout->addLayout(deleteRow);
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
                    guard->m_message->showMessage(error, VaultInfoBar::Error);
                    return;
                }
                guard->reload();
            };
            admin->listMembers(
                orgId,
                [admin, orgId, userId, finished](const QList<ServiceAccountAdmin::Member>& list, const QString& error) {
                    for (const auto& member : list) {
                        if (member.userId == userId) {
                            admin->setMemberRole(orgId, member, "owner", finished);
                            return;
                        }
                    }
                    finished(false,
                             error.isEmpty() ? DeleteAccountDialog::tr("That member has left the organization.")
                                             : error);
                });
        });
        connect(deleteOrg, &QPushButton::clicked, this, [this, orgId, orgName] {
            const auto answer = MessageBox::question(this,
                                                     tr("Delete %1?").arg(orgName),
                                                     tr("Its vaults and service accounts are deleted now, and its "
                                                        "members lose access. This can't be undone."),
                                                     MessageBox::Delete | MessageBox::Cancel,
                                                     MessageBox::Cancel);
            if (answer != MessageBox::Delete || !m_account) {
                return;
            }
            setBusy(true);
            QPointer<DeleteAccountDialog> guard(this);
            m_account->callAccount(
                this,
                [orgId](const ov::Account& account, const ov::CancelToken& cancel) {
                    account.delete_org(rs(orgId), cancel);
                },
                [guard](const AccountManager::CoreError& error) {
                    if (!guard) {
                        return;
                    }
                    guard->setBusy(false);
                    if (!error.ok()) {
                        guard->m_message->showMessage(guard->m_account ? guard->m_account->errorMessage(error)
                                                                       : error.message,
                                                      VaultInfoBar::Error);
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
        // An inset panel: who to give the shared vault to, so it isn't deleted with the account
        auto* box = new VaultPanel(VaultPanel::Inset, m_actions);
        box->setObjectName(QStringLiteral("deleteAccountVault-%1").arg(vaultId));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(16, 14, 16, 14);
        boxLayout->setSpacing(8);
        auto* title = themedLabel(vault.value("name").toString(tr("Shared vault")),
                                  Typography::FontRole::BodyStrong,
                                  &FluentTheme::Palette::text,
                                  box);
        title->setTextFormat(Qt::PlainText);
        auto* note = themedLabel(tr("Shared vaults are deleted with your account. Give it to someone who has it, so "
                                    "they keep it."),
                                 Typography::FontRole::Body,
                                 &FluentTheme::Palette::textSecondary,
                                 box);
        boxLayout->addWidget(title);
        boxLayout->addWidget(note);
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* recipients = new VaultComboBox(box);
        recipients->setAccessibleName(tr("Give it to"));
        for (const auto& person : people) {
            recipients->addItem(person.value("display").toString(person.value("principal").toString()),
                                person.value("principal").toString());
        }
        auto* give = pillButton(tr("Give vault"), QStringLiteral("deleteAccountGiveVault"), box);
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
                    guard->m_message->showMessage(error, VaultInfoBar::Error);
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
    const bool emailMatches =
        m_account && m_confirm->text().trimmed().compare(m_account->email(), Qt::CaseInsensitive) == 0;
    const bool confirmed = emailMatches && !m_password->text().isEmpty();
    const bool blocked = m_preview.value("blocked").toBool();
    m_delete->setEnabled(!m_busy && !m_preview.isEmpty() && !blocked && confirmed);
    // Says what's still missing, beside the buttons
    QString hint;
    if (m_preview.isEmpty()) {
        hint.clear();
    } else if (blocked) {
        hint = tr("Sort out the organization above first");
    } else if (!emailMatches && m_account) {
        hint = tr("Type %1 to confirm").arg(m_account->email());
    } else if (m_password->text().isEmpty()) {
        hint = tr("Enter your master password");
    }
    m_footHint->setText(hint);
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
                guard->m_message->showMessage(error, VaultInfoBar::Error, VaultInfoBar::DisableAutoHide);
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
    , m_message(new VaultInfoBar(this))
    , m_intro(new QLabel(this))
    , m_month(nullptr)
    , m_day(nullptr)
    , m_calendar(new QWidget(this))
    , m_dateTitle(new QLabel(this))
    , m_dateDetail(new QLabel(this))
    , m_passwordLabel(new QLabel(tr("Master password"), this))
    , m_password(new VaultLineEdit(this))
    , m_buttonRow(new QWidget(this))
    , m_signOut(nullptr)
    , m_restore(nullptr)
{
    setObjectName("pendingDeletionDialog");
    setWindowTitle(tr("Scheduled for deletion"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    layout->setSpacing(16);

    const auto email = m_account ? m_account->email() : QString();
    const auto iso = m_account ? m_account->deletionScheduledFor() : QString();
    const auto at = QDateTime::fromString(iso, Qt::ISODate).toLocalTime();

    m_intro->setText(tr("%1 is locked until then.").arg(email));
    m_intro->setTextFormat(Qt::PlainText);
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);
    m_message->setHidden(true);
    layout->addWidget(m_message);

    // The date on a calendar tile, then how long is left
    auto* datePanel = new VaultPanel(VaultPanel::Inset, this);
    datePanel->setObjectName("pendingDeletionDate");
    auto* dateLayout = new QHBoxLayout(datePanel);
    dateLayout->setContentsMargins(18, 14, 18, 14);
    dateLayout->setSpacing(16);
    m_calendar->setParent(datePanel);
    m_calendar->setObjectName("pendingDeletionCalendar");
    m_calendar->setAttribute(Qt::WA_StyledBackground);
    m_calendar->setFixedSize(56, 60);
    auto* calendarLayout = new QVBoxLayout(m_calendar);
    calendarLayout->setContentsMargins(0, 0, 0, 0);
    calendarLayout->setSpacing(0);
    m_month = new QLabel(
        at.isValid() ? QLocale().monthName(at.date().month(), QLocale::ShortFormat).toUpper() : QString(), m_calendar);
    m_month->setAlignment(Qt::AlignCenter);
    m_month->setFixedHeight(20);
    m_day = new QLabel(at.isValid() ? QString::number(at.date().day()) : QStringLiteral("–"), m_calendar);
    m_day->setAlignment(Qt::AlignCenter);
    calendarLayout->addWidget(m_month);
    calendarLayout->addWidget(m_day, 1);
    auto* dateText = new QVBoxLayout();
    dateText->setSpacing(2);
    m_dateTitle->setText(tr("Deleted for good on %1").arg(longDate(iso)));
    m_dateTitle->setWordWrap(true);
    const qint64 days = at.isValid() ? QDate::currentDate().daysTo(at.date()) : -1;
    m_dateDetail->setText(days >= 0 ? tr("With its vaults · %n day(s) left to change your mind", "", int(days))
                                    : tr("With its vaults"));
    m_dateDetail->setWordWrap(true);
    dateText->addWidget(m_dateTitle);
    dateText->addWidget(m_dateDetail);
    dateLayout->addWidget(m_calendar);
    dateLayout->addLayout(dateText, 1);
    layout->addWidget(datePanel);

    // What restoring brings back and what it doesn't
    const QList<QPair<bool, QString>> points = {
        {true, tr("Restore it and everything comes back. Plans that haven't ended start renewing again.")},
        {false, tr("Share links you had don't come back.")}};
    for (const auto& point : points) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* icon = new QLabel(point.first ? Typography::Icons::CheckMark : Typography::Icons::Cancel, this);
        icon->setFixedWidth(16);
        icon->setProperty("positive", point.first);
        auto* text = new QLabel(point.second, this);
        text->setWordWrap(true);
        row->addWidget(icon, 0, Qt::AlignTop);
        row->addWidget(text, 1);
        layout->addLayout(row);
        m_pointIcons << icon;
        m_points << text;
    }

    auto* passwordBlock = new QVBoxLayout();
    passwordBlock->setSpacing(6);
    m_passwordLabel->setBuddy(m_password);
    m_password->setObjectName("pendingDeletionPassword");
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(tr("Master password"));
    passwordBlock->addWidget(m_passwordLabel);
    passwordBlock->addWidget(m_password);
    layout->addLayout(passwordBlock);

    // Sign out on the left, Restore on the right
    auto* buttons = new QHBoxLayout(m_buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_signOut = pillButton(tr("Sign out"), "pendingDeletionSignOut", m_buttonRow);
    auto* restore = pillButton(tr("Restore account"), "pendingDeletionRestore", m_buttonRow);
    restore->setIconGlyph(Typography::Icons::Undo);
    restore->setFluentStyle(fluent::basicinput::Button::Accent);
    restore->setDefault(true);
    m_restore = restore;
    buttons->addWidget(m_signOut);
    buttons->addStretch();
    buttons->addWidget(m_restore);
    layout->addWidget(m_buttonRow);

    m_restore->setEnabled(false);
    connect(
        m_password, &QLineEdit::textChanged, this, [this] { m_restore->setEnabled(!m_password->text().isEmpty()); });
    connect(m_password, &QLineEdit::returnPressed, m_restore, &QPushButton::click);
    connect(m_signOut, &QPushButton::clicked, this, [this] {
        if (m_account) {
            m_account->signOut();
        }
        reject();
    });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!m_account || m_password->text().isEmpty()) {
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
                guard->m_message->showMessage(error, VaultInfoBar::Error, VaultInfoBar::DisableAutoHide);
                return;
            }
            guard->m_password->clear();
            guard->accept();
        });
    });

    Vault::followTheme(this, [this] { applyColors(); });
    setFixedWidth(PendingCardWidth);
}

PendingDeletionDialog* PendingDeletionDialog::present(AccountManager* account, QWidget* parent)
{
    auto* dialog = new PendingDeletionDialog(account, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* modal = VaultModal::forDialog(dialog, QStringLiteral("vaultPendingDeletion"), PendingCardWidth);
    if (!modal) {
        dialog->open();
        return dialog;
    }
    dialog->layout()->setContentsMargins(12, 4, 8, 4);
    modal->addFooterWidget(dialog->m_buttonRow, 1);
    modal->setInitialFocus(dialog->m_password);
    modal->open();
    return dialog;
}

void PendingDeletionDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_dateTitle, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_dateDetail, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_passwordLabel, Typography::FontRole::BodyStrong, p.text);
    for (auto* label : m_points) {
        Vault::styleLabel(label, Typography::FontRole::Body, p.textSecondary);
    }
    for (auto* icon : m_pointIcons) {
        icon->setFont(glyphFont(14));
        auto palette = icon->palette();
        palette.setColor(QPalette::WindowText, icon->property("positive").toBool() ? p.accentText : p.caution);
        icon->setPalette(palette);
    }

    // The tile: a critical band with the month over the day, on the card's layer
    m_calendar->setStyleSheet(
        QStringLiteral("QWidget#%1 { background-color: %2; border: 1px solid %3; border-radius: 12px; }")
            .arg(m_calendar->objectName(), p.layer.name(QColor::HexArgb), p.stroke.name(QColor::HexArgb)));
    m_month->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: %2; border-top-left-radius: 11px; "
                                          "border-top-right-radius: 11px; }")
                               .arg(p.critical.name(QColor::HexArgb), p.onAccent.name(QColor::HexArgb)));
    auto month = Vault::font(Typography::FontRole::Caption);
    month.setWeight(QFont::DemiBold);
    month.setLetterSpacing(QFont::PercentageSpacing, 108);
    m_month->setFont(month);
    m_day->setFont(Vault::headingFont(22));
    auto dayPalette = m_day->palette();
    dayPalette.setColor(QPalette::WindowText, p.text);
    m_day->setPalette(dayPalette);
}
