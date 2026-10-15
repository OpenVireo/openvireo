/*
 *  Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
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

#include "DatabaseSettingsWidgetRemote.h"
#include "ui_DatabaseSettingsWidgetRemote.h"

#include "core/Global.h"
#include "core/Metadata.h"

#include "RemoteHandler.h"
#include "RemoteSettings.h"
#include "SyncTokenStore.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultSettingsPage.h"

#include <QFile>
#include <QHBoxLayout>
#include <QTabBar>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    constexpr int FieldWidth = 320;
    constexpr int NumberWidth = 140;
    constexpr int ListHeight = 120;
    constexpr int InputHeight = 72;
} // namespace

DatabaseSettingsWidgetRemote::DatabaseSettingsWidgetRemote(QWidget* parent)
    : DatabaseSettingsWidget(parent)
    , m_remoteSettings(new RemoteSettings(nullptr, this))
    , m_ui(new Ui::DatabaseSettingsWidgetRemote())
{
    m_ui->setupUi(this);
    m_ui->messageWidget->setHidden(true);

    connect(m_ui->saveSettingsButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::saveCurrentSettings);
    connect(
        m_ui->removeSettingsButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::removeCurrentSettings);
    connect(m_ui->settingsListWidget,
            &QListWidget::itemSelectionChanged,
            this,
            &DatabaseSettingsWidgetRemote::editCurrentSettings);
    connect(m_ui->testDownloadCommandButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::testDownload);
    connect(m_ui->testServerButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::testDownload);
    connect(m_ui->typeComboBox,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &DatabaseSettingsWidgetRemote::updateTypeVisibility);

    auto setModified = [this]() { m_modified = true; };
    connect(m_ui->nameLineEdit, &QLineEdit::textChanged, setModified);
    connect(m_ui->downloadCommand, &QLineEdit::textChanged, setModified);
    connect(m_ui->inputForDownload, &QPlainTextEdit::textChanged, setModified);
    connect(m_ui->downloadTimeoutSec, QOverload<int>::of(&QSpinBox::valueChanged), setModified);
    connect(m_ui->uploadCommand, &QLineEdit::textChanged, setModified);
    connect(m_ui->inputForUpload, &QPlainTextEdit::textChanged, setModified);
    connect(m_ui->uploadTimeoutSec, QOverload<int>::of(&QSpinBox::valueChanged), setModified);
    connect(m_ui->typeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), setModified);
    connect(m_ui->serverUrlEdit, &QLineEdit::textChanged, setModified);
    connect(m_ui->serverDatabaseIdEdit, &QLineEdit::textChanged, setModified);
    connect(m_ui->serverTokenEdit, &QLineEdit::textChanged, setModified);
    connect(m_ui->serverTimeoutSec, QOverload<int>::of(&QSpinBox::valueChanged), setModified);
    connect(m_ui->autoSyncCheckBox, &QCheckBox::toggled, setModified);

    updateTypeVisibility();
    // Each device has its own token, so it isn't stored in the database that all devices share
    m_ui->serverTokenEdit->setPlaceholderText(tr("Stored on this device only, in %1. Each device needs its own token: "
                                                 "keepassxc-sync-server devices add <user> <device-name>")
                                                  .arg(SyncTokenStore::description()));

    if (FluentTheme::vaultLayoutEnabled()) {
        buildVaultPage();
    }
}

void DatabaseSettingsWidgetRemote::buildVaultPage()
{
    auto ui = m_ui.data();
    // The database settings list already names the page
    VaultSettingsPage page(this, {});
    page.addWidget(ui->messageWidget);

    // The saved configurations, with the button to remove the selected one beside them
    auto configurations = page.addSection(ui->syncCommandsGroupBox);
    auto list = new QWidget(this);
    auto listLayout = new QHBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(12);
    ui->settingsListWidget->setFixedHeight(ListHeight);
    listLayout->addWidget(ui->settingsListWidget, 1);
    listLayout->addWidget(ui->removeSettingsButton, 0, Qt::AlignTop);
    configurations->addWidget(list);

    // Fields share one width so they line up down the cards; commands keep the full width for their length
    for (auto field : {ui->nameLineEdit, ui->serverUrlEdit, ui->serverDatabaseIdEdit, ui->serverTokenEdit}) {
        field->setSizePolicy(QSizePolicy::Fixed, field->sizePolicy().verticalPolicy());
        field->setFixedWidth(FieldWidth);
    }
    for (auto spinBox : {ui->serverTimeoutSec, ui->downloadTimeoutSec, ui->uploadTimeoutSec}) {
        spinBox->setFixedWidth(NumberWidth);
    }
    for (auto input : {ui->inputForDownload, ui->inputForUpload}) {
        input->setFixedHeight(InputHeight);
    }

    auto configuration = page.addSection(ui->commandSettingsGroupBox);
    configuration->addSetting(ui->label, {ui->nameLineEdit, ui->saveSettingsButton});
    configuration->addSetting(ui->typeLabel, ui->typeComboBox);
    // The sync server's settings show while it is the type
    for (auto card :
         {configuration->addSetting(ui->serverUrlLabel, ui->serverUrlEdit),
          configuration->addSetting(
              ui->serverDatabaseIdLabel, ui->serverDatabaseIdEdit, ui->serverDatabaseIdEdit->placeholderText()),
          configuration->addSetting(ui->serverTokenLabel, ui->serverTokenEdit, ui->serverTokenEdit->placeholderText()),
          configuration->addSetting(ui->serverTimeoutLabel, {ui->serverTimeoutSec, ui->testServerButton}),
          configuration->addToggle(ui->autoSyncCheckBox, ui->autoSyncCheckBox->toolTip())}) {
        card->follow(ui->serverSettingsWidget);
    }
    // Their hints show as the cards' descriptions, so the fields don't repeat them
    ui->serverDatabaseIdEdit->setPlaceholderText({});
    ui->serverTokenEdit->setPlaceholderText({});

    // The commands show while they are the type, with a pivot between downloading and uploading in place of the tabs
    auto commands = page.addSection();
    commands->follow(ui->commandTabWidget);
    auto direction = new QTabBar(this);
    direction->setObjectName("commandDirectionSelector");
    direction->setDrawBase(false);
    direction->setExpanding(false);
    for (int i = 0; i < ui->commandTabWidget->count(); ++i) {
        direction->addTab(ui->commandTabWidget->tabText(i));
    }
    direction->setCurrentIndex(ui->commandTabWidget->currentIndex());
    connect(direction, &QTabBar::currentChanged, ui->commandTabWidget, &QTabWidget::setCurrentIndex);
    connect(ui->commandTabWidget, &QTabWidget::currentChanged, direction, &QTabBar::setCurrentIndex);
    commands->addHeaderWidget(direction);
    ui->commandTabWidget->tabBar()->hide();

    const struct
    {
        QWidget* tab;
        QLabel* commandLabel;
        QWidget* command;
        QWidget* test;
        QLabel* inputLabel;
        QWidget* input;
        QLabel* timeoutLabel;
        QWidget* timeout;
    } directions[] = {
        {ui->downloadTab,
         ui->downloadCommandLabel,
         ui->downloadCommand,
         ui->testDownloadCommandButton,
         ui->downloadCommandInputLabel,
         ui->inputForDownload,
         ui->downloadTimeoutLabel,
         ui->downloadTimeoutSec},
        {ui->uploadTab,
         ui->uploadCommandLabel,
         ui->uploadCommand,
         nullptr,
         ui->uploadCommandInputLabel,
         ui->inputForUpload,
         ui->uploadTimeoutLabel,
         ui->uploadTimeoutSec},
    };
    for (const auto& d : directions) {
        QWidgetList commandControls{d.command};
        if (d.test) {
            commandControls << d.test;
        }
        auto command = commands->addSetting(d.commandLabel, commandControls);
        // The input is several lines, so it takes the card's width below its caption
        d.inputLabel->setBuddy(d.input);
        auto input = commands->addSetting(d.inputLabel, QWidgetList{});
        input->setCollapsible(false);
        input->addWidget(d.input);
        auto timeout = commands->addSetting(d.timeoutLabel, d.timeout);
        for (auto card : {command, input, timeout}) {
            card->follow(d.tab);
        }
    }

    page.finish();
}

DatabaseSettingsWidgetRemote::~DatabaseSettingsWidgetRemote() = default;

void DatabaseSettingsWidgetRemote::initialize()
{
    clearFields();
    m_remoteSettings->setDatabase(m_db);
    updateSettingsList();
    if (m_ui->settingsListWidget->count() > 0) {
        m_ui->settingsListWidget->setCurrentRow(0);
        m_ui->removeSettingsButton->setEnabled(true);
    } else {
        m_ui->removeSettingsButton->setDisabled(true);
    }
}

void DatabaseSettingsWidgetRemote::uninitialize()
{
}

bool DatabaseSettingsWidgetRemote::saveSettings()
{
    if (m_modified) {
        auto ans = MessageBox::question(this,
                                        tr("Save Remote Settings"),
                                        tr("You have unsaved changes. Do you want to save them?"),
                                        MessageBox::Save | MessageBox::Discard | MessageBox::Cancel,
                                        MessageBox::Save);
        if (ans == MessageBox::Save) {
            saveCurrentSettings();
        } else if (ans == MessageBox::Cancel) {
            return false;
        }
    }

    m_remoteSettings->saveSettings();
    return true;
}

void DatabaseSettingsWidgetRemote::saveCurrentSettings()
{
    QString name = m_ui->nameLineEdit->text();
    if (name.isEmpty()) {
        m_ui->messageWidget->showMessage(tr("Name cannot be empty."), MessageWidget::Warning);
        return;
    }

    QScopedPointer<RemoteParams> params(paramsFromFields());
    if (params->type == RemoteParams::Type::SyncServer) {
        QUrl url(params->serverUrl.trimmed());
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
            m_ui->messageWidget->showMessage(tr("Enter a valid server URL starting with https://."),
                                             MessageWidget::Warning);
            return;
        }
        if (params->serverDatabaseId.trimmed().isEmpty() || params->serverToken.trimmed().isEmpty()) {
            m_ui->messageWidget->showMessage(tr("Database ID and access token cannot be empty."),
                                             MessageWidget::Warning);
            return;
        }
        if (url.scheme() == "http" && url.host() != "localhost" && url.host() != "127.0.0.1" && url.host() != "::1") {
            m_ui->messageWidget->showMessage(
                tr("Warning: the access token will be sent unencrypted. Use https:// for servers other than "
                   "localhost."),
                MessageWidget::Warning);
        }
    }

    // Only one remote syncs automatically
    if (params->autoSync) {
        for (auto* other : m_remoteSettings->getAllRemoteParams()) {
            if (other->name != params->name) {
                other->autoSync = false;
            }
        }
    }

    m_remoteSettings->addRemoteParams(params.take());
    updateSettingsList();

    auto item = findItemByName(name);
    m_ui->settingsListWidget->setCurrentItem(item);
    m_ui->removeSettingsButton->setEnabled(true);
    m_modified = false;
}

QListWidgetItem* DatabaseSettingsWidgetRemote::findItemByName(const QString& name)
{
    return m_ui->settingsListWidget->findItems(name, Qt::MatchExactly).first();
}

void DatabaseSettingsWidgetRemote::removeCurrentSettings()
{
    m_remoteSettings->removeRemoteParams(m_ui->nameLineEdit->text());
    updateSettingsList();
    if (!m_remoteSettings->getAllRemoteParams().empty()) {
        m_ui->settingsListWidget->setCurrentRow(0);
        m_ui->removeSettingsButton->setEnabled(true);
    } else {
        clearFields();
        m_ui->removeSettingsButton->setDisabled(true);
    }
}

void DatabaseSettingsWidgetRemote::editCurrentSettings()
{
    if (!m_ui->settingsListWidget->currentItem()) {
        return;
    }

    QString name = m_ui->settingsListWidget->currentItem()->text();
    auto* params = m_remoteSettings->getRemoteParams(name);
    if (!params) {
        return;
    }

    m_ui->nameLineEdit->setText(params->name);
    m_ui->typeComboBox->setCurrentIndex(params->type == RemoteParams::Type::SyncServer ? 1 : 0);
    m_ui->serverUrlEdit->setText(params->serverUrl);
    m_ui->serverDatabaseIdEdit->setText(params->serverDatabaseId);
    m_ui->serverTokenEdit->setText(params->serverToken);
    m_ui->autoSyncCheckBox->setChecked(params->autoSync);
    m_ui->serverTimeoutSec->setValue(params->downloadTimeoutMsec / 1000);
    m_ui->downloadCommand->setText(params->downloadCommand);
    m_ui->inputForDownload->setPlainText(params->downloadInput);
    m_ui->downloadTimeoutSec->setValue(params->downloadTimeoutMsec / 1000);
    m_ui->uploadCommand->setText(params->uploadCommand);
    m_ui->inputForUpload->setPlainText(params->uploadInput);
    m_ui->uploadTimeoutSec->setValue(params->uploadTimeoutMsec / 1000);
    m_modified = false;
}

void DatabaseSettingsWidgetRemote::updateSettingsList()
{
    m_ui->settingsListWidget->clear();
    for (auto params : m_remoteSettings->getAllRemoteParams()) {
        auto* item = new QListWidgetItem(m_ui->settingsListWidget);
        item->setText(params->name);
        m_ui->settingsListWidget->addItem(item);
    }
}

void DatabaseSettingsWidgetRemote::clearFields()
{
    m_ui->nameLineEdit->setText("");
    m_ui->typeComboBox->setCurrentIndex(0);
    m_ui->serverUrlEdit->setText("");
    m_ui->serverDatabaseIdEdit->setText("");
    m_ui->serverTokenEdit->setText("");
    m_ui->autoSyncCheckBox->setChecked(false);
    m_ui->serverTimeoutSec->setValue(30);
    m_ui->downloadCommand->setText("");
    m_ui->inputForDownload->setPlainText("");
    m_ui->downloadTimeoutSec->setValue(10);
    m_ui->uploadCommand->setText("");
    m_ui->inputForUpload->setPlainText("");
    m_ui->uploadTimeoutSec->setValue(10);
    m_modified = false;
}

RemoteParams* DatabaseSettingsWidgetRemote::paramsFromFields() const
{
    auto* params = new RemoteParams();
    params->name = m_ui->nameLineEdit->text();
    if (m_ui->typeComboBox->currentIndex() == 1) {
        params->type = RemoteParams::Type::SyncServer;
        params->serverUrl = m_ui->serverUrlEdit->text().trimmed();
        params->serverDatabaseId = m_ui->serverDatabaseIdEdit->text().trimmed();
        params->serverToken = m_ui->serverTokenEdit->text().trimmed();
        params->autoSync = m_ui->autoSyncCheckBox->isChecked();
        params->downloadTimeoutMsec = m_ui->serverTimeoutSec->value() * 1000;
        params->uploadTimeoutMsec = params->downloadTimeoutMsec;
    } else {
        params->downloadCommand = m_ui->downloadCommand->text();
        params->downloadInput = m_ui->inputForDownload->toPlainText();
        params->downloadTimeoutMsec = m_ui->downloadTimeoutSec->value() * 1000;
        params->uploadCommand = m_ui->uploadCommand->text();
        params->uploadInput = m_ui->inputForUpload->toPlainText();
        params->uploadTimeoutMsec = m_ui->uploadTimeoutSec->value() * 1000;
    }
    return params;
}

void DatabaseSettingsWidgetRemote::updateTypeVisibility()
{
    bool isServer = m_ui->typeComboBox->currentIndex() == 1;
    m_ui->commandTabWidget->setVisible(!isServer);
    m_ui->serverSettingsWidget->setVisible(isServer);
}

void DatabaseSettingsWidgetRemote::testDownload()
{
    QScopedPointer<RemoteParams> params(paramsFromFields());
    bool isServer = params->type == RemoteParams::Type::SyncServer;

    if (!isServer && params->downloadCommand.isEmpty()) {
        m_ui->messageWidget->showMessage(tr("Download command cannot be empty."), MessageWidget::Warning);
        return;
    }
    if (isServer && (params->serverUrl.isEmpty() || params->serverDatabaseId.isEmpty())) {
        m_ui->messageWidget->showMessage(tr("Server URL and database ID cannot be empty."), MessageWidget::Warning);
        return;
    }

    QScopedPointer<RemoteHandler> remoteHandler(new RemoteHandler(this));
    RemoteHandler::RemoteResult result = remoteHandler->download(params.data());
    if (!result.success) {
        m_ui->messageWidget->showMessage(tr("Download failed with error: %1").arg(result.errorMessage),
                                         MessageWidget::Error);
        return;
    }

    if (result.remoteMissing) {
        m_ui->messageWidget->showMessage(
            tr("Connected to the sync server. The database does not exist there yet and will be uploaded on the "
               "first sync."),
            MessageWidget::Positive);
        return;
    }

    if (!QFile::exists(result.filePath)) {
        m_ui->messageWidget->showMessage(tr("Download finished, but file %1 could not be found.").arg(result.filePath),
                                         MessageWidget::Error);
        return;
    }

    // The test download is not used for anything, don't leave a copy of the database behind
    QFile::remove(result.filePath);
    m_ui->messageWidget->showMessage(tr("Download successful."), MessageWidget::Positive);
}
