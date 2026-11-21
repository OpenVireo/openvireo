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

#include "SyncHistoryDialog.h"

#include "SyncServerClient.h"

#include "crypto/Random.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace
{
    constexpr int RevisionRole = Qt::UserRole;
}

SyncHistoryDialog::SyncHistoryDialog(const RemoteParams& params, QWidget* parent)
    : QDialog(parent)
    , m_params(params)
    , m_history(SyncServerClient::accountVaultHistoryFor(params))
    , m_client(new SyncServerClient(params, this))
    , m_versions(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_openButton(new QPushButton(tr("Open"), this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Close, this))
{
    setObjectName("syncHistoryDialog");
    setWindowTitle(tr("Versions on %1").arg(params.name));
    resize(520, 380);

    auto* intro = new QLabel(tr("The sync server keeps the latest versions of \"%1\". Opening one shows it in its "
                                "own tab, where you can copy entries from it or merge it into this database. "
                                "Nothing on the server changes.")
                                 .arg(params.type == RemoteParams::Type::AccountVault && !params.name.isEmpty()
                                          ? params.name
                                          : params.remoteDatabaseId()),
                             this);
    intro->setWordWrap(true);

    m_versions->setObjectName("syncHistoryVersions");
    m_versions->setRootIsDecorated(false);
    m_versions->setHeaderLabels({tr("Version"), tr("Saved"), tr("Size")});
    m_versions->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_versions->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_versions, &QTreeWidget::itemSelectionChanged, this, [this] {
        setCanOpen(!m_versions->selectedItems().isEmpty());
    });
    connect(m_versions, &QTreeWidget::itemDoubleClicked, this, &SyncHistoryDialog::openSelected);

    m_status->setWordWrap(true);

    m_buttons->setObjectName("syncHistoryButtons");
    m_openButton->setObjectName("syncHistoryOpen");
    m_openButton->setEnabled(false);
    m_buttons->addButton(m_openButton, QDialogButtonBox::AcceptRole);
    connect(m_openButton, &QPushButton::clicked, this, &SyncHistoryDialog::openSelected);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_versions, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);

    loadVersions();
}

QDialogButtonBox* SyncHistoryDialog::buttonBox() const
{
    return m_buttons;
}

QTreeWidget* SyncHistoryDialog::versionList() const
{
    return m_versions;
}

void SyncHistoryDialog::setCanOpen(bool canOpen)
{
    m_openButton->setEnabled(canOpen);
    emit canOpenChanged(canOpen);
}

void SyncHistoryDialog::showStatus(const QString& text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void SyncHistoryDialog::loadVersions()
{
    showStatus(tr("Loading…"));
    if (m_history) {
        QPointer<SyncHistoryDialog> guard(this);
        m_history.list(m_params.vaultId,
                       [guard](const QList<AccountVaultSync::Version>& versions, const QString& error) {
                           if (!guard) {
                               return;
                           }
                           if (!error.isEmpty()) {
                               guard->showStatus(error);
                               return;
                           }
                           guard->showVersions(versions);
                       });
        return;
    }
    m_client->history([this](const SyncServerClient::Response& response) {
        if (response.status != 200) {
            showStatus(SyncServerClient::errorMessage(response));
            return;
        }
        QList<AccountVaultSync::Version> versions;
        bool current = true;
        for (const auto& value : QJsonDocument::fromJson(response.body).object().value("versions").toArray()) {
            const auto json = value.toObject();
            AccountVaultSync::Version version;
            version.revision = json.value("revision").toVariant().toLongLong();
            version.savedAt = QDateTime::fromString(json.value("updatedAt").toString(), Qt::ISODateWithMs);
            version.size = json.value("size").toVariant().toLongLong();
            // The server lists the newest version first, which is the one devices sync with
            version.current = current;
            current = false;
            versions << version;
        }
        showVersions(versions);
    });
}

void SyncHistoryDialog::showVersions(const QList<AccountVaultSync::Version>& versions)
{
    for (const auto& version : versions) {
        auto* item = new QTreeWidgetItem(m_versions);
        item->setText(0,
                      version.current ? tr("%1 (current)").arg(version.revision) : QString::number(version.revision));
        item->setText(1, QLocale().toString(version.savedAt.toLocalTime(), QLocale::ShortFormat));
        item->setText(2, version.size < 0 ? QString() : QLocale().formattedDataSize(version.size));
        item->setData(0, RevisionRole, version.revision);
    }
    showStatus(versions.isEmpty() ? tr("The server has no versions of this database.") : QString());
}

void SyncHistoryDialog::openSelected()
{
    const auto selected = m_versions->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    const auto revision = selected.first()->data(0, RevisionRole).toLongLong();
    setCanOpen(false);
    showStatus(tr("Downloading version %1…").arg(revision));

    if (m_history) {
        // The version comes under a one-off password: the vault key is random and never shown
        const auto password = QString::fromLatin1(randomGen()->randomArray(24).toBase64(QByteArray::Base64UrlEncoding));
        QPointer<SyncHistoryDialog> guard(this);
        m_history.open(m_params.vaultId,
                       revision,
                       password,
                       [guard, revision, password](const QByteArray& kdbx, const QString& error) {
                           if (!guard) {
                               return;
                           }
                           guard->setCanOpen(true);
                           if (!error.isEmpty()) {
                               guard->showStatus(error);
                               return;
                           }
                           guard->saveVersion(revision, kdbx, password);
                       });
        return;
    }
    m_client->downloadRevision(revision, [this, revision](const SyncServerClient::Response& response) {
        setCanOpen(true);
        if (response.status != 200) {
            showStatus(SyncServerClient::errorMessage(response));
            return;
        }
        saveVersion(revision, response.body, {});
    });
}

void SyncHistoryDialog::saveVersion(qint64 revision, const QByteArray& data, const QString& password)
{
    // A folder of its own, so the name can say which version it is; it outlives the dialog because the tab keeps
    // using the file
    const auto dir = QDir::temp().filePath("OpenVireo-version-" + QUuid::createUuid().toString(QUuid::Id128));
    const auto name = m_params.type == RemoteParams::Type::AccountVault && !m_params.name.isEmpty()
                          ? m_params.name
                          : m_params.remoteDatabaseId();
    const auto filePath = QDir(dir).filePath(tr("%1 (version %2).kdbx").arg(name).arg(revision));
    QFile file(filePath);
    if (!QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        showStatus(tr("Could not save the downloaded version: %1").arg(file.errorString()));
        return;
    }
    file.close();
    emit openRequested(filePath, password);
    accept();
}
