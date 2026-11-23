/*
 *  Copyright (C) 2017 KeePassXC Team <team@keepassxc.org>
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

#include "CloneDialog.h"
#include "ui_CloneDialog.h"

#include <QPushButton>

#include "gui/vault/VaultSettingsPage.h"

CloneDialog::CloneDialog(DatabaseWidget* parent, Database* db, Entry* entry)
    : VaultDialog(parent)
    , m_ui(new Ui::CloneDialog())
{
    m_db = db;
    m_entry = entry;
    m_parent = parent;

    m_ui->setupUi(this);

    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_DeleteOnClose);
    setNamePrefix(QStringLiteral("vaultClone"));
    setCommands(m_ui->buttonBox);
    m_ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Clone"));

    // The options are switches on settings cards
    {
        VaultSettingsPage page(this, {});
        auto* options = page.addSection();
        options->addToggle(m_ui->titleClone, tr("So the copy is easy to tell apart from the original"));
        options->addToggle(m_ui->referencesClone,
                           tr("The copy follows the original's username and password when they change"));
        options->addToggle(m_ui->historyClone, tr("The copy keeps the original's earlier versions"));
        m_ui->buttonBox->hide();
        page.finish();
    }

    connect(m_ui->buttonBox, SIGNAL(rejected()), SLOT(close()));
    connect(m_ui->buttonBox, SIGNAL(accepted()), SLOT(cloneEntry()));
}

void CloneDialog::cloneEntry()
{
    Entry::CloneFlags flags = Entry::CloneNewUuid | Entry::CloneResetTimeInfo;

    if (m_ui->titleClone->isChecked()) {
        flags |= Entry::CloneRenameTitle;
    }

    if (m_ui->referencesClone->isChecked()) {
        flags |= Entry::CloneUserAsRef;
        flags |= Entry::ClonePassAsRef;
    }

    if (m_ui->historyClone->isChecked()) {
        flags |= Entry::CloneIncludeHistory;
    }

    auto entry = m_entry->clone(flags);
    entry->setGroup(m_entry->group());

    emit entryCloned(entry);
    close();
}

CloneDialog::~CloneDialog() = default;
