/*
 *  Copyright (C) 2017 Toni Spets <toni.spets@iki.fi>
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

#include "AgentSettingsWidget.h"
#include "SSHAgent.h"
#include "ui_AgentSettingsWidget.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultSettingsPage.h"

AgentSettingsWidget::AgentSettingsWidget(QWidget* parent)
    : QWidget(parent)
    , m_ui(new Ui::AgentSettingsWidget())
{
    m_ui->setupUi(this);
#ifndef Q_OS_WIN
    m_ui->usePageantRadioButton->setVisible(false);
    m_ui->useOpenSSHRadioButton->setVisible(false);
    m_ui->useBothRadioButton->setVisible(false);
#else
    m_ui->sshAuthSockWidget->setVisible(false);
#endif
    m_ui->sshAuthSockMessageWidget->setVisible(sshAgent()->isEnabled());
    m_ui->sshAuthSockMessageWidget->setCloseButtonVisible(false);
    m_ui->sshAuthSockMessageWidget->setAutoHideTimeout(-1);
    connect(m_ui->enableSSHAgentCheckBox, SIGNAL(toggled(bool)), SLOT(toggleSettingsEnabled()));

    if (FluentTheme::vaultLayoutEnabled()) {
        buildVaultPage();
    }
}

AgentSettingsWidget::~AgentSettingsWidget()
{
}

void AgentSettingsWidget::buildVaultPage()
{
    VaultSettingsPage page(this, tr("SSH Agent"));
    page.addWidget(m_ui->sshAuthSockMessageWidget);

    // The choice of agents only shows on Windows
    auto agent = page.addSection();
    auto enable = agent->addToggle(m_ui->enableSSHAgentCheckBox);
    enable->addWidget(m_ui->usePageantRadioButton);
    enable->addWidget(m_ui->useOpenSSHRadioButton);
    enable->addWidget(m_ui->useBothRadioButton);

    auto environment = page.addSection(tr("Environment"));
    environment->follow(m_ui->agentConfigPageBody);
    auto authSock = environment->addSetting(m_ui->sshAuthSockValueLabel, m_ui->sshAuthSockLabel);
    authSock->addSetting(m_ui->sshAuthSockOverrideLabel, m_ui->sshAuthSockOverrideEdit);
    auto securityKeyProvider =
        environment->addSetting(m_ui->sshSecurityKeyProviderValueLabel, m_ui->sshSecurityKeyProviderLabel);
    securityKeyProvider->addSetting(m_ui->sshSecurityKeyProviderOverrideLabel,
                                    m_ui->sshSecurityKeyProviderOverrideEdit);

    // Long paths are cut off rather than holding the modal wider; they can still be selected in full
    for (auto value : {m_ui->sshAuthSockLabel, m_ui->sshSecurityKeyProviderLabel}) {
        value->setMinimumWidth(120);
    }

    page.finish();
}

void AgentSettingsWidget::loadSettings()
{
    auto sshAgentEnabled = sshAgent()->isEnabled();

    m_ui->enableSSHAgentCheckBox->setChecked(sshAgentEnabled);
#ifdef Q_OS_WIN
    m_ui->usePageantRadioButton->setChecked(sshAgent()->usePageant());
    m_ui->useOpenSSHRadioButton->setChecked(sshAgent()->useOpenSSH());
    m_ui->useBothRadioButton->setChecked(sshAgent()->usePageant() && sshAgent()->useOpenSSH());
    sshAgentEnabled = sshAgentEnabled && (sshAgent()->usePageant() || sshAgent()->useOpenSSH());
#else
    auto sshAuthSock = sshAgent()->socketPath(false);
    auto sshAuthSockOverride = sshAgent()->authSockOverride();
    m_ui->sshAuthSockLabel->setText(sshAuthSock.isEmpty() ? tr("(empty)") : sshAuthSock);
    m_ui->sshAuthSockOverrideEdit->setText(sshAuthSockOverride);
    auto sshSecurityKeyProvider = sshAgent()->securityKeyProvider(false);
    auto sshSecurityKeyProviderOverride = sshAgent()->securityKeyProviderOverride();
    m_ui->sshSecurityKeyProviderLabel->setText(sshSecurityKeyProvider.isEmpty() ? tr("(empty)")
                                                                                : sshSecurityKeyProvider);
    m_ui->sshSecurityKeyProviderOverrideEdit->setText(sshSecurityKeyProviderOverride);
#endif

    m_ui->sshAuthSockMessageWidget->setVisible(sshAgentEnabled);

    if (sshAgentEnabled) {
#ifndef Q_OS_WIN
        if (sshAuthSock.isEmpty() && sshAuthSockOverride.isEmpty()) {
            m_ui->sshAuthSockMessageWidget->showMessage(
                tr("No SSH Agent socket available. Either make sure SSH_AUTH_SOCK environment variable exists or set "
                   "an override."),
                MessageWidget::Warning);
            return;
        }
#endif
        QList<QSharedPointer<OpenSSHKey>> keys;
        if (sshAgent()->listIdentities(keys)) {
            m_ui->sshAuthSockMessageWidget->showMessage(tr("SSH Agent connection is working!"),
                                                        MessageWidget::Positive);
        } else {
            m_ui->sshAuthSockMessageWidget->showMessage(sshAgent()->errorString(), MessageWidget::Error);
        }
    }

    toggleSettingsEnabled();
}

void AgentSettingsWidget::saveSettings()
{
    auto sshAuthSockOverride = m_ui->sshAuthSockOverrideEdit->text().trimmed();
    sshAgent()->setAuthSockOverride(sshAuthSockOverride);
    auto sshSecurityKeyProviderOverride = m_ui->sshSecurityKeyProviderOverrideEdit->text().trimmed();
    sshAgent()->setSecurityKeyProviderOverride(sshSecurityKeyProviderOverride);
#ifdef Q_OS_WIN
    sshAgent()->setUsePageant(m_ui->usePageantRadioButton->isChecked() || m_ui->useBothRadioButton->isChecked());
    sshAgent()->setUseOpenSSH(m_ui->useOpenSSHRadioButton->isChecked() || m_ui->useBothRadioButton->isChecked());
#endif
    sshAgent()->setEnabled(m_ui->enableSSHAgentCheckBox->isChecked());
}

void AgentSettingsWidget::toggleSettingsEnabled()
{
    const bool enabled = m_ui->enableSSHAgentCheckBox->isChecked();
    m_ui->agentConfigPageBody->setEnabled(enabled);
    // The settings cards move the choice of agents out of the form's frame
    m_ui->usePageantRadioButton->setEnabled(enabled);
    m_ui->useOpenSSHRadioButton->setEnabled(enabled);
    m_ui->useBothRadioButton->setEnabled(enabled);
}
