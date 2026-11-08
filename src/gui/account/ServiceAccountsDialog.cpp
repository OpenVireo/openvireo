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

#include "ServiceAccountsDialog.h"

#include "account/AccountCrypto.h"
#include "account/AccountManager.h"
#include "account/VaultKeyring.h"
#include "core/Config.h"
#include "gui/Clipboard.h"
#include "gui/MessageBox.h"
#include "gui/account/OrgAdminWidgets.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultWidgets.h"

#include <components/basicinput/Button.h>
#include <components/basicinput/ToggleSwitch.h>
#include <components/dialogs_flyouts/ContentDialog.h>
#include <components/layout/Card.h>
#include <components/status_info/InfoBar.h>
#include <components/status_info/ProgressBar.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
    constexpr int IdRole = Qt::UserRole;
    constexpr int StatusRole = Qt::UserRole + 1;
    constexpr int SidebarWidth = 232;
    constexpr int AccountListWidth = 260;
    constexpr int MessageTimeout = 6000;

    using fluent::basicinput::Button;
    using fluent::status_info::InfoBar;

    QString formatTime(const QDateTime& time, const QString& none)
    {
        return time.isValid() ? QLocale().toString(time.toLocalTime(), QLocale::ShortFormat) : none;
    }

    QString formatDate(const QDateTime& time)
    {
        return QLocale().toString(time.toLocalTime().date(), QLocale::ShortFormat);
    }

    bool isExpired(const QDateTime& expiresAt)
    {
        return expiresAt.isValid() && expiresAt < QDateTime::currentDateTimeUtc();
    }

    VaultButton* button(const QString& text, const QString& name, QWidget* parent, bool accent = false)
    {
        auto result = new VaultButton(text, parent);
        result->setObjectName(name);
        if (accent) {
            result->setFluentStyle(Button::Accent);
        }
        return result;
    }

    // A list whose rows are drawn by OrgAdmin::RowDelegate. Trees keep their other columns, hidden, for the
    // values the rows summarise.
    void styleAsList(QAbstractItemView* view)
    {
        view->setItemDelegate(new OrgAdmin::RowDelegate(view));
        view->setFrameShape(QFrame::NoFrame);
        view->setMouseTracking(true);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setAttribute(Qt::WA_MacShowFocusRect, false);
        view->viewport()->setAutoFillBackground(false);
        if (auto tree = qobject_cast<QTreeWidget*>(view)) {
            tree->setHeaderHidden(true);
            tree->setRootIsDecorated(false);
            tree->setIndentation(0);
            for (int column = 1; column < tree->columnCount(); ++column) {
                tree->setColumnHidden(column, true);
            }
        }
    }

    void styleAsTable(QTreeWidget* tree)
    {
        tree->setRootIsDecorated(false);
        tree->setUniformRowHeights(true);
        tree->setAllColumnsShowFocus(true);
        tree->setAttribute(Qt::WA_MacShowFocusRect, false);
        tree->header()->setSectionsMovable(false);
        tree->header()->setHighlightSections(false);
    }

    // A card on the service account's details, with a heading, an optional action and its content below
    QVBoxLayout* card(const QString& title, QWidget* action, QVBoxLayout* into, QWidget* parent)
    {
        auto frame = new fluent::layout::Card(parent);
        frame->setAppearance(fluent::layout::Card::Layer);
        auto layout = new QVBoxLayout(frame);
        layout->setContentsMargins(16, 14, 16, 16);
        layout->setSpacing(10);
        auto header = new QHBoxLayout();
        auto heading = OrgAdmin::label(frame, Typography::FontRole::BodyStrong);
        heading->setText(title);
        header->addWidget(heading, 1);
        if (action) {
            header->addWidget(action);
        }
        layout->addLayout(header);
        into->addWidget(frame);
        return layout;
    }

    // A caption and value side by side, as on the entry details
    QLabel* field(const QString& caption, QGridLayout* grid, QWidget* parent, QWidget* trailing = nullptr)
    {
        const int row = grid->rowCount();
        auto name = OrgAdmin::label(parent, Typography::FontRole::Body, true);
        name->setText(caption);
        name->setWordWrap(false);
        auto value = OrgAdmin::label(parent, Typography::FontRole::Body);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        grid->addWidget(name, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(value, row, 1);
        if (trailing) {
            grid->addWidget(trailing, row, 2, Qt::AlignRight | Qt::AlignVCenter);
        }
        return value;
    }

    QString policyAt(int id)
    {
        static const QStringList policies{"on", "managers", "off"};
        return policies.value(id);
    }
} // namespace

ServiceAccountsDialog::ServiceAccountsDialog(AccountManager* account, QWidget* parent)
    : QDialog(parent)
    , m_account(account)
    , m_admin(new ServiceAccountAdmin(account, this))
{
    setObjectName("serviceAccountsDialog");
    setWindowTitle(tr("Organization"));
    resize(1040, 680);

    m_infoTimer = new QTimer(this);
    m_infoTimer->setSingleShot(true);
    m_infoTimer->setInterval(MessageTimeout);
    connect(m_infoTimer, &QTimer::timeout, this, &ServiceAccountsDialog::hideMessage);

    auto outer = new QVBoxLayout(this);
    auto body = new QHBoxLayout();
    body->setSpacing(20);
    body->addWidget(buildSidebar());

    auto right = new QVBoxLayout();
    right->setSpacing(10);
    m_progress = new fluent::status_info::ProgressBar(this);
    m_progress->setObjectName("orgProgress");
    m_progress->setIsIndeterminate(true);
    m_progress->setRailVisible(false);
    m_progress->setFixedHeight(4);
    auto keepSpace = m_progress->sizePolicy();
    keepSpace.setRetainSizeWhenHidden(true);
    m_progress->setSizePolicy(keepSpace);
    m_progress->hide();
    right->addWidget(m_progress);

    m_infoBar = new InfoBar(this);
    m_infoBar->setObjectName("orgInfoBar");
    m_infoBar->setIsClosable(true);
    m_infoBar->setIsOpen(true);
    m_infoBar->setSizePolicy(QSizePolicy::Expanding, m_infoBar->sizePolicy().verticalPolicy());
    m_infoBar->hide();
    connect(m_infoBar, &InfoBar::isOpenChanged, this, [this](bool open) {
        if (!open) {
            hideMessage();
        }
    });
    right->addWidget(m_infoBar);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName("orgPages");
    m_pages->addWidget(buildMembersPage());
    m_pages->addWidget(buildInvitationsPage());
    m_pages->addWidget(buildServiceAccountsPage());
    m_pages->addWidget(buildShareLinksPage());
    m_pages->addWidget(buildAuditPage());
    m_pages->addWidget(buildEmptyPage());
    right->addWidget(m_pages, 1);

    m_statusLabel = OrgAdmin::label(this, Typography::FontRole::Caption, true);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->hide();
    right->addWidget(m_statusLabel);
    body->addLayout(right, 1);
    outer->addLayout(body, 1);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_buttonBox->setObjectName("buttonBox");
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(m_buttonBox);

    // Ctrl+1 to Ctrl+5 jump between the sections
    for (int section = Members; section <= AuditLog; ++section) {
        auto shortcut = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + section)), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, section] {
            if (!m_navigation->isHidden()) {
                setCurrentSection(static_cast<Section>(section));
            }
        });
    }

    connect(m_orgCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        config()->set(Config::GUI_OrgAdminLastOrg, currentOrg());
        m_orgRole->setText(currentRole() == "owner" ? tr("You are an owner") : tr("You are an admin"));
        m_vaultAccess.clear();
        updateShareLinks();
        loadServiceAccounts();
        loadMembers();
        loadInvitations();
        loadAudit();
    });

    const int section = qBound<int>(Members, config()->get(Config::GUI_OrgAdminLastSection).toInt(), AuditLog);
    setCurrentSection(static_cast<Section>(section));
    Vault::followTheme(this, [this] { applyTheme(); });
    showDetails();
    updateMemberButtons();
    reload();
}

ServiceAccountsDialog::~ServiceAccountsDialog() = default;

QWidget* ServiceAccountsDialog::buildSidebar()
{
    auto sidebar = new QWidget(this);
    sidebar->setObjectName("orgSidebar");
    sidebar->setFixedWidth(SidebarWidth);
    auto layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_orgCombo = new VaultComboBox(sidebar);
    m_orgCombo->setObjectName("orgCombo");
    m_orgCombo->setAccessibleName(tr("Organization"));
    layout->addWidget(m_orgCombo);
    m_orgRole = OrgAdmin::label(sidebar, Typography::FontRole::Caption, true);
    m_orgRole->setObjectName("orgRoleLabel");
    m_orgRole->setContentsMargins(4, 0, 0, 0);
    layout->addWidget(m_orgRole);
    layout->addSpacing(10);

    m_navigation = new QListWidget(sidebar);
    m_navigation->setObjectName("orgNavigation");
    m_navigation->setAccessibleName(tr("Organization sections"));
    m_navigation->setItemDelegate(new OrgAdmin::NavigationDelegate(m_navigation));
    m_navigation->setFrameShape(QFrame::NoFrame);
    m_navigation->setMouseTracking(true);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_navigation->viewport()->setAutoFillBackground(false);
    m_navigation->viewport()->setCursor(Qt::PointingHandCursor);
    const QList<QPair<QString, QString>> sections{
        {tr("Members"), Typography::Icons::People},
        {tr("Invitations"), Typography::Icons::Mail},
        {tr("Service accounts"), Typography::Icons::Desktop},
        {tr("Share links"), Typography::Icons::Link},
        {tr("Audit log"), Typography::Icons::History},
    };
    for (const auto& section : sections) {
        auto item = new QListWidgetItem(section.first, m_navigation);
        item->setData(OrgAdmin::GlyphRole, section.second);
    }
    m_navigation->setFixedHeight(sections.size() * 38 + 4);
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) {
            return;
        }
        if (!m_navigation->isHidden()) {
            m_pages->setCurrentIndex(row);
        }
        config()->set(Config::GUI_OrgAdminLastSection, row);
    });
    layout->addWidget(m_navigation);
    layout->addStretch();

    m_newOrgButton = button(tr("New organization…"), "newOrgButton", sidebar);
    m_newOrgButton->setFluentStyle(Button::Subtle);
    m_newOrgButton->setIconGlyph(Typography::Icons::Add);
    m_newOrgButton->setToolTip(tr("Create an organization with you as its owner"));
    connect(m_newOrgButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewOrg);
    layout->addWidget(m_newOrgButton, 0, Qt::AlignLeft);
    return sidebar;
}

QWidget* ServiceAccountsDialog::buildMembersPage()
{
    auto page = new QWidget(this);
    page->setObjectName("membersPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(OrgAdmin::pageHeader(
        tr("Members"),
        tr("Owners and admins get every vault of the organization; other members get the vaults shared with them. "
           "Removing a member gives the vaults they could open a new key."),
        nullptr,
        page));

    m_waitingBar = new InfoBar(page);
    m_waitingBar->setObjectName("membersWaitingBar");
    m_waitingBar->setSeverity(InfoBar::Warning);
    m_waitingBar->setIsClosable(false);
    m_waitingBar->setIsOpen(true);
    m_waitingBar->setSizePolicy(QSizePolicy::Expanding, m_waitingBar->sizePolicy().verticalPolicy());
    auto review = button(tr("Review"), "membersReviewButton", m_waitingBar);
    connect(review, &QPushButton::clicked, this, &ServiceAccountsDialog::reviewWaitingMembers);
    m_waitingBar->setActionWidget(review);
    m_waitingBar->hide();
    layout->addWidget(m_waitingBar);

    m_membersTree = new QTreeWidget(page);
    m_membersTree->setObjectName("membersTree");
    m_membersTree->setAccessibleName(tr("Members"));
    m_membersTree->setColumnCount(4);
    m_membersTree->setHeaderLabels({tr("Email"), tr("Role"), tr("Status"), tr("Fingerprint")});
    styleAsList(m_membersTree);
    m_membersTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_membersTree, &QWidget::customContextMenuRequested, this, &ServiceAccountsDialog::showMemberMenu);
    connect(m_membersTree, &QTreeWidget::itemSelectionChanged, this, &ServiceAccountsDialog::updateMemberButtons);
    connect(m_membersTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, StatusRole).toString() == "accepted") {
            askToConfirmMember();
        }
    });
    layout->addWidget(m_membersTree, 1);

    m_memberHint = OrgAdmin::label(page, Typography::FontRole::Caption, true);
    m_memberHint->setObjectName("memberHint");
    m_memberHint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_memberHint);

    auto actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_confirmMemberButton = button(tr("Confirm…"), "confirmMemberButton", page, true);
    m_changeRoleButton = button(tr("Change role…"), "changeRoleButton", page);
    m_removeMemberButton = button(tr("Remove…"), "removeMemberButton", page);
    connect(m_confirmMemberButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askToConfirmMember);
    connect(m_changeRoleButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForMemberRole);
    connect(m_removeMemberButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askToRemoveMember);
    actions->addWidget(m_confirmMemberButton);
    actions->addWidget(m_changeRoleButton);
    actions->addStretch();
    actions->addWidget(m_removeMemberButton);
    layout->addLayout(actions);
    return page;
}

QWidget* ServiceAccountsDialog::buildInvitationsPage()
{
    auto page = new QWidget(this);
    page->setObjectName("invitationsPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_inviteButton = button(tr("Invite…"), "inviteButton", page, true);
    m_inviteButton->setIconGlyph(Typography::Icons::Add);
    connect(m_inviteButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askToInvite);
    layout->addWidget(
        OrgAdmin::pageHeader(tr("Invitations"),
                             tr("Invitations go out by email and are valid for 7 days. Once someone accepts, they "
                                "show under Members, where you confirm them to give them their keys."),
                             m_inviteButton,
                             page));

    m_invitationsTree = new QTreeWidget(page);
    m_invitationsTree->setObjectName("invitationsTree");
    m_invitationsTree->setAccessibleName(tr("Pending invitations"));
    m_invitationsTree->setColumnCount(3);
    m_invitationsTree->setHeaderLabels({tr("Email"), tr("Role"), tr("Expires")});
    styleAsList(m_invitationsTree);
    connect(m_invitationsTree, &QTreeWidget::itemSelectionChanged, this, &ServiceAccountsDialog::updateMemberButtons);
    layout->addWidget(m_invitationsTree, 1);

    m_invitationsEmpty = OrgAdmin::label(page, Typography::FontRole::Body, true);
    m_invitationsEmpty->setObjectName("invitationsEmpty");
    m_invitationsEmpty->setText(tr("No pending invitations. Invite someone by their email address."));
    m_invitationsEmpty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_invitationsEmpty, 1);

    auto actions = new QHBoxLayout();
    m_cancelInvitationButton = button(tr("Cancel invitation"), "cancelInvitationButton", page);
    connect(m_cancelInvitationButton, &QPushButton::clicked, this, &ServiceAccountsDialog::cancelSelectedInvitation);
    actions->addStretch();
    actions->addWidget(m_cancelInvitationButton);
    layout->addLayout(actions);
    return page;
}

QWidget* ServiceAccountsDialog::buildServiceAccountsPage()
{
    auto page = new QWidget(this);
    page->setObjectName("serviceAccountsPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_newButton = button(tr("New service account…"), "newButton", page, true);
    m_newButton->setIconGlyph(Typography::Icons::Add);
    connect(m_newButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewServiceAccount);
    layout->addWidget(OrgAdmin::pageHeader(tr("Service accounts"),
                                           tr("A service account lets a CI job, a server or a Kubernetes cluster "
                                              "open vaults with a token, without a person's account."),
                                           m_newButton,
                                           page));

    auto split = new QHBoxLayout();
    split->setSpacing(16);
    auto listColumn = new QVBoxLayout();
    m_accountList = new QListWidget(page);
    m_accountList->setObjectName("accountList");
    m_accountList->setAccessibleName(tr("Service accounts"));
    m_accountList->setFixedWidth(AccountListWidth);
    styleAsList(m_accountList);
    connect(m_accountList, &QListWidget::currentRowChanged, this, &ServiceAccountsDialog::showDetails);
    listColumn->addWidget(m_accountList, 1);
    m_accountsEmpty = OrgAdmin::label(page, Typography::FontRole::Body, true);
    m_accountsEmpty->setObjectName("accountsEmpty");
    m_accountsEmpty->setFixedWidth(AccountListWidth);
    m_accountsEmpty->setText(tr("No service accounts yet."));
    m_accountsEmpty->hide();
    listColumn->addWidget(m_accountsEmpty);
    listColumn->addStretch();
    split->addLayout(listColumn);

    m_detailsStack = new QStackedWidget(page);
    m_detailsLabel = OrgAdmin::label(page, Typography::FontRole::Body, true);
    m_detailsLabel->setObjectName("detailsLabel");
    m_detailsLabel->setAlignment(Qt::AlignCenter);
    m_detailsLabel->setText(tr("Select a service account, or create one for a CI job, a server or a Kubernetes "
                               "cluster."));
    m_detailsStack->addWidget(m_detailsLabel);

    m_details = new QScrollArea(page);
    m_details->setObjectName("detailsWidget");
    m_details->setWidgetResizable(true);
    m_details->setFrameShape(QFrame::NoFrame);
    m_details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto details = new QWidget(m_details);
    auto cards = new QVBoxLayout(details);
    cards->setContentsMargins(0, 0, 4, 0);
    cards->setSpacing(12);

    // Overview: who it is, when it stops working and whether it works at all
    auto overview = card(tr("Overview"), nullptr, cards, details);
    m_saName = OrgAdmin::label(details, Typography::FontRole::Subtitle);
    m_saName->setObjectName("serviceAccountName");
    overview->addWidget(m_saName);
    auto grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    m_saCreated = field(tr("Created"), grid, details);
    auto copyFingerprint = Vault::iconButton(Typography::Icons::Copy, tr("Copy fingerprint"), details);
    copyFingerprint->setObjectName("copyServiceAccountFingerprint");
    m_saFingerprint = field(tr("Fingerprint"), grid, details, copyFingerprint);
    m_saFingerprint->setObjectName("serviceAccountFingerprint");
    m_saFingerprint->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(copyFingerprint, &QPushButton::clicked, this, [this] {
        clipboard()->setText(m_saFingerprint->text());
        showMessage(tr("The fingerprint is copied."), InfoBar::Success);
    });
    m_expiryButton = button(tr("Change…"), "expiryButton", details);
    connect(m_expiryButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForExpiry);
    m_saExpiry = field(tr("Expires"), grid, details, m_expiryButton);
    m_saExpiry->setObjectName("serviceAccountExpiry");
    m_enabledSwitch = new fluent::basicinput::ToggleSwitch(details);
    m_enabledSwitch->setObjectName("enabledSwitch");
    m_enabledSwitch->setAccessibleName(tr("Enabled"));
    connect(m_enabledSwitch, &fluent::basicinput::ToggleSwitch::toggled, this, [this](bool on) {
        if (on == selected().disabled) {
            toggleDisabled();
        }
    });
    m_enabledLabel = field(tr("Enabled"), grid, details, m_enabledSwitch);
    m_enabledLabel->setObjectName("serviceAccountEnabled");
    overview->addLayout(grid);

    // Secrets: each is a token of its own
    m_issueButton = button(tr("Issue secret…"), "issueButton", details);
    m_issueButton->setIconGlyph(Typography::Icons::Add);
    connect(m_issueButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewSecret);
    auto secrets = card(tr("Secrets"), m_issueButton, cards, details);
    auto secretsText = OrgAdmin::label(details, Typography::FontRole::Caption, true);
    secretsText->setText(tr("Each secret is a token of its own. Revoking one stops only what uses it."));
    secrets->addWidget(secretsText);
    m_secretsTree = new QTreeWidget(details);
    m_secretsTree->setObjectName("secretsTree");
    m_secretsTree->setAccessibleName(tr("Secrets"));
    m_secretsTree->setColumnCount(4);
    m_secretsTree->setHeaderLabels({tr("Name"), tr("Starts with"), tr("Last used"), tr("Expires")});
    m_secretsTree->setMinimumHeight(120);
    styleAsTable(m_secretsTree);
    m_secretsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(
        m_secretsTree, &QTreeWidget::itemSelectionChanged, this, &ServiceAccountsDialog::updateServiceAccountButtons);
    secrets->addWidget(m_secretsTree);
    auto secretActions = new QHBoxLayout();
    m_secretExpiryButton = button(tr("Change expiry…"), "secretExpiryButton", details);
    m_revokeButton = button(tr("Revoke…"), "revokeButton", details);
    connect(m_secretExpiryButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askForSecretExpiry);
    connect(m_revokeButton, &QPushButton::clicked, this, &ServiceAccountsDialog::revokeSelectedSecret);
    secretActions->addStretch();
    secretActions->addWidget(m_secretExpiryButton);
    secretActions->addWidget(m_revokeButton);
    secrets->addLayout(secretActions);

    // Vault access: what it can open now, and giving it more
    auto access = card(tr("Vault access"), nullptr, cards, details);
    m_vaultAccessTree = new QTreeWidget(details);
    m_vaultAccessTree->setObjectName("vaultAccessTree");
    m_vaultAccessTree->setAccessibleName(tr("Vault access"));
    m_vaultAccessTree->setColumnCount(2);
    m_vaultAccessTree->setHeaderLabels({tr("Vault"), tr("Access")});
    styleAsList(m_vaultAccessTree);
    m_vaultAccessTree->setFixedHeight(130);
    connect(m_vaultAccessTree,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ServiceAccountsDialog::updateServiceAccountButtons);
    access->addWidget(m_vaultAccessTree);
    m_vaultAccessEmpty = OrgAdmin::label(details, Typography::FontRole::Body, true);
    m_vaultAccessEmpty->setObjectName("vaultAccessEmpty");
    m_vaultAccessEmpty->setText(tr("It can't open any vault yet."));
    access->addWidget(m_vaultAccessEmpty);
    auto accessActions = new QHBoxLayout();
    m_revokeVaultButton = button(tr("Remove access…"), "revokeVaultButton", details);
    connect(m_revokeVaultButton, &QPushButton::clicked, this, &ServiceAccountsDialog::askToRevokeVault);
    accessActions->addStretch();
    accessActions->addWidget(m_revokeVaultButton);
    access->addLayout(accessActions);
    auto grant = new QHBoxLayout();
    grant->setSpacing(8);
    auto grantLabel = OrgAdmin::label(details, Typography::FontRole::Body);
    grantLabel->setObjectName("vaultAccessLabel");
    grantLabel->setText(tr("Give access to"));
    grantLabel->setWordWrap(false);
    m_vaultCombo = new VaultComboBox(details);
    m_vaultCombo->setObjectName("vaultCombo");
    m_vaultCombo->setAccessibleName(tr("Vault to give access to"));
    m_vaultRoleCombo = new VaultComboBox(details);
    m_vaultRoleCombo->setObjectName("vaultRoleCombo");
    m_vaultRoleCombo->setAccessibleName(tr("Access"));
    m_vaultRoleCombo->addItem(tr("Read only"), "read");
    m_vaultRoleCombo->addItem(tr("Can edit"), "write");
    m_grantButton = button(tr("Grant"), "grantButton", details);
    connect(m_grantButton, &QPushButton::clicked, this, &ServiceAccountsDialog::grantSelectedVault);
    grant->addWidget(grantLabel);
    grant->addWidget(m_vaultCombo, 1);
    grant->addWidget(m_vaultRoleCombo);
    grant->addWidget(m_grantButton);
    access->addLayout(grant);

    // Deleting is apart from everything else
    auto danger = card(tr("Delete"), nullptr, cards, details);
    auto dangerRow = new QHBoxLayout();
    auto dangerText = OrgAdmin::label(details, Typography::FontRole::Caption, true);
    dangerText->setText(tr("Its secrets stop working at once and it loses access to every vault."));
    m_deleteButton = button(tr("Delete service account…"), "deleteButton", details);
    connect(m_deleteButton, &QPushButton::clicked, this, &ServiceAccountsDialog::deleteSelected);
    dangerRow->addWidget(dangerText, 1);
    dangerRow->addWidget(m_deleteButton);
    danger->addLayout(dangerRow);
    cards->addStretch();

    m_details->setWidget(details);
    m_detailsStack->addWidget(m_details);
    split->addWidget(m_detailsStack, 1);
    layout->addLayout(split, 1);
    return page;
}

QWidget* ServiceAccountsDialog::buildShareLinksPage()
{
    auto page = new QWidget(this);
    page->setObjectName("shareLinksPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addWidget(OrgAdmin::pageHeader(tr("Share links"),
                                           tr("A share link opens a copy of one entry in any browser, without an "
                                              "account. Choose who can make them from this organization's vaults."),
                                           nullptr,
                                           page));
    auto heading = OrgAdmin::label(page, Typography::FontRole::BodyStrong);
    heading->setText(tr("Who can make share links"));
    layout->addWidget(heading);
    m_shareLinks = new QButtonGroup(this);
    const QStringList options{tr("Everyone in the organization"), tr("Only owners and admins"), tr("Nobody")};
    const QStringList names{"shareLinksOn", "shareLinksManagers", "shareLinksOff"};
    for (int i = 0; i < options.size(); ++i) {
        auto radio = new VaultRadioButton(options[i], page);
        radio->setObjectName(names[i]);
        m_shareLinks->addButton(radio, i);
        layout->addWidget(radio);
    }
    connect(m_shareLinks, &QButtonGroup::idClicked, this, &ServiceAccountsDialog::askForShareLinkPolicy);
    m_shareLinksHint = OrgAdmin::label(page, Typography::FontRole::Caption, true);
    m_shareLinksHint->setText(tr("Every link that is made or opened shows in the audit log. Narrowing this "
                                 "ends the links it no longer allows at once."));
    layout->addWidget(m_shareLinksHint);
    layout->addStretch();
    return page;
}

QWidget* ServiceAccountsDialog::buildAuditPage()
{
    auto page = new QWidget(this);
    page->setObjectName("auditPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_auditFilter = new VaultLineEdit(page);
    m_auditFilter->setObjectName("auditFilter");
    m_auditFilter->setPlaceholderText(tr("Filter by person, action or target"));
    m_auditFilter->setClearButtonEnabled(true);
    m_auditFilter->setFixedWidth(280);
    connect(m_auditFilter, &QLineEdit::textChanged, this, &ServiceAccountsDialog::filterAudit);
    layout->addWidget(OrgAdmin::pageHeader(
        tr("Audit log"), tr("What happened in the organization and who did it, newest first."), m_auditFilter, page));

    m_auditTree = new QTreeWidget(page);
    m_auditTree->setObjectName("auditTree");
    m_auditTree->setAccessibleName(tr("Audit log"));
    m_auditTree->setColumnCount(5);
    m_auditTree->setHeaderLabels({tr("Time"), tr("Who"), tr("Action"), tr("Target"), tr("IP address")});
    styleAsTable(m_auditTree);
    m_auditTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_auditTree->header()->resizeSection(0, 150);
    m_auditTree->header()->resizeSection(1, 190);
    layout->addWidget(m_auditTree, 1);
    m_auditEmpty = OrgAdmin::label(page, Typography::FontRole::Body, true);
    m_auditEmpty->setObjectName("auditEmpty");
    m_auditEmpty->setAlignment(Qt::AlignCenter);
    m_auditEmpty->hide();
    layout->addWidget(m_auditEmpty);

    m_loadMoreButton = button(tr("Load more"), "loadMoreButton", page);
    m_loadMoreButton->setFluentStyle(Button::Subtle);
    m_loadMoreButton->setEnabled(false);
    connect(m_loadMoreButton, &QPushButton::clicked, this, [this] { loadAudit(true); });
    layout->addWidget(m_loadMoreButton, 0, Qt::AlignHCenter);
    return page;
}

QWidget* ServiceAccountsDialog::buildEmptyPage()
{
    m_emptyPage = new QWidget(this);
    m_emptyPage->setObjectName("orgEmptyPage");
    auto layout = new QVBoxLayout(m_emptyPage);
    layout->addStretch();
    auto title = OrgAdmin::label(m_emptyPage, Typography::FontRole::Subtitle);
    title->setText(tr("Create an organization"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    m_emptyText = OrgAdmin::label(m_emptyPage, Typography::FontRole::Body, true);
    m_emptyText->setObjectName("orgEmptyText");
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setMaximumWidth(460);
    layout->addWidget(m_emptyText, 0, Qt::AlignHCenter);
    layout->addSpacing(12);
    auto create = button(tr("Create organization…"), "createOrgButton", m_emptyPage, true);
    connect(create, &QPushButton::clicked, this, &ServiceAccountsDialog::askForNewOrg);
    layout->addWidget(create, 0, Qt::AlignHCenter);
    auto invited = OrgAdmin::label(m_emptyPage, Typography::FontRole::Caption, true);
    invited->setText(tr("Were you invited to one? Open the link in the email, or use Account › Accept Invitation."));
    invited->setAlignment(Qt::AlignCenter);
    layout->addSpacing(8);
    layout->addWidget(invited);
    layout->addStretch();
    return m_emptyPage;
}

void ServiceAccountsDialog::applyTheme()
{
    for (auto view : {static_cast<QAbstractItemView*>(m_navigation),
                      static_cast<QAbstractItemView*>(m_membersTree),
                      static_cast<QAbstractItemView*>(m_invitationsTree),
                      static_cast<QAbstractItemView*>(m_accountList),
                      static_cast<QAbstractItemView*>(m_vaultAccessTree)}) {
        view->setStyleSheet(OrgAdmin::listStyleSheet());
    }
    for (auto table : {m_secretsTree, m_auditTree}) {
        table->setStyleSheet(OrgAdmin::tableStyleSheet());
    }
    m_details->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    m_details->viewport()->setAutoFillBackground(false);
    m_details->widget()->setAutoFillBackground(false);
}

void ServiceAccountsDialog::setEmbedded(bool embedded)
{
    m_embedded = embedded;
    m_buttonBox->setVisible(!embedded);
    if (embedded) {
        setWindowFlags(Qt::Widget);
        layout()->setContentsMargins(0, 0, 0, 0);
    }
}

QString ServiceAccountsDialog::currentOrg() const
{
    return m_orgCombo->currentData().toString();
}

QString ServiceAccountsDialog::currentRole() const
{
    for (const auto& org : m_orgs) {
        if (org.id == currentOrg()) {
            return org.role;
        }
    }
    return {};
}

ServiceAccountsDialog::Section ServiceAccountsDialog::currentSection() const
{
    return static_cast<Section>(qMax(0, m_navigation->currentRow()));
}

void ServiceAccountsDialog::setCurrentSection(Section section)
{
    m_navigation->setCurrentRow(section);
    if (!m_navigation->isHidden()) {
        m_pages->setCurrentIndex(section);
    }
}

QListWidget* ServiceAccountsDialog::navigation() const
{
    return m_navigation;
}

int ServiceAccountsDialog::waitingCount() const
{
    return static_cast<int>(std::count_if(
        m_members.cbegin(), m_members.cend(), [](const auto& member) { return member.status == "accepted"; }));
}

QHash<QString, QString> ServiceAccountsDialog::vaultAccess() const
{
    return m_vaultAccess;
}

QString ServiceAccountsDialog::shareLinkPolicy() const
{
    for (const auto& org : m_orgs) {
        if (org.id == currentOrg()) {
            return org.shareLinks;
        }
    }
    return {};
}

QString ServiceAccountsDialog::roleName(const QString& role)
{
    if (role == "owner") {
        return tr("Owner");
    }
    if (role == "admin") {
        return tr("Admin");
    }
    if (role == "member") {
        return tr("Member");
    }
    return role;
}

QString ServiceAccountsDialog::vaultRoleName(const QString& role)
{
    if (role == "read") {
        return tr("Read only");
    }
    if (role == "write") {
        return tr("Can edit");
    }
    if (role == "manage") {
        return tr("Can manage");
    }
    return role;
}

QString ServiceAccountsDialog::auditActionName(const QString& action)
{
    static const QHash<QString, const char*> names{
        {"org.create", QT_TR_NOOP("Organization created")},
        {"org.rename", QT_TR_NOOP("Organization renamed")},
        {"org.delete", QT_TR_NOOP("Organization deleted")},
        {"org.share_links", QT_TR_NOOP("Share link setting changed")},
        {"org.member.invite", QT_TR_NOOP("Member invited")},
        {"org.member.accept", QT_TR_NOOP("Invitation accepted")},
        {"org.member.confirm", QT_TR_NOOP("Member confirmed")},
        {"org.member.role", QT_TR_NOOP("Member's role changed")},
        {"org.member.remove", QT_TR_NOOP("Member removed")},
        {"org.invitation.cancel", QT_TR_NOOP("Invitation cancelled")},
        {"sa.create", QT_TR_NOOP("Service account created")},
        {"sa.update", QT_TR_NOOP("Service account changed")},
        {"sa.delete", QT_TR_NOOP("Service account deleted")},
        {"sa.auth", QT_TR_NOOP("Service account signed in")},
        {"sa.secret.issue", QT_TR_NOOP("Secret issued")},
        {"sa.secret.update", QT_TR_NOOP("Secret's expiry changed")},
        {"sa.secret.revoke", QT_TR_NOOP("Secret revoked")},
        {"vault.create", QT_TR_NOOP("Vault created")},
        {"vault.rename", QT_TR_NOOP("Vault renamed")},
        {"vault.delete", QT_TR_NOOP("Vault deleted")},
        {"vault.access.grant", QT_TR_NOOP("Vault access given")},
        {"vault.access.revoke", QT_TR_NOOP("Vault access removed")},
        {"vault.key.rotate", QT_TR_NOOP("Vault key rotation started")},
        {"vault.key.activate", QT_TR_NOOP("Vault got a new key")},
        {"share.create", QT_TR_NOOP("Share link made")},
        {"share.open", QT_TR_NOOP("Share link opened")},
        {"share.revoke", QT_TR_NOOP("Share link revoked")},
        {"user.login", QT_TR_NOOP("Signed in")},
        {"user.settings", QT_TR_NOOP("Account settings changed")},
    };
    const auto name = names.value(action);
    return name ? tr(name) : action;
}

ServiceAccountAdmin::Member ServiceAccountsDialog::member(const QString& userId) const
{
    for (const auto& candidate : m_members) {
        if (candidate.userId == userId) {
            return candidate;
        }
    }
    return {};
}

QString ServiceAccountsDialog::selectedMember() const
{
    const auto items = m_membersTree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(0, IdRole).toString();
}

bool ServiceAccountsDialog::isSelf(const ServiceAccountAdmin::Member& member) const
{
    return m_account && !member.userId.isEmpty() && member.principal() == m_account->principal();
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
    const auto row = m_accountList->currentRow();
    return row >= 0 && row < m_accounts.size() ? m_accounts.at(row) : ServiceAccountAdmin::ServiceAccount();
}

void ServiceAccountsDialog::setBusy(const QString& message)
{
    m_busy = !message.isEmpty();
    m_statusLabel->setText(message);
    m_statusLabel->setVisible(m_busy);
    m_progress->setVisible(m_busy);
    m_newOrgButton->setEnabled(!m_busy);
    for (auto radio : m_shareLinks->buttons()) {
        radio->setEnabled(!m_busy);
    }
    updateServiceAccountButtons();
    updateMemberButtons();
}

void ServiceAccountsDialog::showMessage(const QString& message, int severity, bool autoHide)
{
    m_infoBar->setSeverity(static_cast<InfoBar::InfoBarSeverity>(severity));
    // Long errors and warnings wrap instead of being cut off; short ones stay a slim bar
    const bool wrap = message.size() > 90;
    m_infoBar->setSingleLine(!wrap);
    m_infoBar->setMultiLineMinHeight(0);
    m_infoBar->setMessage(message);
    m_infoBar->setIsOpen(true);
    m_infoBar->show();
    if (autoHide) {
        m_infoTimer->start();
    } else {
        m_infoTimer->stop();
    }
}

void ServiceAccountsDialog::showError(const QString& error)
{
    setBusy({});
    showMessage(error, InfoBar::Error, false);
}

void ServiceAccountsDialog::hideMessage()
{
    m_infoTimer->stop();
    m_infoBar->hide();
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
        guard->m_orgs = orgs;
        guard->setBusy({});
        guard->showOrgs();
    });
}

void ServiceAccountsDialog::showOrgs()
{
    const QSignalBlocker blocker(m_orgCombo);
    auto keep = m_selectOrg.isEmpty() ? currentOrg() : m_selectOrg;
    if (keep.isEmpty()) {
        keep = config()->get(Config::GUI_OrgAdminLastOrg).toString();
    }
    m_selectOrg.clear();
    m_orgCombo->clear();
    QStringList memberOf;
    for (const auto& org : m_orgs) {
        if (org.canAdminister()) {
            m_orgCombo->addItem(org.name, org.id);
        } else {
            memberOf << org.name;
        }
    }

    const bool empty = m_orgCombo->count() == 0;
    m_orgCombo->setVisible(!empty);
    m_orgRole->setVisible(!empty);
    m_navigation->setVisible(!empty);
    if (empty) {
        // Nothing to administer: say how to get an organization instead of showing empty sections
        m_emptyText->setText(
            memberOf.isEmpty()
                ? tr("An organization lets a team share vaults, invite people and give CI jobs and servers access "
                     "with service accounts. You become its owner.")
                : tr("You're a member of %1, but only owners and admins manage an organization. Create one of "
                     "your own, or ask an owner to make you an admin.")
                      .arg(QLocale().createSeparatedList(memberOf)));
        m_pages->setCurrentWidget(m_emptyPage);
        return;
    }
    m_pages->setCurrentIndex(qMax(0, m_navigation->currentRow()));
    m_orgCombo->setCurrentIndex(qMax(0, m_orgCombo->findData(keep)));
    config()->set(Config::GUI_OrgAdminLastOrg, currentOrg());
    m_orgRole->setText(currentRole() == "owner" ? tr("You are an owner") : tr("You are an admin"));
    m_vaultAccess.clear();
    updateShareLinks();
    loadServiceAccounts();
    loadMembers();
    loadInvitations();
    loadAudit();
}

void ServiceAccountsDialog::updateNavigation()
{
    const int waiting = waitingCount();
    auto item = m_navigation->item(Members);
    item->setData(OrgAdmin::CountRole, waiting);
    item->setData(Qt::AccessibleTextRole,
                  waiting > 0 ? tr("Members, %n waiting to be confirmed", nullptr, waiting) : tr("Members"));
    m_navigation->viewport()->update();
}

void ServiceAccountsDialog::loadServiceAccounts(const QString& select)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    const auto keep = select.isEmpty() ? currentServiceAccount() : select;
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->list(orgId,
                  [guard, keep](const QList<ServiceAccountAdmin::ServiceAccount>& accounts, const QString& error) {
                      if (!guard) {
                          return;
                      }
                      if (!error.isEmpty()) {
                          guard->showError(error);
                          return;
                      }
                      guard->m_accounts = accounts;
                      auto* list = guard->m_accountList;
                      const QSignalBlocker blocker(list);
                      list->clear();
                      int row = accounts.isEmpty() ? -1 : 0;
                      for (int i = 0; i < accounts.size(); ++i) {
                          const auto& account = accounts.at(i);
                          auto* item = new QListWidgetItem(account.name, list);
                          item->setData(IdRole, account.id);
                          QString expiry;
                          if (!account.expiresAt.isValid()) {
                              expiry = tr("Never expires");
                          } else if (isExpired(account.expiresAt)) {
                              expiry = tr("Expired %1").arg(formatDate(account.expiresAt));
                          } else {
                              expiry = tr("Expires %1").arg(formatDate(account.expiresAt));
                          }
                          item->setData(OrgAdmin::SecondaryRole, expiry);
                          if (account.disabled) {
                              item->setData(OrgAdmin::BadgeRole, tr("Disabled"));
                              item->setData(OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Neutral));
                          } else if (isExpired(account.expiresAt)) {
                              item->setData(OrgAdmin::BadgeRole, tr("Expired"));
                              item->setData(OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Critical));
                          }
                          item->setData(Qt::AccessibleTextRole,
                                        account.disabled ? tr("%1, disabled, %2").arg(account.name, expiry)
                                                         : tr("%1, %2").arg(account.name, expiry));
                          if (account.id == keep) {
                              row = i;
                          }
                      }
                      list->setVisible(!accounts.isEmpty());
                      guard->m_accountsEmpty->setVisible(accounts.isEmpty());
                      list->setCurrentRow(row);
                      guard->showDetails();
                      guard->showAudit();
                  });
}

void ServiceAccountsDialog::updateServiceAccountButtons()
{
    const auto account = selected();
    const bool enabled = !account.id.isEmpty() && !m_busy;
    const bool secretSelected = !m_secretsTree->selectedItems().isEmpty();
    m_newButton->setEnabled(!m_busy && !currentOrg().isEmpty());
    m_expiryButton->setEnabled(enabled);
    m_enabledSwitch->setEnabled(enabled);
    m_issueButton->setEnabled(enabled);
    m_deleteButton->setEnabled(enabled);
    m_revokeButton->setEnabled(enabled && secretSelected);
    m_secretExpiryButton->setEnabled(enabled && secretSelected);
    const auto pickSecret = enabled && !secretSelected ? tr("Select a secret first") : QString();
    m_revokeButton->setToolTip(pickSecret);
    m_secretExpiryButton->setToolTip(pickSecret);
    m_revokeVaultButton->setEnabled(enabled && !m_vaultAccessTree->selectedItems().isEmpty());
    m_revokeVaultButton->setToolTip(enabled && m_vaultAccessTree->selectedItems().isEmpty() ? tr("Select a vault first")
                                                                                            : QString());
    m_grantButton->setEnabled(enabled && m_vaultCombo->count() > 0);
    m_grantButton->setToolTip(enabled && m_vaultCombo->count() == 0 ? tr("It can already open every vault you manage")
                                                                    : QString());
}

void ServiceAccountsDialog::showDetails()
{
    const auto account = selected();
    const bool has = !account.id.isEmpty();
    m_detailsStack->setCurrentIndex(has ? 1 : 0);
    updateServiceAccountButtons();
    if (!has) {
        m_secretsTree->clear();
        m_secretsTree->setProperty("account", QString());
        m_vaultAccessTree->clear();
        m_vaultAccessTree->setProperty("account", QString());
        return;
    }
    m_saName->setText(account.name);
    m_saCreated->setText(formatTime(account.createdAt, tr("unknown")));
    m_saFingerprint->setText(account.publicKeys.isNull() ? tr("unknown")
                                                         : AccountCrypto::fingerprint(account.publicKeys));
    if (!account.expiresAt.isValid()) {
        m_saExpiry->setText(tr("Never"));
    } else if (isExpired(account.expiresAt)) {
        m_saExpiry->setText(tr("Expired %1").arg(formatTime(account.expiresAt, {})));
    } else {
        m_saExpiry->setText(formatTime(account.expiresAt, {}));
    }
    {
        const QSignalBlocker blocker(m_enabledSwitch);
        m_enabledSwitch->setIsOn(!account.disabled);
    }
    m_enabledLabel->setText(account.disabled ? tr("Off: its tokens don't work until it is enabled again")
                                             : tr("On: tokens with its secrets work"));

    if (m_vaultAccessTree->property("account").toString() != account.id) {
        m_vaultAccessTree->setProperty("account", account.id);
        loadVaultAccess();
    }
    // The secrets are fetched again only for another service account, or after they changed
    if (m_secretsTree->property("account").toString() == account.id) {
        return;
    }
    m_secretsTree->setProperty("account", account.id);
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->listSecrets(
        account.id, [guard, id = account.id](const QList<ServiceAccountAdmin::Secret>& secrets, const QString& error) {
            if (!guard || guard->currentServiceAccount() != id) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showMessage(error, InfoBar::Error);
                return;
            }
            guard->m_secrets = secrets;
            auto* tree = guard->m_secretsTree;
            tree->clear();
            for (const auto& secret : secrets) {
                auto* item = new QTreeWidgetItem(tree);
                item->setData(0, IdRole, secret.id);
                item->setText(0, secret.name.isEmpty() ? tr("(unnamed)") : secret.name);
                item->setText(1, secret.prefix + "…");
                item->setFont(1, QFontDatabase::systemFont(QFontDatabase::FixedFont));
                item->setText(2, formatTime(secret.lastUsedAt, tr("never")));
                item->setText(3, formatTime(secret.expiresAt, tr("never")));
                if (isExpired(secret.expiresAt)) {
                    item->setToolTip(3, tr("This secret has expired"));
                }
            }
            guard->updateServiceAccountButtons();
        });
}

void ServiceAccountsDialog::loadVaultAccess()
{
    const auto account = selected();
    if (account.id.isEmpty() || !m_account) {
        return;
    }
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->vaultAccess(account, [guard, id = account.id](const QHash<QString, QString>& roles, const QString& error) {
        if (!guard || guard->currentServiceAccount() != id || !guard->m_account) {
            return;
        }
        if (!error.isEmpty()) {
            guard->showMessage(error, InfoBar::Error);
        }
        guard->m_vaultAccess = roles;
        auto* tree = guard->m_vaultAccessTree;
        const QSignalBlocker treeBlocker(tree);
        tree->clear();
        // The vaults it can't open yet are the ones to offer; filled in one go, so the choice is never empty
        // while it is being refilled
        const QSignalBlocker comboBlocker(guard->m_vaultCombo);
        const auto previous = guard->m_vaultCombo->currentData().toString();
        guard->m_vaultCombo->clear();
        for (const auto& vault : guard->m_account->keyring().vaults()) {
            if (vault.role != "manage") {
                continue;
            }
            const auto name = vault.name.isEmpty() ? vault.id : vault.name;
            if (roles.contains(vault.id)) {
                auto* item = new QTreeWidgetItem(tree);
                item->setData(0, IdRole, vault.id);
                item->setText(0, name);
                item->setText(1, vaultRoleName(roles.value(vault.id)));
                item->setData(0, OrgAdmin::SecondaryRole, vaultRoleName(roles.value(vault.id)));
                item->setData(0, Qt::AccessibleTextRole, name + ", " + vaultRoleName(roles.value(vault.id)));
            } else {
                guard->m_vaultCombo->addItem(name, vault.id);
            }
        }
        guard->m_vaultCombo->setCurrentIndex(qMax(0, guard->m_vaultCombo->findData(previous)));
        tree->setVisible(tree->topLevelItemCount() > 0);
        guard->m_vaultAccessEmpty->setVisible(tree->topLevelItemCount() == 0);
        guard->updateServiceAccountButtons();
    });
}

bool ServiceAccountsDialog::askForDate(const QString& title, const QString& text, QDateTime& expiresAt)
{
    OrgAdmin::Prompt prompt(this, title, tr("Save"), "expiryDialog");
    prompt.addText(text);
    auto never = new VaultCheckBox(tr("Never expires"), prompt.content());
    never->setObjectName("neverExpiresCheckBox");
    never->setChecked(!expiresAt.isValid());
    auto edit = new VaultDateTimeEdit(prompt.content());
    edit->setObjectName("expiryEdit");
    edit->setCalendarPopup(true);
    edit->setDateTime(expiresAt.isValid() ? expiresAt.toLocalTime() : QDateTime::currentDateTime().addDays(90));
    edit->setDisabled(!expiresAt.isValid());
    QObject::connect(never, &QCheckBox::toggled, edit, &QWidget::setDisabled);
    prompt.layout()->addWidget(never);
    prompt.layout()->addWidget(edit);
    if (!prompt.exec()) {
        return false;
    }
    expiresAt = never->isChecked() ? QDateTime() : edit->dateTime().toUTC();
    return true;
}

void ServiceAccountsDialog::askForNewServiceAccount()
{
    OrgAdmin::Prompt prompt(this, tr("New service account"), tr("Create"), "newServiceAccountDialog");
    prompt.addText(tr("Its token is shown once, right after it is made. Give it access to vaults afterwards."));
    auto name = new VaultLineEdit(prompt.content());
    name->setObjectName("serviceAccountNameEdit");
    name->setPlaceholderText(tr("Name, e.g. the job or server that uses it"));
    prompt.layout()->addWidget(name);
    auto never = new VaultCheckBox(tr("Never expires"), prompt.content());
    never->setChecked(true);
    auto edit = new VaultDateTimeEdit(prompt.content());
    edit->setCalendarPopup(true);
    edit->setDateTime(QDateTime::currentDateTime().addDays(90));
    edit->setDisabled(true);
    QObject::connect(never, &QCheckBox::toggled, edit, &QWidget::setDisabled);
    prompt.layout()->addWidget(never);
    prompt.layout()->addWidget(edit);
    prompt.acceptButton()->setEnabled(false);
    QObject::connect(name, &QLineEdit::textChanged, prompt.acceptButton(), [&prompt](const QString& text) {
        prompt.acceptButton()->setEnabled(!text.trimmed().isEmpty());
    });
    QTimer::singleShot(0, name, [name] { name->setFocus(); });
    if (prompt.exec() && !name->text().trimmed().isEmpty()) {
        createServiceAccount(name->text().trimmed(), never->isChecked() ? QDateTime() : edit->dateTime().toUTC());
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
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    OrgAdmin::Prompt prompt(this, tr("Issue a secret for %1").arg(account.name), tr("Issue"), "newSecretDialog");
    prompt.addText(tr("The new token is shown once. The service account's other secrets keep working."));
    auto name = new VaultLineEdit(prompt.content());
    name->setObjectName("secretNameEdit");
    name->setPlaceholderText(tr("Name, e.g. where it will be used (optional)"));
    prompt.layout()->addWidget(name);
    auto never = new VaultCheckBox(tr("Never expires"), prompt.content());
    never->setChecked(true);
    auto edit = new VaultDateTimeEdit(prompt.content());
    edit->setCalendarPopup(true);
    edit->setDateTime(QDateTime::currentDateTime().addDays(90));
    edit->setDisabled(true);
    QObject::connect(never, &QCheckBox::toggled, edit, &QWidget::setDisabled);
    prompt.layout()->addWidget(never);
    prompt.layout()->addWidget(edit);
    QTimer::singleShot(0, name, [name] { name->setFocus(); });
    if (prompt.exec()) {
        issueSecret(name->text().trimmed(), never->isChecked() ? QDateTime() : edit->dateTime().toUTC());
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
            guard->m_secretsTree->setProperty("account", QString());
            guard->showDetails();
            guard->loadAudit();
        });
}

void ServiceAccountsDialog::revokeSelectedSecret()
{
    const auto account = selected();
    const auto items = m_secretsTree->selectedItems();
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
        guard->showMessage(tr("The secret is revoked."), InfoBar::Success);
        guard->m_secretsTree->setProperty("account", QString());
        guard->showDetails();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askForSecretExpiry()
{
    const auto items = m_secretsTree->selectedItems();
    if (selected().id.isEmpty() || items.isEmpty()) {
        return;
    }
    const auto secretId = items.first()->data(0, IdRole).toString();
    QDateTime expiresAt;
    for (const auto& secret : m_secrets) {
        if (secret.id == secretId) {
            expiresAt = secret.expiresAt;
        }
    }
    if (askForDate(tr("Change the secret's expiry"),
                   tr("The secret keeps its token; only the time it stops working changes. A time that has passed "
                      "stops it at once."),
                   expiresAt)) {
        setSelectedSecretExpiry(expiresAt);
    }
}

void ServiceAccountsDialog::setSelectedSecretExpiry(const QDateTime& expiresAt)
{
    const auto account = selected();
    const auto items = m_secretsTree->selectedItems();
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
        guard->m_secretsTree->setProperty("account", QString());
        guard->showDetails();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askForExpiry()
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
    auto expiresAt = account.expiresAt;
    if (askForDate(tr("Change when %1 expires").arg(account.name),
                   tr("Once it expires, none of its secrets work. A time that has passed stops it at once."),
                   expiresAt)) {
        setExpiry(expiresAt);
    }
}

void ServiceAccountsDialog::setExpiry(const QDateTime& expiresAt)
{
    const auto account = selected();
    if (account.id.isEmpty()) {
        return;
    }
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
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::grantSelectedVault()
{
    const auto account = selected();
    const auto vaultId = m_vaultCombo->currentData().toString();
    if (account.id.isEmpty() || vaultId.isEmpty()) {
        return;
    }
    const auto role = m_vaultRoleCombo->currentData().toString();
    setBusy(tr("Giving access…"));
    QPointer<ServiceAccountsDialog> guard(this);
    const auto vaultName = m_vaultCombo->currentText();
    m_admin->grantVault(account, vaultId, role, [guard, account, vaultName](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->showMessage(tr("%1 can now open \"%2\".").arg(account.name, vaultName), InfoBar::Success);
        guard->loadVaultAccess();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askToRevokeVault()
{
    const auto account = selected();
    const auto items = m_vaultAccessTree->selectedItems();
    if (account.id.isEmpty() || items.isEmpty()) {
        return;
    }
    const auto vaultId = items.first()->data(0, IdRole).toString();
    auto answer = MessageBox::question(
        this,
        tr("Remove %1's access to \"%2\"?").arg(account.name, items.first()->text(0)),
        tr("The vault gets a new key, so its secrets open nothing that is saved in the vault from now on."),
        MessageBox::Remove | MessageBox::Cancel,
        MessageBox::Cancel);
    if (answer == MessageBox::Remove) {
        revokeVault(vaultId);
    }
}

void ServiceAccountsDialog::revokeVault(const QString& vaultId)
{
    const auto account = selected();
    if (account.id.isEmpty() || vaultId.isEmpty()) {
        return;
    }
    setBusy(tr("Removing access and giving the vault a new key…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->revokeVault(account, vaultId, [guard, account](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->showMessage(tr("%1 can no longer open the vault, and the vault has a new key.").arg(account.name),
                           InfoBar::Success);
        guard->loadVaultAccess();
        guard->loadAudit();
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
            guard->showDetails();
            return;
        }
        guard->setBusy({});
        guard->loadServiceAccounts();
        guard->loadAudit();
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
    m_admin->remove(account.id, [guard, account](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->showMessage(tr("%1 is deleted.").arg(account.name), InfoBar::Success);
        guard->loadServiceAccounts();
        guard->loadAudit();
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
                guard->showMessage(error, InfoBar::Error);
                return;
            }
            if (!more) {
                guard->m_auditEvents.clear();
            }
            guard->m_auditEvents << events;
            guard->m_auditCursor = next;
            guard->m_loadMoreButton->setEnabled(!next.isEmpty());
            guard->m_loadMoreButton->setVisible(!next.isEmpty());
            guard->showAudit();
        });
}

QString ServiceAccountsDialog::displayName(const QString& id) const
{
    if (id.startsWith("user:")) {
        for (const auto& member : m_members) {
            if (member.principal() == id) {
                return member.email;
            }
        }
    } else if (id.startsWith("sa:")) {
        for (const auto& account : m_accounts) {
            if ("sa:" + account.id == id) {
                return account.name;
            }
        }
    } else if (m_account) {
        for (const auto& vault : m_account->keyring().vaults()) {
            if (vault.id == id && !vault.name.isEmpty()) {
                return vault.name;
            }
        }
    }
    return id;
}

void ServiceAccountsDialog::showAudit()
{
    // Drawn again once members and service accounts are known, so ids turn into names
    m_auditTree->clear();
    for (const auto& event : m_auditEvents) {
        auto* item = new QTreeWidgetItem(m_auditTree);
        item->setText(0, formatTime(event.at, {}));
        item->setText(1, displayName(event.actor));
        item->setToolTip(1, event.actor);
        item->setText(2, auditActionName(event.action));
        // The action as the server names it, for filtering and for the tooltip
        item->setData(2, Qt::UserRole, event.action);
        item->setToolTip(2, event.action);
        item->setText(3, displayName(event.target));
        item->setToolTip(3, event.target);
        item->setText(4, event.ip);
    }
    filterAudit();
}

void ServiceAccountsDialog::filterAudit()
{
    const auto filter = m_auditFilter->text().trimmed();
    int shown = 0;
    for (int i = 0; i < m_auditTree->topLevelItemCount(); ++i) {
        auto item = m_auditTree->topLevelItem(i);
        bool match = filter.isEmpty();
        for (int column = 0; !match && column < m_auditTree->columnCount(); ++column) {
            match = item->text(column).contains(filter, Qt::CaseInsensitive);
        }
        match |= !filter.isEmpty() && item->data(2, Qt::UserRole).toString().contains(filter, Qt::CaseInsensitive);
        item->setHidden(!match);
        shown += match ? 1 : 0;
    }
    m_auditEmpty->setVisible(shown == 0);
    m_auditEmpty->setText(filter.isEmpty() ? tr("Nothing has happened in this organization yet.")
                                           : tr("No events match \"%1\" among the ones loaded.").arg(filter));
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
            guard->showMessage(error, InfoBar::Error);
            return;
        }
        guard->m_members = members;
        // Members who wait to be confirmed come first
        auto ordered = members;
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.status == "accepted" && b.status != "accepted";
        });
        const auto keep = guard->selectedMember();
        auto* tree = guard->m_membersTree;
        const QSignalBlocker blocker(tree);
        tree->clear();
        for (const auto& member : ordered) {
            auto* item = new QTreeWidgetItem(tree);
            item->setData(0, IdRole, member.userId);
            item->setData(0, StatusRole, member.status);
            item->setData(0, OrgAdmin::AvatarRole, true);
            item->setText(0, member.email);
            item->setText(1, roleName(member.role));
            item->setText(2, member.status);
            const auto fingerprint = member.publicKeys.isNull() ? tr("unknown")
                                     : member.verified          ? AccountCrypto::fingerprint(member.publicKeys)
                                                                : tr("invalid signature");
            item->setText(3, fingerprint);
            QString secondary = roleName(member.role);
            if (member.status == "accepted") {
                secondary = tr("%1 · Accepted the invitation, waiting for you to confirm them").arg(secondary);
                item->setData(0, OrgAdmin::BadgeRole, tr("Waiting"));
                item->setData(0, OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Caution));
            } else if (member.status == "invited") {
                secondary = tr("%1 · Invited").arg(secondary);
            }
            if (!member.publicKeys.isNull() && !member.verified) {
                item->setData(0, OrgAdmin::BadgeRole, tr("Keys not verified"));
                item->setData(0, OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Critical));
            } else if (guard->isSelf(member)) {
                item->setData(0, OrgAdmin::BadgeRole, tr("You"));
                item->setData(0, OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Neutral));
            }
            item->setData(0, OrgAdmin::SecondaryRole, secondary);
            item->setToolTip(0, tr("Fingerprint: %1").arg(fingerprint));
            item->setData(0, Qt::AccessibleTextRole, member.email + ", " + secondary);
            if (member.userId == keep) {
                item->setSelected(true);
            }
        }
        QStringList waiting;
        for (const auto& member : ordered) {
            if (member.status == "accepted") {
                waiting << member.email;
            }
        }
        guard->m_waitingBar->setTitle(tr("Waiting for you to confirm them"));
        guard->m_waitingBar->setMessage(waiting.size() == 1
                                            ? tr("%1 accepted an invitation.").arg(waiting.first())
                                            : tr("%1 people accepted an invitation.").arg(waiting.size()));
        guard->m_waitingBar->setToolTip(tr("They get their keys once an owner or admin confirms them: %1")
                                            .arg(QLocale().createSeparatedList(waiting)));
        guard->m_waitingBar->setVisible(!waiting.isEmpty());
        guard->updateNavigation();
        guard->updateMemberButtons();
        guard->showAudit();
    });
}

void ServiceAccountsDialog::updateMemberButtons()
{
    const auto selected = member(selectedMember());
    const bool has = !selected.userId.isEmpty();
    const bool idle = !m_busy;
    const auto myRole = currentRole();
    const bool self = isSelf(selected);
    const bool waiting = has && selected.status == "accepted";
    // Only owners change or remove owners (the server checks it too)
    const bool canChange = has && (selected.role != "owner" || myRole == "owner");

    m_confirmMemberButton->setVisible(waiting);
    m_confirmMemberButton->setEnabled(idle && waiting);
    m_changeRoleButton->setEnabled(idle && canChange && !self);
    m_removeMemberButton->setEnabled(idle && (canChange || (has && self)));
    m_removeMemberButton->setText(has && self ? tr("Leave…") : tr("Remove…"));

    QString hint;
    QString roleReason;
    if (!has) {
        hint = tr("Select a member to confirm them, change their role or remove them.");
        roleReason = hint;
    } else if (self) {
        hint = tr("This is you. You can leave the organization, but another owner has to change your role.");
        roleReason = tr("You can't change your own role");
    } else if (!canChange) {
        hint = tr("Only owners can change or remove an owner.");
        roleReason = hint;
    } else if (waiting) {
        hint = tr("Check their fingerprint with them before you confirm them: %1")
                   .arg(m_membersTree->selectedItems().first()->text(3));
    } else {
        hint = tr("Fingerprint: %1").arg(m_membersTree->selectedItems().first()->text(3));
    }
    m_memberHint->setText(hint);
    m_changeRoleButton->setToolTip(m_changeRoleButton->isEnabled() ? QString() : roleReason);
    m_removeMemberButton->setToolTip(m_removeMemberButton->isEnabled() ? QString() : roleReason);

    const bool canInvite = !currentOrg().isEmpty() && !ServiceAccountAdmin::assignableRoles(myRole).isEmpty();
    m_inviteButton->setEnabled(idle && canInvite);
    m_cancelInvitationButton->setEnabled(idle && !m_invitationsTree->selectedItems().isEmpty());
    m_cancelInvitationButton->setToolTip(m_invitationsTree->selectedItems().isEmpty() ? tr("Select an invitation first")
                                                                                      : QString());
}

void ServiceAccountsDialog::showMemberMenu(const QPoint& pos)
{
    auto item = m_membersTree->itemAt(pos);
    if (!item) {
        return;
    }
    m_membersTree->setCurrentItem(item);
    item->setSelected(true);
    VaultMenu menu(this);
    if (m_confirmMemberButton->isVisible()) {
        menu.addAction(tr("Confirm…"), this, &ServiceAccountsDialog::askToConfirmMember)
            ->setEnabled(m_confirmMemberButton->isEnabled());
    }
    menu.addAction(tr("Change role…"), this, &ServiceAccountsDialog::askForMemberRole)
        ->setEnabled(m_changeRoleButton->isEnabled());
    const auto fingerprint = item->text(3);
    menu.addAction(tr("Copy fingerprint"), this, [this, fingerprint] {
        clipboard()->setText(fingerprint);
        showMessage(tr("The fingerprint is copied."), InfoBar::Success);
    });
    menu.addSeparator();
    menu.addAction(m_removeMemberButton->text(), this, &ServiceAccountsDialog::askToRemoveMember)
        ->setEnabled(m_removeMemberButton->isEnabled());
    menu.exec(m_membersTree->viewport()->mapToGlobal(pos));
}

void ServiceAccountsDialog::reviewWaitingMembers()
{
    setCurrentSection(Members);
    for (int i = 0; i < m_membersTree->topLevelItemCount(); ++i) {
        auto item = m_membersTree->topLevelItem(i);
        if (item->data(0, StatusRole).toString() == "accepted") {
            m_membersTree->clearSelection();
            m_membersTree->setCurrentItem(item);
            item->setSelected(true);
            m_membersTree->scrollToItem(item);
            askToConfirmMember();
            return;
        }
    }
}

void ServiceAccountsDialog::askForMemberRole()
{
    const auto selected = member(selectedMember());
    const auto roles = ServiceAccountAdmin::assignableRoles(currentRole());
    if (selected.userId.isEmpty() || roles.isEmpty()) {
        return;
    }
    OrgAdmin::Prompt prompt(
        this, tr("Change the role of %1").arg(selected.email), tr("Change role"), "memberRoleDialog");
    const QHash<QString, QString> descriptions{
        {"owner", tr("Everything an admin can do, and can change or remove other owners.")},
        {"admin",
         tr("Gets every vault of the organization with manage access, invites and confirms members, and manages "
            "service accounts.")},
        {"member", tr("Gets only the vaults shared with them. A member who was an admin keeps the vaults they have.")},
    };
    QButtonGroup group;
    for (const auto& role : roles) {
        auto radio = new VaultRadioButton(roleName(role), prompt.content());
        radio->setObjectName("memberRole_" + role);
        radio->setChecked(role == selected.role);
        radio->setProperty("role", role);
        group.addButton(radio);
        prompt.layout()->addWidget(radio);
        auto description = OrgAdmin::label(prompt.content(), Typography::FontRole::Caption, true);
        description->setText(descriptions.value(role));
        description->setContentsMargins(28, 0, 0, 4);
        prompt.layout()->addWidget(description);
    }
    if (!prompt.exec() || !group.checkedButton()) {
        return;
    }
    const auto role = group.checkedButton()->property("role").toString();
    if (role != selected.role) {
        setMemberRole(selected.userId, role);
    }
}

void ServiceAccountsDialog::setMemberRole(const QString& userId, const QString& role)
{
    const auto selected = member(userId);
    if (selected.userId.isEmpty()) {
        return;
    }
    setBusy(tr("Changing the role of %1…").arg(selected.email));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->setMemberRole(currentOrg(), selected, role, [guard, selected, role](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            guard->loadMembers();
            return;
        }
        guard->setBusy({});
        guard->showMessage(tr("%1 is now %2.").arg(selected.email, roleName(role).toLower()), InfoBar::Success);
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askToRemoveMember()
{
    const auto selected = member(selectedMember());
    if (selected.userId.isEmpty()) {
        return;
    }
    const bool self = isSelf(selected);
    auto answer = MessageBox::question(
        this,
        self ? tr("Leave the organization?") : tr("Remove %1?").arg(selected.email),
        self ? tr("You lose access to the organization's vaults.")
             : tr("%1 loses access to every vault of the organization. The vaults they could open get a new key, so "
                  "the keys they had open nothing that is saved from now on.")
                   .arg(selected.email),
        MessageBox::Remove | MessageBox::Cancel,
        MessageBox::Cancel);
    if (answer == MessageBox::Remove) {
        removeMember(selected.userId);
    }
}

void ServiceAccountsDialog::removeMember(const QString& userId)
{
    const auto selected = member(userId);
    if (selected.userId.isEmpty()) {
        return;
    }
    setBusy(tr("Removing %1 and rotating the keys of their vaults…").arg(selected.email));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->removeMember(
        currentOrg(),
        userId,
        [guard, selected](const QStringList& rotated, const QStringList& failed, const QString& error) {
            if (!guard) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showError(error);
                return;
            }
            guard->setBusy({});
            if (!failed.isEmpty()) {
                guard->showMessage(
                    tr("%1 was removed, but these vaults still need a new key (Share, then Rotate key): %2")
                        .arg(selected.email, failed.join(QStringLiteral("; "))),
                    InfoBar::Warning,
                    false);
            } else {
                guard->showMessage(
                    tr("%1 was removed. %n vault(s) got a new key.", nullptr, rotated.size()).arg(selected.email),
                    InfoBar::Success);
            }
            if (guard->isSelf(selected)) {
                guard->reload();
                return;
            }
            guard->loadMembers();
            guard->loadAudit();
        });
}

void ServiceAccountsDialog::askForNewOrg()
{
    OrgAdmin::Prompt prompt(this, tr("New organization"), tr("Create"), "newOrgDialog");
    prompt.addText(tr("You become its owner. You can invite people and create vaults for it afterwards."));
    auto name = new VaultLineEdit(prompt.content());
    name->setObjectName("orgNameEdit");
    name->setPlaceholderText(tr("Name of the organization"));
    name->setMaxLength(128);
    prompt.layout()->addWidget(name);
    prompt.acceptButton()->setEnabled(false);
    QObject::connect(name, &QLineEdit::textChanged, prompt.acceptButton(), [&prompt](const QString& text) {
        prompt.acceptButton()->setEnabled(!text.trimmed().isEmpty());
    });
    QTimer::singleShot(0, name, [name] { name->setFocus(); });
    if (prompt.exec() && !name->text().trimmed().isEmpty()) {
        createOrg(name->text().trimmed());
    }
}

void ServiceAccountsDialog::createOrg(const QString& name)
{
    setBusy(tr("Creating %1…").arg(name));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->createOrg(name, [guard](const ServiceAccountAdmin::Org& org, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->showError(error);
            return;
        }
        guard->m_selectOrg = org.id;
        guard->reload();
        guard->setCurrentSection(Invitations);
        guard->showMessage(tr("%1 is created, with you as its owner. Invite people to it next.").arg(org.name),
                           InfoBar::Success);
        if (guard->m_account) {
            guard->m_account->reloadVaults();
        }
    });
}

void ServiceAccountsDialog::loadInvitations()
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->listInvitations(
        orgId, [guard, orgId](const QList<ServiceAccountAdmin::Invitation>& invitations, const QString& error) {
            if (!guard || guard->currentOrg() != orgId) {
                return;
            }
            if (!error.isEmpty()) {
                guard->showMessage(error, InfoBar::Error);
                return;
            }
            guard->m_invitations = invitations;
            auto* tree = guard->m_invitationsTree;
            const QSignalBlocker blocker(tree);
            tree->clear();
            for (const auto& invitation : invitations) {
                auto* item = new QTreeWidgetItem(tree);
                item->setData(0, IdRole, invitation.id);
                item->setData(0, OrgAdmin::AvatarRole, true);
                item->setText(0, invitation.email);
                item->setText(1, roleName(invitation.role));
                item->setText(2, formatTime(invitation.expiresAt, tr("never")));
                QString secondary = roleName(invitation.role);
                if (isExpired(invitation.expiresAt)) {
                    secondary = tr("%1 · Expired %2").arg(secondary, formatDate(invitation.expiresAt));
                    item->setData(0, OrgAdmin::BadgeRole, tr("Expired"));
                    item->setData(0, OrgAdmin::BadgeKindRole, static_cast<int>(OrgAdmin::BadgeKind::Critical));
                } else if (invitation.expiresAt.isValid()) {
                    secondary = tr("%1 · Expires %2").arg(secondary, formatDate(invitation.expiresAt));
                }
                item->setData(0, OrgAdmin::SecondaryRole, secondary);
                item->setData(0, Qt::AccessibleTextRole, invitation.email + ", " + secondary);
            }
            tree->setVisible(!invitations.isEmpty());
            guard->m_invitationsEmpty->setVisible(invitations.isEmpty());
            guard->updateMemberButtons();
        });
}

void ServiceAccountsDialog::askToInvite()
{
    const auto roles = ServiceAccountAdmin::assignableRoles(currentRole());
    if (currentOrg().isEmpty() || roles.isEmpty()) {
        return;
    }
    OrgAdmin::Prompt prompt(
        this, tr("Invite to %1").arg(m_orgCombo->currentText()), tr("Send invitation"), "inviteMemberDialog");
    prompt.addText(tr("The invitation goes out by email and is valid for 7 days. Once they accept it, confirm them "
                      "under Members to give them their keys."));
    auto email = new VaultLineEdit(prompt.content());
    email->setObjectName("inviteEmailEdit");
    email->setPlaceholderText(tr("Email address"));
    prompt.layout()->addWidget(email);
    auto heading = OrgAdmin::label(prompt.content(), Typography::FontRole::BodyStrong);
    heading->setText(tr("Role"));
    prompt.layout()->addWidget(heading);
    QButtonGroup group;
    for (const auto& role : roles) {
        auto radio = new VaultRadioButton(roleName(role), prompt.content());
        radio->setObjectName("inviteRole_" + role);
        radio->setProperty("role", role);
        radio->setChecked(role == "member");
        group.addButton(radio);
        prompt.layout()->addWidget(radio);
    }
    // An address needs something on both sides of the @ and a dot in the domain
    static const QRegularExpression address(QStringLiteral("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$"));
    prompt.acceptButton()->setEnabled(false);
    QObject::connect(email, &QLineEdit::textChanged, prompt.acceptButton(), [&prompt](const QString& text) {
        prompt.acceptButton()->setEnabled(address.match(text.trimmed()).hasMatch());
    });
    QTimer::singleShot(0, email, [email] { email->setFocus(); });
    if (!prompt.exec() || !address.match(email->text().trimmed()).hasMatch() || !group.checkedButton()) {
        return;
    }
    invite(email->text().trimmed(), group.checkedButton()->property("role").toString());
}

void ServiceAccountsDialog::invite(const QString& email, const QString& role)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    setBusy(tr("Inviting %1…").arg(email));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->invite(orgId, email, role, [guard, email](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->showMessage(tr("An invitation is on its way to %1.").arg(email), InfoBar::Success);
        guard->loadInvitations();
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::cancelSelectedInvitation()
{
    const auto items = m_invitationsTree->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    const auto invitationId = items.first()->data(0, IdRole).toString();
    setBusy(tr("Cancelling the invitation…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->cancelInvitation(currentOrg(), invitationId, [guard](bool ok, const QString& error) {
        if (!guard) {
            return;
        }
        if (!ok) {
            guard->showError(error);
            return;
        }
        guard->setBusy({});
        guard->loadInvitations();
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::askToConfirmMember()
{
    const auto userId = selectedMember();
    const auto member = this->member(userId);
    if (member.userId.isEmpty() || member.status != "accepted") {
        return;
    }
    const auto fingerprint = AccountCrypto::fingerprint(member.publicKeys);
    OrgAdmin::Prompt prompt(this, tr("Confirm %1").arg(member.email), tr("Confirm"), "confirmMemberDialog");
    prompt.addText(tr("Confirming gives %1 their keys. Check with them, for example on a call, that this is "
                      "their fingerprint:")
                       .arg(member.email));
    auto fingerprintRow = new QHBoxLayout();
    auto fingerprintLabel = OrgAdmin::label(prompt.content(), Typography::FontRole::BodyStrong);
    fingerprintLabel->setObjectName("confirmFingerprint");
    fingerprintLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    fingerprintLabel->setText(fingerprint);
    fingerprintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    auto copy = Vault::iconButton(Typography::Icons::Copy, tr("Copy fingerprint"), prompt.content());
    QObject::connect(copy, &QPushButton::clicked, copy, [fingerprint] { clipboard()->setText(fingerprint); });
    fingerprintRow->addWidget(fingerprintLabel, 1);
    fingerprintRow->addWidget(copy);
    prompt.layout()->addLayout(fingerprintRow);

    QHash<QCheckBox*, QComboBox*> picks;
    if (member.isAdmin()) {
        prompt.addText(tr("As %1 of the organization, they get every vault with manage access and can issue secrets "
                          "for its service accounts.")
                           .arg(roleName(member.role).toLower()));
    } else {
        // A regular member gets the vaults picked here
        const auto vaults = m_admin->orgVaults(currentOrg());
        if (vaults.isEmpty()) {
            prompt.addText(tr("The organization has no vaults you manage yet. Share vaults with them later."));
        } else {
            prompt.addText(tr("Choose the vaults they can open:"));
            auto grid = new QGridLayout();
            grid->setHorizontalSpacing(12);
            grid->setVerticalSpacing(8);
            grid->setColumnStretch(0, 1);
            for (const auto& vault : vaults) {
                auto* pick = new VaultCheckBox(vault.second, prompt.content());
                pick->setProperty("vaultId", vault.first);
                auto* combo = new VaultComboBox(prompt.content());
                combo->setAccessibleName(tr("Access to %1").arg(vault.second));
                combo->addItem(vaultRoleName("read"), "read");
                combo->addItem(vaultRoleName("write"), "write");
                combo->addItem(vaultRoleName("manage"), "manage");
                combo->setEnabled(false);
                QObject::connect(pick, &QCheckBox::toggled, combo, &QWidget::setEnabled);
                const int row = grid->rowCount();
                grid->addWidget(pick, row, 0);
                grid->addWidget(combo, row, 1);
                picks.insert(pick, combo);
            }
            prompt.layout()->addLayout(grid);
        }
    }
    if (!prompt.exec()) {
        return;
    }
    QHash<QString, QString> vaults;
    for (auto it = picks.constBegin(); it != picks.constEnd(); ++it) {
        if (it.key()->isChecked()) {
            vaults.insert(it.key()->property("vaultId").toString(), it.value()->currentData().toString());
        }
    }
    confirmMember(userId, vaults);
}

void ServiceAccountsDialog::confirmMember(const QString& userId, const QHash<QString, QString>& vaults)
{
    const auto member = this->member(userId);
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
        guard->showMessage(tr("%1 is confirmed and has their keys.").arg(member.email), InfoBar::Success);
        guard->loadMembers();
        guard->loadAudit();
    });
}

void ServiceAccountsDialog::updateShareLinks()
{
    const auto policy = shareLinkPolicy();
    static const QStringList policies{"on", "managers", "off"};
    const int id = policies.indexOf(policy.isEmpty() ? QStringLiteral("on") : policy);
    if (auto radio = m_shareLinks->button(id)) {
        radio->setChecked(true);
    }
}

void ServiceAccountsDialog::askForShareLinkPolicy(int id)
{
    const auto current = shareLinkPolicy();
    const auto next = policyAt(id);
    if (next.isEmpty() || next == current) {
        return;
    }
    // Narrowing ends links people already made
    const bool narrower = next == "off" || (next == "managers" && current == "on");
    if (narrower) {
        auto answer = MessageBox::question(this,
                                           tr("End some share links?"),
                                           tr("Links that this no longer allows stop opening at once."),
                                           MessageBox::Continue | MessageBox::Cancel,
                                           MessageBox::Cancel);
        if (answer != MessageBox::Continue) {
            updateShareLinks();
            return;
        }
    }
    setShareLinkPolicy(next);
}

void ServiceAccountsDialog::setShareLinkPolicy(const QString& policy)
{
    const auto orgId = currentOrg();
    if (orgId.isEmpty()) {
        return;
    }
    setBusy(tr("Saving…"));
    QPointer<ServiceAccountsDialog> guard(this);
    m_admin->setShareLinks(orgId, policy, [guard, orgId](const ServiceAccountAdmin::Org& org, const QString& error) {
        if (!guard) {
            return;
        }
        if (!error.isEmpty()) {
            guard->showError(error);
            guard->updateShareLinks();
            return;
        }
        for (auto& known : guard->m_orgs) {
            if (known.id == orgId) {
                known.shareLinks = org.shareLinks;
            }
        }
        guard->setBusy({});
        guard->updateShareLinks();
        guard->showMessage(tr("Share link setting saved."), InfoBar::Success);
        guard->loadAudit();
        // The rest of the app reads who can make links from the account's orgs
        if (guard->m_account) {
            guard->m_account->reloadVaults();
        }
    });
}

void ServiceAccountsDialog::showToken(const QString& title, const QString& token)
{
    delete m_tokenDialog.data();
    const auto explanation = tr("Copy this token now and store it where it will be used, e.g. as a CI secret named "
                                "OPENVIREO_SERVICE_ACCOUNT_TOKEN. It is shown only once and can't be recovered; "
                                "issue a new secret if it is lost.");
    auto field = new VaultLineEdit();
    field->setObjectName("serviceAccountToken");
    field->setReadOnly(true);
    field->setText(token);
    field->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    field->setMinimumWidth(420);

    if (OrgAdmin::Prompt::canShowFluent(this)) {
        using fluent::dialogs_flyouts::ContentDialog;
        auto dialog = new ContentDialog(window());
        dialog->setObjectName("serviceAccountTokenDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setThemeSource(window());
        dialog->setTitle(title);
        dialog->setPrimaryButtonText(tr("Copy"));
        dialog->setCloseButtonText(tr("Done"));
        dialog->setDefaultButton(ContentDialog::Primary);
        dialog->setClosePolicy(fluent::dialogs_flyouts::Dialog::NoAutoClose);
        auto content = new QWidget();
        auto layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto warning = new InfoBar(content);
        warning->setSeverity(InfoBar::Warning);
        warning->setTitle(tr("Shown only once"));
        warning->setMessage(explanation);
        warning->setIsClosable(false);
        warning->setSingleLine(false);
        warning->setMultiLineMinHeight(0);
        warning->setIsOpen(true);
        warning->setSizePolicy(QSizePolicy::Expanding, warning->sizePolicy().verticalPolicy());
        layout->addWidget(warning);
        field->setParent(content);
        layout->addWidget(field);
        dialog->setContent(new VaultFittedContent(content));
        dialog->resize(560 + 2 * dialog->shadowSize(), dialog->minimumHeight());
        // Copying keeps the window open, so the token can be copied again; Done forgets it
        connect(dialog, &ContentDialog::primaryButtonClicked, dialog, [dialog, field] {
            clipboard()->setText(field->text());
            for (auto button : dialog->findChildren<fluent::basicinput::Button*>()) {
                if (button->text() == tr("Copy")) {
                    button->setText(tr("Copied"));
                }
            }
        });
        connect(dialog, &QDialog::finished, field, [field] { field->clear(); });
        m_tokenDialog = dialog;
        dialog->open();
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setObjectName("serviceAccountTokenDialog");
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* label = OrgAdmin::label(dialog, Typography::FontRole::Body);
    label->setText(explanation);
    field->setParent(dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, dialog, [field, copy] {
        clipboard()->setText(field->text());
        copy->setText(tr("Copied"));
    });
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
