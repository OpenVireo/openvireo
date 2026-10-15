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

#ifndef KEEPASSXC_VAULTIMPORTMODAL_H
#define KEEPASSXC_VAULTIMPORTMODAL_H

#include "VaultModal.h"

class ImportWizard;
class ImportWizardPageSelect;
class VaultStepList;

/**
 * Shows the import wizard in a modal card over the main window in the Vault
 * Index layout. Choosing the source and filling in its details are steps of
 * their own, followed by the review, listed in a sidebar with Back and
 * Continue in the footer. The wizard keeps its pages and its logic, and
 * finishing or cancelling it closes the modal, which deletes it.
 */
class VaultImportModal : public VaultModal
{
    Q_OBJECT

public:
    enum Step
    {
        SourceStep,
        DetailsStep,
        ReviewStep,
    };

    VaultImportModal(QWidget* window, ImportWizard* wizard);
    ~VaultImportModal() override;

    int currentStep() const;

public slots:
    void next();
    void back();

protected:
    void accept() override;
    void reject() override;
    void prepare() override;
    void applyColors() override;

private:
    void showStep(int step);
    void updateNextButton();

    ImportWizard* m_wizard;
    ImportWizardPageSelect* m_pageSelect;
    int m_current = SourceStep;

    VaultStepList* m_stepList;
    QLabel* m_stepTitle;
    QLabel* m_stepSubtitle;
    QPushButton* m_backButton;
    QPushButton* m_nextButton;
};

#endif // KEEPASSXC_VAULTIMPORTMODAL_H
