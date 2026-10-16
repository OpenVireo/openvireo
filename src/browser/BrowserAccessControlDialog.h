/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2013 Francois Ferrand
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KEEPASSXC_BROWSERACCESSCONTROLDIALOG_H
#define KEEPASSXC_BROWSERACCESSCONTROLDIALOG_H

#include <QDialog>
#include <QTableWidget>

class Entry;
class QAbstractButton;
class VaultButton;

namespace fluent::textfields
{
    class Label;
}

namespace Ui
{
    class BrowserAccessControlDialog;
}

enum SelectionType
{
    Selected,
    NonSelected,
    Disabled
};

class BrowserAccessControlDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BrowserAccessControlDialog(QWidget* parent = nullptr);
    ~BrowserAccessControlDialog() override;

    void setEntries(const QList<Entry*>& entriesToConfirm, const QString& urlString, bool httpAuth);
    bool remember() const;
    QList<QTableWidgetItem*> getEntries(SelectionType selectionType) const;

    int exec() override;

signals:
    void disableAccess(QTableWidgetItem* item);
    // A choice in the Vault Index layout changed, so whether there is anything to allow may have too
    void vaultChoicesChanged();

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void selectionChanged();

private:
    void addEntryToList(const Entry* entry, int row);
    bool areAllDisabled() const;
    QList<QTableWidgetItem*> getAllItems() const;
    void setupVaultLayout();
    void addVaultEntryRow(int row);
    void syncVaultEntryRow(int row);
    bool hasVaultChoice() const;
    int execVaultDialog();

private:
    QScopedPointer<Ui::BrowserAccessControlDialog> m_ui;
    QList<Entry*> m_entriesToConfirm;

    // In the Vault Index layout, the dialog shows as a Fluent content dialog over the window, holding these cards
    struct VaultEntryRow
    {
        QAbstractButton* choice;
        fluent::textfields::Label* username;
        VaultButton* disable;
    };
    QWidget* m_vaultCards = nullptr;
    fluent::textfields::Label* m_vaultMessage = nullptr;
    QWidget* m_vaultEntryList = nullptr;
    QList<VaultEntryRow> m_vaultEntryRows;
};

#endif // KEEPASSXC_BROWSERACCESSCONTROLDIALOG_H
