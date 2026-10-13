
/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
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

#include "DatabaseSettingsWidgetKeeShare.h"
#include "ui_DatabaseSettingsWidgetKeeShare.h"

#include "core/Group.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultSettingsPage.h"
#include "gui/vault/VaultWidgets.h"
#include "keeshare/KeeShare.h"
#include "keeshare/KeeShareSettings.h"

#include <QHeaderView>
#include <QLabel>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace
{
    constexpr int TableHeight = 180;
} // namespace

DatabaseSettingsWidgetKeeShare::DatabaseSettingsWidgetKeeShare(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::DatabaseSettingsWidgetKeeShare())
{
    m_ui->setupUi(this);

    if (FluentTheme::vaultLayoutEnabled()) {
        buildVaultPage();
    }
}

void DatabaseSettingsWidgetKeeShare::buildVaultPage()
{
    auto ui = m_ui.data();
    // The database settings list already names the page
    VaultSettingsPage page(this, {});
    auto sharing = page.addSection(ui->enableGroupBox);

    // The shared groups across the card, or a note while there are none
    m_noSharesLabel = new QLabel(tr("No groups of this database are shared. A group is shared from the KeeShare "
                                    "settings of the group."),
                                 this);
    m_noSharesLabel->setObjectName("noSharedGroupsLabel");
    m_noSharesLabel->setWordWrap(true);
    Vault::followTheme(m_noSharesLabel, [label = m_noSharesLabel] {
        Vault::styleLabel(label, Typography::FontRole::Body, FluentTheme::palette().textSecondary);
    });
    m_noSharesLabel->hide();

    auto table = ui->sharedGroupsView;
    table->setFixedHeight(TableHeight);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto shares = new QWidget(this);
    auto sharesLayout = new QVBoxLayout(shares);
    sharesLayout->setContentsMargins(0, 0, 0, 0);
    sharesLayout->addWidget(table);
    sharesLayout->addWidget(m_noSharesLabel);
    sharing->addWidget(shares);

    page.finish();
}

DatabaseSettingsWidgetKeeShare::~DatabaseSettingsWidgetKeeShare()
{
}

void DatabaseSettingsWidgetKeeShare::loadSettings(const QSharedPointer<Database>& db)
{
    m_db = db;

    m_referencesModel.reset(new QStandardItemModel());

    m_referencesModel->setHorizontalHeaderLabels(QStringList() << tr("Breadcrumb") << tr("Type") << tr("Path")
                                                               << tr("Last Signer") << tr("Certificates"));
    const QList<Group*> groups = db->rootGroup()->groupsRecursive(true);
    for (const Group* group : groups) {
        if (!KeeShare::isShared(group)) {
            continue;
        }
        const KeeShareSettings::Reference reference = KeeShare::referenceOf(group);

        QStringList hierarchy = group->hierarchy();
        hierarchy.removeFirst();
        QList<QStandardItem*> row = QList<QStandardItem*>();
        row << new QStandardItem(hierarchy.join(tr(" > ", "Breadcrumb separator")));
        row << new QStandardItem(KeeShare::referenceTypeLabel(reference));
        row << new QStandardItem(reference.path);
        m_referencesModel->appendRow(row);
    }

    m_ui->sharedGroupsView->setModel(m_referencesModel.data());

    if (m_noSharesLabel) {
        const bool empty = m_referencesModel->rowCount() == 0;
        m_noSharesLabel->setVisible(empty);
        m_ui->sharedGroupsView->setVisible(!empty);

        // The signer and certificates are never filled in, so the group, type and path get their room
        auto header = m_ui->sharedGroupsView->horizontalHeader();
        for (int column = 3; column < m_referencesModel->columnCount(); ++column) {
            m_ui->sharedGroupsView->setColumnHidden(column, true);
        }
        header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        header->setStretchLastSection(true);
    }
}

void DatabaseSettingsWidgetKeeShare::saveSettings()
{
    // nothing to do - the tab is passive
}
