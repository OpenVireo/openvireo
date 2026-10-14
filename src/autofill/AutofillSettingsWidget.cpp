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

#include "AutofillSettingsWidget.h"

#include "AutofillCredentials.h"
#include "AutofillPlatform.h"
#include "AutofillService.h"
#include "core/Database.h"
#include "core/Metadata.h"
#include "core/Tools.h"
#include "gui/DatabaseWidget.h"
#include "gui/MainWindow.h"
#include "gui/MessageWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultSettingsPage.h"

#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

AutofillSettingsWidget::AutofillSettingsWidget(QWidget* parent)
    : QWidget(parent)
    , m_statusMessage(new MessageWidget(this))
    , m_enableCheckBox(
          new VaultCheckBox(tr("Fill passwords, passkeys and one-time codes in Safari and other apps"), this))
    , m_keepIdentitiesCheckBox(new VaultCheckBox(tr("Keep suggesting usernames while a database is locked"), this))
    , m_openSettingsButton(new VaultButton(tr("Open AutoFill Settings…"), this))
    , m_databasesLabel(new QLabel(tr("Databases to fill from:"), this))
    , m_databasesList(new QListWidget(this))
{
    m_statusMessage->setCloseButtonVisible(false);
    m_statusMessage->setAutoHideTimeout(-1);
    m_statusMessage->setVisible(false);
    m_databasesList->setObjectName("autofillDatabasesList");
    m_databasesList->setMinimumHeight(90);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_statusMessage);
    layout->addWidget(m_enableCheckBox);
    layout->addWidget(m_keepIdentitiesCheckBox);
    layout->addWidget(m_openSettingsButton);
    layout->addWidget(m_databasesLabel);
    layout->addWidget(m_databasesList);
    layout->addStretch();

    connect(m_enableCheckBox, &QAbstractButton::toggled, this, &AutofillSettingsWidget::updateEnabledState);
    connect(m_openSettingsButton, &QAbstractButton::clicked, this, [] { AutofillPlatform::openProviderSettings(); });

    if (FluentTheme::vaultLayoutEnabled()) {
        buildVaultPage();
    }
}

void AutofillSettingsWidget::buildVaultPage()
{
    VaultSettingsPage page(this, tr("AutoFill"));
    page.addWidget(m_statusMessage);

    auto provider = page.addSection();
    provider->addToggle(m_enableCheckBox,
                        tr("Also turn KeePassXC on in System Settings under General, AutoFill & Passwords."));
    provider->addSetting(tr("System Settings"), m_openSettingsButton);

    auto behavior = page.addSection(tr("Suggestions"));
    behavior->addToggle(m_keepIdentitiesCheckBox,
                        tr("Usernames stay in the QuickType bar; filling one still asks you to unlock."));
    behavior->addSetting(m_databasesLabel, m_databasesList);

    page.finish();
}

void AutofillSettingsWidget::loadSettings()
{
    const auto available = AutofillService::isAvailable() && AutofillService::isSignedForAutofill();
    m_enableCheckBox->setEnabled(available);
    m_enableCheckBox->setChecked(available && autofillService()->isEnabled());
    m_keepIdentitiesCheckBox->setChecked(autofillService()->keepIdentitiesWhenLocked());

    // Only unlocked databases can be listed by name and id
    m_databasesList->clear();
    const auto excluded = autofillService()->excludedDatabases();
    for (auto* dbWidget : getMainWindow()->getOpenDatabases()) {
        if (dbWidget->isLocked() || !dbWidget->database()) {
            continue;
        }
        const auto db = dbWidget->database();
        const auto id = Tools::uuidToHex(AutofillCredentials::databaseId(db.data()));
        auto name = db->metadata()->name();
        if (name.isEmpty()) {
            name = QFileInfo(db->filePath()).fileName();
        }
        auto item = new QListWidgetItem(name, m_databasesList);
        item->setData(Qt::UserRole, id);
        item->setToolTip(db->filePath());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(excluded.contains(id) ? Qt::Unchecked : Qt::Checked);
    }
    if (m_databasesList->count() == 0) {
        auto item = new QListWidgetItem(tr("Unlock a database to choose whether AutoFill uses it."), m_databasesList);
        item->setFlags(Qt::NoItemFlags);
    }

    if (!available) {
        m_statusMessage->showMessage(tr("AutoFill needs macOS 11 or later."), MessageWidget::Warning);
    } else if (!AutofillService::isSignedForAutofill()) {
        m_statusMessage->showMessage(tr("This build of KeePassXC is not signed for AutoFill, so AutoFill stays off."),
                                     MessageWidget::Warning);
    } else {
        updateProviderStatus();
    }
    updateEnabledState();
}

void AutofillSettingsWidget::saveSettings()
{
    // Keep exclusions of databases that are not unlocked now, which the list can't show
    auto excluded = autofillService()->excludedDatabases();
    for (int i = 0; i < m_databasesList->count(); ++i) {
        const auto* item = m_databasesList->item(i);
        const auto id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) {
            continue;
        }
        excluded.removeAll(id);
        if (item->checkState() == Qt::Unchecked) {
            excluded << id;
        }
    }

    autofillService()->setExcludedDatabases(excluded);
    autofillService()->setKeepIdentitiesWhenLocked(m_keepIdentitiesCheckBox->isChecked());
    autofillService()->setEnabled(m_enableCheckBox->isChecked());
}

void AutofillSettingsWidget::updateEnabledState()
{
    const bool enabled = m_enableCheckBox->isChecked();
    m_keepIdentitiesCheckBox->setEnabled(enabled);
    m_databasesLabel->setEnabled(enabled);
    m_databasesList->setEnabled(enabled);
}

void AutofillSettingsWidget::updateProviderStatus()
{
    AutofillPlatform::queryProviderEnabled(this, [this](bool enabled) {
        if (enabled) {
            m_statusMessage->showMessage(tr("KeePassXC is turned on as an AutoFill provider in System Settings."),
                                         MessageWidget::Positive);
        } else {
            m_statusMessage->showMessage(
                tr("KeePassXC is not turned on in System Settings yet. Open AutoFill Settings and turn it on "
                   "under AutoFill From."),
                MessageWidget::Information);
        }
    });
}
