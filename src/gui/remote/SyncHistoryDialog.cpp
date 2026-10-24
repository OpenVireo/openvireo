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

#include "SyncHistoryDialog.h"

#include "SyncServerClient.h"

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
                                 .arg(params.remoteDatabaseId()),
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
    m_client->history([this](const SyncServerClient::Response& response) {
        if (response.status != 200) {
            showStatus(SyncServerClient::errorMessage(response));
            return;
        }
        const auto versions = QJsonDocument::fromJson(response.body).object().value("versions").toArray();
        bool current = true;
        for (const auto& value : versions) {
            const auto version = value.toObject();
            const auto revision = version.value("revision").toVariant().toLongLong();
            const auto saved = QDateTime::fromString(version.value("updatedAt").toString(), Qt::ISODateWithMs);
            auto* item = new QTreeWidgetItem(m_versions);
            // The server lists the newest version first, which is the one devices sync with
            item->setText(0, current ? tr("%1 (current)").arg(revision) : QString::number(revision));
            item->setText(1, QLocale().toString(saved.toLocalTime(), QLocale::ShortFormat));
            item->setText(2, QLocale().formattedDataSize(version.value("size").toVariant().toLongLong()));
            item->setData(0, RevisionRole, revision);
            current = false;
        }
        showStatus(versions.isEmpty() ? tr("The server has no versions of this database.") : QString());
    });
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

    m_client->downloadRevision(revision, [this, revision](const SyncServerClient::Response& response) {
        setCanOpen(true);
        if (response.status != 200) {
            showStatus(SyncServerClient::errorMessage(response));
            return;
        }
        // A folder of its own, so the name can say which version it is; it outlives the dialog
        // because the tab keeps using the file
        const auto dir = QDir::temp().filePath("OpenVireo-version-" + QUuid::createUuid().toString(QUuid::Id128));
        const auto filePath =
            QDir(dir).filePath(tr("%1 (version %2).kdbx").arg(m_params.remoteDatabaseId()).arg(revision));
        QFile file(filePath);
        if (!QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly)
            || file.write(response.body) != response.body.size()) {
            showStatus(tr("Could not save the downloaded version: %1").arg(file.errorString()));
            return;
        }
        file.close();
        emit openRequested(filePath);
        accept();
    });
}
