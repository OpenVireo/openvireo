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
#include <QVariant>
#include <QWizardPage>

class QListWidgetItem;
class VaultComboBox;
class VaultRadioButton;

namespace Ui
{
    class ImportWizardPageSelect;
}

class ImportWizardPageSelect : public QWizardPage
{
    Q_OBJECT
    // Storage for the ImportType, ImportIntoType and ImportInto wizard fields. QWizard needs a real property
    // to write a field to; widgets without one only worked where Qt fell back to a dynamic property.
    Q_PROPERTY(int importType MEMBER m_importType)
    Q_PROPERTY(int importIntoType MEMBER m_importIntoType)
    Q_PROPERTY(QVariant importInto MEMBER m_importInto)

public:
    explicit ImportWizardPageSelect(QWidget* parent = nullptr);
    Q_DISABLE_COPY(ImportWizardPageSelect)
    ~ImportWizardPageSelect() override;

    void initializePage() override;
    bool validatePage() override;
    bool isComplete() const override;

    // The modal shows the choice of source and the rest of the page as separate steps
    void setSourceStep(bool sourceStep);
    QString sourceName() const;

    bool importAsCopies() const;
    QString vaultOrgId() const;

signals:
    // A source was double-clicked, which moves on to the next step
    void sourceActivated();

private slots:
    void itemSelected(QListWidgetItem* current, QListWidgetItem* previous);
    void chooseImportFile();
    void chooseKeyFile();
    void updateDatabaseChoices() const;
    void loadSyncServerDatabases();
    // Trades a pairing code typed in the token field for this device's token; false if that failed
    bool claimPairingCode();

private:
    void setupVaultLayout();
    QString importFileFilter();
    void setCredentialState(bool passwordEnabled, bool keyFileEnable = false);
    void setDownloadCommand(bool downloadCommandEnabled);
    void setSyncServer(bool syncServerEnabled);
    // "New vault in my account", and Merge / Add as copies for a .kdbx going into an existing database
    void setupImportChoices();
    void updateImportChoices();

    QScopedPointer<Ui::ImportWizardPageSelect> m_ui;
    int m_importType = 0;
    int m_importIntoType = 0;
    QVariant m_importInto;
    // The cards below the choice of source
    QWidget* m_details = nullptr;
    QWidget* m_newVaultRow = nullptr;
    VaultRadioButton* m_newVaultRadio = nullptr;
    VaultComboBox* m_vaultOrgChoice = nullptr;
    QWidget* m_kdbxModes = nullptr;
    VaultRadioButton* m_mergeRadio = nullptr;
    VaultRadioButton* m_copiesRadio = nullptr;
};

#endif
