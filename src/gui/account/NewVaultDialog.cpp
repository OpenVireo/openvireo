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

#include "NewVaultDialog.h"

#include "account/AccountPlan.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultModal.h"
#include "gui/vault/VaultWidgets.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace
{
    // The card's width over the main window, as in the mockup
    constexpr int CardWidth = 520;
    // WinUI's content dialog padding, for the dialog when it opens as a window of its own
    constexpr int WindowPadding = 24;
    constexpr int TileSize = 36;

    QString roleName(const QString& role)
    {
        if (role == QLatin1String("owner")) {
            return NewVaultDialog::tr("Owner");
        }
        if (role == QLatin1String("admin")) {
            return NewVaultDialog::tr("Admin");
        }
        return {};
    }
} // namespace

/**
 * One owner the vault can belong to: a letter tile, the name with the user's role on a pill, a line about it and a
 * radio button. Clicking anywhere on the row picks it; the picked row is tinted and outlined in the accent.
 */
class NewVaultOwnerRow : public QWidget
{
public:
    NewVaultOwnerRow(const QString& id,
                     const QString& name,
                     const QString& role,
                     const QString& meta,
                     int tint,
                     QWidget* parent)
        : QWidget(parent)
        , m_id(id)
        , m_tint(tint)
        , m_tile(new VaultPill(name.left(1).toUpper(), this))
        , m_name(new QLabel(name, this))
        , m_role(role.isEmpty() ? nullptr : new VaultPill(role, this))
        , m_meta(new QLabel(meta, this))
        , m_radio(new VaultRadioButton(this))
    {
        setObjectName(QStringLiteral("newVaultOwner-%1").arg(id.isEmpty() ? QStringLiteral("personal") : id));
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(60);
        m_radio->setObjectName(objectName() + QStringLiteral("Radio"));
        m_radio->setAccessibleName(name);
        m_radio->setAccessibleDescription(meta);
        m_tile->setAlignment(Qt::AlignCenter);
        m_tile->setFixedSize(TileSize, TileSize);
        m_tile->setRadius(11);
        m_meta->setTextFormat(Qt::PlainText);
        m_name->setTextFormat(Qt::PlainText);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 10, 14, 10);
        layout->setSpacing(14);
        layout->addWidget(m_tile);
        auto* text = new QVBoxLayout();
        text->setSpacing(2);
        auto* top = new QHBoxLayout();
        top->setSpacing(8);
        top->addWidget(m_name);
        if (m_role) {
            top->addWidget(m_role);
        }
        top->addStretch();
        text->addLayout(top);
        text->addWidget(m_meta);
        layout->addLayout(text, 1);
        layout->addWidget(m_radio);
        connect(m_radio, &QAbstractButton::toggled, this, [this] { update(); });
    }

    QString id() const
    {
        return m_id;
    }
    QAbstractButton* radio() const
    {
        return m_radio;
    }

    void applyColors()
    {
        const auto p = FluentTheme::palette();
        // The account in the accent, orgs alternating the info and caution tints so neighbours tell apart
        if (m_tint == 0) {
            m_tile->setColors(p.accentSubtle, p.accentText);
        } else if (m_tint % 2 == 1) {
            // The link blue over the card, as infoBg is too close to the card's white to show a tile
            auto fill = p.link;
            fill.setAlphaF(0.14);
            m_tile->setColors(fill, p.link);
        } else {
            m_tile->setColors(p.cautionBg, p.caution);
        }
        auto tileFont = Vault::headingFont(14);
        m_tile->setFont(tileFont);
        Vault::styleLabel(m_name, Typography::FontRole::BodyStrong, p.text);
        Vault::styleLabel(m_meta, Typography::FontRole::Caption, p.textSecondary);
        if (m_role) {
            m_role->setColors(p.stroke, p.textSecondary);
            auto roleFont = Vault::font(Typography::FontRole::Caption);
            roleFont.setWeight(QFont::DemiBold);
            m_role->setFont(roleFont);
        }
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_radio->setChecked(true);
            m_radio->setFocus(Qt::MouseFocusReason);
        }
        QWidget::mousePressEvent(event);
    }

    void enterEvent(QEnterEvent* event) override
    {
        m_hovered = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        m_hovered = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        const auto p = FluentTheme::palette();
        const bool selected = m_radio->isChecked();
        if (!selected && !m_hovered) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath path;
        path.addRoundedRect(r, VaultMetrics::RowRadius, VaultMetrics::RowRadius);
        if (selected) {
            auto fill = p.accentSubtle;
            fill.setAlphaF(fill.alphaF() * 0.6);
            painter.fillPath(path, fill);
            painter.setPen(QPen(p.accent, 2));
            painter.drawPath(path);
        } else {
            painter.fillPath(path, p.subtleHover);
        }
    }

private:
    QString m_id;
    int m_tint;
    VaultPill* m_tile;
    QLabel* m_name;
    VaultPill* m_role;
    QLabel* m_meta;
    VaultRadioButton* m_radio;
    bool m_hovered = false;
};

NewVaultDialog::NewVaultDialog(AccountManager* account, const QList<AccountManager::Org>& orgs, QWidget* parent)
    : QDialog(parent)
    , m_orgs(orgs)
    , m_intro(new QLabel(this))
    , m_nameCaption(new QLabel(tr("Name"), this))
    , m_nameEdit(new VaultLineEdit(this))
    , m_nameNote(new QLabel(this))
    , m_ownerCaption(new QLabel(tr("Belongs to"), this))
    , m_ownerPanel(new VaultPanel(VaultPanel::Layer, this))
    , m_ownerGroup(new QButtonGroup(this))
    , m_whoPanel(new VaultPanel(VaultPanel::Inset, this))
    , m_whoIcon(new QLabel(this))
    , m_whoTitle(new QLabel(this))
    , m_whoBody(new QLabel(this))
    , m_buttonRow(new QWidget(this))
    , m_billedTo(new QLabel(m_buttonRow))
    , m_cancelButton(nullptr)
    , m_createButton(nullptr)
{
    setObjectName("newAccountVaultDialog");
    setWindowTitle(tr("New vault"));

    if (account) {
        for (const auto& vault : account->keyring().vaults()) {
            m_vaultNames[vault.orgId] << vault.name.trimmed().toLower();
        }
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(WindowPadding, WindowPadding, WindowPadding, WindowPadding);
    layout->setSpacing(16);

    m_intro->setText(tr("A vault keeps its own set of items, synced and encrypted with its own key."));
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    // Name, with a note under it when it's taken already
    auto* nameBlock = new QVBoxLayout();
    nameBlock->setSpacing(6);
    m_nameCaption->setBuddy(m_nameEdit);
    m_nameEdit->setObjectName("newVaultNameEdit");
    m_nameEdit->setAccessibleName(tr("Name"));
    m_nameEdit->setPlaceholderText(tr("e.g. Work, Shared Wi-Fi, Banking"));
    m_nameEdit->setText(orgs.isEmpty() ? tr("Personal") : QString());
    m_nameEdit->selectAll();
    m_nameNote->setObjectName("newVaultNameNote");
    m_nameNote->setWordWrap(true);
    m_nameNote->setTextFormat(Qt::PlainText);
    m_nameNote->hide();
    nameBlock->addWidget(m_nameCaption);
    nameBlock->addWidget(m_nameEdit);
    nameBlock->addWidget(m_nameNote);
    layout->addLayout(nameBlock);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &NewVaultDialog::refresh);

    // Who it belongs to: the account, then each org
    auto* ownerBlock = new QVBoxLayout();
    ownerBlock->setSpacing(6);
    m_ownerPanel->setObjectName("newVaultOwners");
    auto* ownerLayout = new QVBoxLayout(m_ownerPanel);
    ownerLayout->setContentsMargins(4, 4, 4, 4);
    ownerLayout->setSpacing(2);
    auto addOwner = [&](const QString& id, const QString& name, const QString& role, const QString& meta) {
        auto* row = new NewVaultOwnerRow(id, name, role, meta, m_ownerRows.size(), m_ownerPanel);
        m_ownerGroup->addButton(row->radio());
        m_ownerRows << row;
        ownerLayout->addWidget(row);
        connect(row->radio(), &QAbstractButton::toggled, this, [this](bool checked) {
            if (checked) {
                refresh();
            }
        });
    };
    addOwner({}, tr("Personal"), {}, tr("Only you"));
    for (const auto& org : orgs) {
        const auto plan = org.plan.isKnown() ? AccountPlan::displayName(org.plan.id) : QString();
        const auto meta = plan.isEmpty() ? tr("Organization") : tr("%1 plan").arg(plan);
        addOwner(org.id, org.name, roleName(org.role), meta);
    }
    ownerBlock->addWidget(m_ownerCaption);
    ownerBlock->addWidget(m_ownerPanel);
    layout->addLayout(ownerBlock);
    // An org's vault is what this dialog is mostly for once the user runs one, so the first org starts picked
    m_ownerRows.value(orgs.isEmpty() ? 0 : 1)->radio()->setChecked(true);
    if (orgs.isEmpty()) {
        m_ownerCaption->hide();
        m_ownerPanel->hide();
    }

    // Who can open it, for the owner picked
    m_whoPanel->setObjectName("newVaultWhoPanel");
    auto* whoLayout = new QHBoxLayout(m_whoPanel);
    whoLayout->setContentsMargins(16, 14, 16, 14);
    whoLayout->setSpacing(12);
    m_whoIcon->setFixedSize(28, 28);
    m_whoIcon->setAlignment(Qt::AlignCenter);
    m_whoTitle->setObjectName("newVaultWhoTitle");
    m_whoTitle->setWordWrap(true);
    m_whoTitle->setTextFormat(Qt::PlainText);
    m_whoBody->setWordWrap(true);
    m_whoBody->setTextFormat(Qt::PlainText);
    auto* whoText = new QVBoxLayout();
    whoText->setSpacing(4);
    whoText->addWidget(m_whoTitle);
    whoText->addWidget(m_whoBody);
    whoLayout->addWidget(m_whoIcon, 0, Qt::AlignTop);
    whoLayout->addLayout(whoText, 1);
    layout->addWidget(m_whoPanel);

    // The buttons, with which org pays for the vault on the left
    auto* buttons = new QHBoxLayout(m_buttonRow);
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_billedTo->setObjectName("newVaultBilledTo");
    m_billedTo->setTextFormat(Qt::PlainText);
    buttons->addWidget(m_billedTo, 1);
    auto* cancel = new VaultButton(tr("Cancel"), m_buttonRow);
    cancel->setObjectName("newVaultCancel");
    m_cancelButton = cancel;
    auto* create = new VaultButton(tr("Create vault"), m_buttonRow);
    create->setObjectName("newVaultCreate");
    create->setIconGlyph(Typography::Icons::Add);
    create->setFluentStyle(fluent::basicinput::Button::Accent);
    create->setDefault(true);
    m_createButton = create;
    for (auto* button : {cancel, create}) {
        Vault::makePill(button);
        buttons->addWidget(button);
    }
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &NewVaultDialog::accept);
    layout->addWidget(m_buttonRow);

    Vault::followTheme(this, [this] { applyColors(); });
    refresh();
    setFixedWidth(CardWidth);
}

NewVaultDialog* NewVaultDialog::present(AccountManager* account,
                                        const QList<AccountManager::Org>& orgs,
                                        QWidget* parent,
                                        std::function<void(const QString&, const QString&)> created)
{
    auto* dialog = new NewVaultDialog(account, orgs, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, dialog, [dialog, created] {
        if (created) {
            created(dialog->name(), dialog->orgId());
        }
    });
    auto* modal = VaultModal::forDialog(dialog, QStringLiteral("vaultNewVault"), CardWidth);
    if (!modal) {
        dialog->open();
        dialog->m_nameEdit->setFocus();
        return dialog;
    }
    // The card brings the title and the close button, and its footer takes the buttons. The content lines up with
    // the card's title.
    dialog->layout()->setContentsMargins(12, 4, 8, 4);
    modal->addFooterWidget(dialog->m_buttonRow, 1);
    modal->setInitialFocus(dialog->m_nameEdit);
    modal->open();
    return dialog;
}

QString NewVaultDialog::name() const
{
    return m_nameEdit->text().trimmed();
}

QString NewVaultDialog::orgId() const
{
    for (auto* row : m_ownerRows) {
        if (row->radio()->isChecked()) {
            return row->id();
        }
    }
    return {};
}

void NewVaultDialog::setOrgId(const QString& orgId)
{
    for (auto* row : m_ownerRows) {
        if (row->id() == orgId) {
            row->radio()->setChecked(true);
        }
    }
}

void NewVaultDialog::accept()
{
    // Return in the name field lands here too, so an empty name never makes a vault
    if (name().isEmpty()) {
        m_nameEdit->setFocus();
        return;
    }
    QDialog::accept();
}

void NewVaultDialog::refresh()
{
    // Picking the first owner while the constructor builds the rows lands here before the buttons exist
    if (!m_createButton) {
        return;
    }
    const auto org = orgId();
    QString orgName;
    bool billed = false;
    for (const auto& o : m_orgs) {
        if (o.id == org) {
            orgName = o.name;
            billed = o.plan.hasBilling;
        }
    }

    const bool personal = org.isEmpty();
    m_whoIcon->setText(personal ? Typography::Icons::Lock : Typography::Icons::People);
    m_whoTitle->setText(personal ? tr("Only you can open this vault")
                                 : tr("Opens for the owners and admins of %1").arg(orgName));
    m_whoBody->setText(personal ? tr("It is sealed to your account. You can share it with someone later.")
                                : tr("Owners and admins get access right away. Share it with other members "
                                     "afterwards."));
    m_billedTo->setText(billed ? tr("Billed to %1").arg(orgName) : QString());

    // A second vault of the same name is allowed, but rarely meant
    const auto name = this->name();
    const bool taken = !name.isEmpty() && m_vaultNames.value(org).contains(name.toLower());
    m_nameNote->setText(taken ? tr("You already have a vault called “%1” in %2. You can still create it.")
                                    .arg(name, personal ? tr("your account") : orgName)
                              : QString());
    m_nameNote->setVisible(taken);
    m_createButton->setEnabled(!name.isEmpty());
}

void NewVaultDialog::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_intro, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_nameCaption, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_ownerCaption, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_nameNote, Typography::FontRole::Caption, p.caution);
    Vault::styleLabel(m_whoTitle, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_whoBody, Typography::FontRole::Body, p.textSecondary);
    Vault::styleLabel(m_billedTo, Typography::FontRole::Caption, p.textTertiary);

    QFont glyph(Typography::FontFamily::FluentIcons);
    glyph.setPixelSize(16);
    m_whoIcon->setFont(glyph);
    m_whoIcon->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-radius: 9px; color: %2; }")
                                 .arg(p.accentSubtle.name(QColor::HexArgb), p.accentText.name(QColor::HexArgb)));
    for (auto* row : m_ownerRows) {
        row->applyColors();
    }
}
