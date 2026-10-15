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

#ifndef KEEPASSXC_SYNCHISTORYDIALOG_H
#define KEEPASSXC_SYNCHISTORYDIALOG_H

#include "RemoteSettings.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;
class SyncServerClient;

/**
 * The versions of a database kept on the sync server. Opening one downloads it to a temporary
 * file and opens it in its own tab, where entries can be copied from it or it can be merged
 * into the current database. Nothing is changed on the server.
 */
class SyncHistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SyncHistoryDialog(const RemoteParams& params, QWidget* parent = nullptr);

signals:
    // A version was downloaded to `filePath`
    void openRequested(const QString& filePath);

private:
    void loadVersions();
    void openSelected();
    void showStatus(const QString& text);

    RemoteParams m_params;
    SyncServerClient* m_client;
    QTreeWidget* m_versions;
    QLabel* m_status;
    QPushButton* m_openButton;
};

#endif // KEEPASSXC_SYNCHISTORYDIALOG_H
