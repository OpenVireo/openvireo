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

#include "ImportWizardPageReview.h"
#include "ui_ImportWizardPageReview.h"

#include "core/Database.h"
#include "core/Group.h"
#include "core/KdbxImport.h"
#include "format/BitwardenReader.h"
#include "format/KeePass1Reader.h"
#include "format/OPUXReader.h"
#include "format/OpVaultReader.h"
#include "format/ProtonPassReader.h"
#include "gui/csvImport/CsvImportWidget.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"
#include "gui/wizard/ImportWizard.h"

#include "cli/Utils.h"
#include "keys/FileKey.h"
#include "keys/PasswordKey.h"

#include <QBoxLayout>
#include <QDir>
#include <QHeaderView>
#include <QTableWidget>

#include "gui/remote/RemoteSettings.h"

#include <components/status_info/InfoBar.h>

struct RemoteParams;

ImportWizardPageReview::ImportWizardPageReview(QWidget* parent)
    : QWizardPage(parent)
    , m_ui(new Ui::ImportWizardPageReview)
    , m_remoteHandler(new RemoteHandler(this))
{
}

ImportWizardPageReview::~ImportWizardPageReview()
{
}

void ImportWizardPageReview::initializePage()
{
    m_db.reset();

    // Reset the widget in case we changed the import type
    for (auto child : children()) {
        delete child;
    }
    m_ui->setupUi(this);

    auto filename = field("ImportFile").toString();
    m_ui->filenameLabel->setText(filename);

    m_ui->messageWidget->hideMessage();
    m_ui->messageWidget->setAnimate(false);
    m_ui->messageWidget->setCloseButtonVisible(false);
    if (FluentTheme::vaultLayoutEnabled()) {
        setupVaultLayout();
    }

    auto importType = field("ImportType").toInt();
    switch (importType) {
    case ImportWizard::IMPORT_CSV:
        setupCsvImport(filename);
        break;
    case ImportWizard::IMPORT_OPVAULT:
        m_db = importOPVault(filename, field("ImportPassword").toString());
        break;
    case ImportWizard::IMPORT_OPUX:
        m_db = importOPUX(filename);
        break;
    case ImportWizard::IMPORT_KEEPASS1:
        m_db = importKeePass1(filename, field("ImportPassword").toString(), field("ImportKeyFile").toString());
        break;
    case ImportWizard::IMPORT_KEEPASS2: {
        QString error;
        m_db = KdbxImport::read(filename, field("ImportPassword").toString(), field("ImportKeyFile").toString(), &error);
        if (!m_db) {
            showError(error);
        }
        break;
    }
    case ImportWizard::IMPORT_BITWARDEN:
        m_db = importBitwarden(filename, field("ImportPassword").toString());
        break;
    case ImportWizard::IMPORT_PROTONPASS:
        m_db = importProtonPass(filename);
        break;
    case ImportWizard::IMPORT_REMOTE:
        m_db = importRemote(field("DownloadCommand").toString(),
                            field("DownloadInput").toString(),
                            field("ImportPassword").toString(),
                            field("ImportKeyFile").toString());
        break;
    case ImportWizard::IMPORT_SYNC_SERVER:
        m_ui->filenameLabel->setText(field("SyncServerDatabaseId").toString());
        m_db = importSyncServer(field("SyncServerUrl").toString(),
                                field("SyncServerToken").toString(),
                                field("SyncServerDatabaseId").toString(),
                                field("ImportPassword").toString(),
                                field("ImportKeyFile").toString());
        break;
    default:
        break;
    }

    setupDatabasePreview();
}

bool ImportWizardPageReview::validatePage()
{
    if (isCsvImport()) {
        m_db = m_csvWidget->buildDatabase();
    }
    return !m_db.isNull();
}

bool ImportWizardPageReview::isComplete() const
{
    // Nothing to import when reading the source failed; a CSV file is only read on finishing
    return isCsvImport() || m_db;
}

QSharedPointer<Database> ImportWizardPageReview::database()
{
    return m_db;
}

// In the Vault Index layout, the page is the content of a modal, which names the step. The source heads a flat
// preview, and errors show in a Fluent info bar.
void ImportWizardPageReview::setupVaultLayout()
{
    m_ui->messageWidget->hide();
    m_errorBar = new fluent::status_info::InfoBar(this);
    m_errorBar->setObjectName("importErrorBar");
    m_errorBar->setSeverity(fluent::status_info::InfoBar::Error);
    m_errorBar->setIsClosable(false);
    m_errorBar->setIsOpen(true);
    m_errorBar->setSizePolicy(QSizePolicy::Expanding, m_errorBar->sizePolicy().verticalPolicy());
    m_errorBar->hide();
    m_ui->verticalLayout->insertWidget(0, m_errorBar);
    m_ui->verticalLayout->setContentsMargins(0, 0, 0, 0);

    m_ui->scrollArea->setFrameShape(QFrame::NoFrame);
    m_ui->scrollArea->setMinimumSize(0, 0);
    m_ui->scrollArea->viewport()->setAutoFillBackground(false);
    m_ui->scrollAreaContents->setAutoFillBackground(false);
    m_ui->scrollAreaContents->layout()->setSpacing(4);

    m_ui->filenameLabel->setFont(Vault::font(Typography::FontRole::BodyStrong));
    m_ui->filenameLabel->setWordWrap(true);
    m_ui->previewLabel->setFont(Vault::font(Typography::FontRole::Body));
    // Style sheets rather than palettes, since the modal resets the palettes of its content
    Vault::followTheme(m_ui->previewLabel, [label = m_ui->previewLabel] {
        label->setStyleSheet(
            QStringLiteral("color: %1;").arg(FluentTheme::palette().textSecondary.name(QColor::HexArgb)));
    });
}

void ImportWizardPageReview::showError(const QString& message)
{
    if (m_errorBar) {
        m_errorBar->setTitle(tr("Import failed"));
        m_errorBar->setMessage(message);
        m_errorBar->show();
        return;
    }
    m_ui->messageWidget->showMessage(message, MessageWidget::Error, -1);
}

void ImportWizardPageReview::setupCsvImport(const QString& filename)
{
    // No need for this label with CSV
    m_ui->previewLabel->hide();

    m_csvWidget = new CsvImportWidget();
    connect(m_csvWidget, &CsvImportWidget::message, m_ui->messageWidget, [this](QString message) {
        if (!message.isEmpty()) {
            showError(message);
        } else if (m_errorBar) {
            m_errorBar->hide();
        } else {
            m_ui->messageWidget->hideMessage();
        }
    });

    m_csvWidget->load(filename);
}

void ImportWizardPageReview::setupDatabasePreview()
{
    // CSV preview is handled by the import widget
    if (isCsvImport()) {
        // Qt does not automatically resize a QScrollWidget in a QWizard...
        m_ui->scrollAreaContents->layout()->addWidget(m_csvWidget);
        m_ui->scrollArea->setMinimumSize(m_csvWidget->width() + 50, m_csvWidget->height() + 100);
        return;
    }

    if (!m_db) {
        m_ui->scrollArea->setVisible(false);
        return;
    }

    auto entryList = m_db->rootGroup()->entriesRecursive();
    if (m_errorBar) {
        m_ui->previewLabel->setText(tr("%n entries to import", "", entryList.count()));
    } else {
        m_ui->previewLabel->setText(tr("Entry count: %1").arg(entryList.count()));
    }

    QStringList headerLabels({tr("Group"), tr("Title"), tr("Username"), tr("Password"), tr("Url")});

    auto tableWidget = new QTableWidget(entryList.count(), headerLabels.count());
    tableWidget->setHorizontalHeaderLabels(headerLabels);

    int row = 0;
    for (auto entry : entryList) {
        QList items({new QTableWidgetItem(entry->group()->name()),
                     new QTableWidgetItem(entry->title()),
                     new QTableWidgetItem(entry->username()),
                     new QTableWidgetItem(entry->password()),
                     new QTableWidgetItem(entry->url())});
        int column = 0;
        for (auto item : items) {
            tableWidget->setItem(row, column++, item);
        }
        ++row;
    }

    tableWidget->setSortingEnabled(true);
    tableWidget->setSelectionMode(QTableWidget::NoSelection);
    tableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget->setWordWrap(true);
    tableWidget->horizontalHeader()->setMaximumSectionSize(200);
    tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableWidget->horizontalHeader()->setStretchLastSection(true);
    if (m_errorBar) {
        tableWidget->setObjectName("importPreviewTable");
        tableWidget->setShowGrid(false);
        tableWidget->setAlternatingRowColors(true);
        tableWidget->verticalHeader()->hide();
        tableWidget->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_ui->scrollAreaContents->layout()->addItem(new QSpacerItem(0, 8, QSizePolicy::Minimum, QSizePolicy::Fixed));
    }

    m_ui->scrollAreaContents->layout()->addWidget(tableWidget);
}

QSharedPointer<Database> ImportWizardPageReview::importOPUX(const QString& filename)
{
    OPUXReader reader;
    auto db = reader.convert(filename);
    if (reader.hasError()) {
        showError(reader.errorString());
    }
    return db;
}

QSharedPointer<Database> ImportWizardPageReview::importBitwarden(const QString& filename, const QString& password)
{
    BitwardenReader reader;
    auto db = reader.convert(filename, password);
    if (reader.hasError()) {
        showError(reader.errorString());
    }
    return db;
}

QSharedPointer<Database> ImportWizardPageReview::importOPVault(const QString& filename, const QString& password)
{
    OpVaultReader reader;
    QDir opVault(filename);
    auto db = reader.convert(opVault, password);
    if (reader.hasError()) {
        showError(reader.errorString());
    }
    return db;
}

QSharedPointer<Database>
ImportWizardPageReview::importKeePass1(const QString& filename, const QString& password, const QString& keyfile)
{
    KeePass1Reader reader;

    // TODO: Handle case of empty password?

    auto db = reader.readDatabase(filename, password, keyfile);
    if (reader.hasError()) {
        showError(reader.errorString());
    }

    return db;
}

QSharedPointer<Database> ImportWizardPageReview::importProtonPass(const QString& filename)
{
    ProtonPassReader reader;
    auto db = reader.convert(filename);
    if (reader.hasError()) {
        showError(reader.errorString());
    }
    return db;
}

bool ImportWizardPageReview::isCsvImport() const
{
    return m_csvWidget && field("ImportType").toInt() == ImportWizard::IMPORT_CSV;
}

QSharedPointer<Database> ImportWizardPageReview::importRemote(const QString& downloadCommand,
                                                              const QString& downloadInput,
                                                              const QString& password,
                                                              const QString& keyfile)
{
    auto* params = new RemoteParams();
    params->downloadCommand = downloadCommand;
    params->downloadInput = downloadInput;

    auto result = m_remoteHandler->download(params);
    delete params;

    if (!result.success) {
        showError(result.errorMessage);
        return {};
    }

    auto key = buildKey(password, keyfile);

    QString error;
    QSharedPointer<Database> remoteDb = QSharedPointer<Database>::create();
    if (!remoteDb->open(result.filePath, key, &error)) {
        QFile::remove(result.filePath);
        showError(tr("Could not open remote database. Password or key file may be incorrect."));
        return {};
    }
    remoteDb->markAsTemporaryDatabase();

    return remoteDb;
}

QSharedPointer<Database> ImportWizardPageReview::importSyncServer(const QString& serverUrl,
                                                                  const QString& token,
                                                                  const QString& databaseId,
                                                                  const QString& password,
                                                                  const QString& keyfile)
{
    RemoteParams params;
    params.type = RemoteParams::Type::SyncServer;
    params.serverUrl = serverUrl.trimmed();
    params.serverToken = token.trimmed();
    params.serverDatabaseId = databaseId.trimmed();
    params.downloadTimeoutMsec = 30000;
    params.uploadTimeoutMsec = 30000;

    auto result = m_remoteHandler->download(&params);
    if (!result.success) {
        showError(result.errorMessage);
        return {};
    }
    if (result.remoteMissing) {
        showError(tr("The database '%1' does not exist on the sync server.").arg(params.serverDatabaseId));
        return {};
    }

    QString error;
    auto db = QSharedPointer<Database>::create();
    if (!db->open(result.filePath, buildKey(password, keyfile), &error)) {
        QFile::remove(result.filePath);
        showError(tr("Could not open the database from the sync server. Password or key file may be incorrect."));
        return {};
    }

    // Link the imported database to the server so Remote Sync works right away.
    // The device that uploaded it has usually configured this already.
    RemoteSettings remoteSettings(db);
    auto normalizedUrl = [](QString url) {
        url = url.trimmed();
        while (url.endsWith('/')) {
            url.chop(1);
        }
        return url;
    };
    bool linked = false;
    for (const auto existing : remoteSettings.getAllRemoteParams()) {
        if (existing->type == RemoteParams::Type::SyncServer
            && normalizedUrl(existing->serverUrl) == normalizedUrl(params.serverUrl)
            && existing->serverDatabaseId == params.serverDatabaseId) {
            // Keep the token that worked for this import
            existing->serverToken = params.serverToken;
            linked = true;
        }
    }
    if (!linked) {
        auto* link = new RemoteParams(params);
        link->name = tr("Sync Server (%1)").arg(params.serverDatabaseId);
        for (int i = 2; remoteSettings.getRemoteParams(link->name); ++i) {
            link->name = tr("Sync Server (%1) %2").arg(params.serverDatabaseId).arg(i);
        }
        remoteSettings.addRemoteParams(link);
    }
    remoteSettings.saveSettings();

    if (field("ImportIntoType").toInt() == ImportWizard::TEMPORARY_DATABASE) {
        db->markAsTemporaryDatabase();
    } else {
        // Don't leave the download in the temp directory; the first save asks where to keep the local copy
        QFile::remove(result.filePath);
        db->setFilePath({});
    }

    return db;
}

QSharedPointer<CompositeKey> ImportWizardPageReview::buildKey(const QString& password, const QString& keyfile)
{
    auto key = QSharedPointer<CompositeKey>::create();

    if (!password.isEmpty()) {
        key->addKey(QSharedPointer<PasswordKey>::create(password));
    }
    if (!keyfile.isEmpty()) {
        QSharedPointer<FileKey> fileKey = QSharedPointer<FileKey>::create();
        if (Utils::loadFileKey(keyfile, fileKey)) {
            key->addKey(fileKey);
        } else {
            showError(tr("Could not load key file."));
        }
    }
    return key;
}
