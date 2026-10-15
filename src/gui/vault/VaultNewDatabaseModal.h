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

#ifndef KEEPASSXC_VAULTNEWDATABASEMODAL_H
#define KEEPASSXC_VAULTNEWDATABASEMODAL_H

#include "VaultModal.h"

#include <QSharedPointer>

class Database;
class DatabaseSettingsWidget;
class QStackedWidget;
class VaultStepList;

/**
 * Creates a new database in a modal card over the main window, standing in for
 * the New Database wizard in the Vault Index layout. The wizard's pages are
 * steps listed in a sidebar, with Back and Continue in the footer; the last
 * step's Continue creates the database.
 */
class VaultNewDatabaseModal : public VaultModal
{
    Q_OBJECT

public:
    explicit VaultNewDatabaseModal(QWidget* window);
    ~VaultNewDatabaseModal() override;

    int currentStep() const;
    int stepCount() const;

public slots:
    // Saves the current step and moves on, or creates the database after the last step
    void next();
    void back();

signals:
    // The created database, or null when the modal was cancelled
    void finished(QSharedPointer<Database> db);

protected:
    void accept() override;
    void reject() override;
    void prepare() override;
    void applyColors() override;

private:
    struct Step
    {
        QString title;
        QString subtitle;
        DatabaseSettingsWidget* page;
    };

    void showStep(int index, bool load);

    QList<Step> m_steps;
    QSharedPointer<Database> m_db;
    int m_current = 0;

    VaultStepList* m_stepList;
    QLabel* m_stepTitle;
    QLabel* m_stepSubtitle;
    QStackedWidget* m_stack;
    QPushButton* m_backButton;
    QPushButton* m_nextButton;
};

#endif // KEEPASSXC_VAULTNEWDATABASEMODAL_H
