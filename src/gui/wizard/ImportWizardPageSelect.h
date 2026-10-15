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

#ifndef KEEPASSXC_IMPORTWIZARDPAGESELECT_H
#define KEEPASSXC_IMPORTWIZARDPAGESELECT_H

#include <QPointer>
#include <QWizardPage>

class QListWidgetItem;

namespace Ui
{
    class ImportWizardPageSelect;
}

class ImportWizardPageSelect : public QWizardPage
{
    Q_OBJECT

public:
    explicit ImportWizardPageSelect(QWidget* parent = nullptr);
    Q_DISABLE_COPY(ImportWizardPageSelect)
    ~ImportWizardPageSelect() override;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

    // In the Vault Index layout, the modal shows the choice of source and the rest of the page as separate steps
    void setSourceStep(bool sourceStep);
    QString sourceName() const;

signals:
    // A source was double-clicked, which moves on to the next step
    void sourceActivated();

private slots:
    void itemSelected(QListWidgetItem* current, QListWidgetItem* previous);
    void chooseImportFile();
    void chooseKeyFile();
    void updateDatabaseChoices() const;
    void loadSyncServerDatabases();

private:
    void setupVaultLayout();
    QString importFileFilter();
    void setCredentialState(bool passwordEnabled, bool keyFileEnable = false);
    void setDownloadCommand(bool downloadCommandEnabled);
    void setSyncServer(bool syncServerEnabled);

    QScopedPointer<Ui::ImportWizardPageSelect> m_ui;
    bool m_vaultLayout = false;
    // The cards below the choice of source in the Vault Index layout
    QWidget* m_details = nullptr;
};

#endif
