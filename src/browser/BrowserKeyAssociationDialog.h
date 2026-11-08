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

#ifndef KEEPASSXC_BROWSERKEYASSOCIATIONDIALOG_H
#define KEEPASSXC_BROWSERKEYASSOCIATIONDIALOG_H

#include <components/dialogs_flyouts/ContentDialog.h>

#include <functional>

class BrowserKeyAssociationHint;
class QAbstractButton;
class VaultLineEdit;

/**
 * A browser's request to connect to the database, in the Vault Index layout.
 *
 * Shows as a Fluent content dialog over the window, asking for a name for the
 * connection in a Fluent text field. Save and allow access leads as the accent
 * command and stays disabled until there is a name. A name that is already in
 * use is called out under the field, and the command then reads Replace and
 * allow access, so replacing the key needs no second prompt.
 */
class BrowserKeyAssociationDialog : public fluent::dialogs_flyouts::ContentDialog
{
    Q_OBJECT

public:
    BrowserKeyAssociationDialog(const QString& databaseName,
                                std::function<bool(const QString&)> nameInUse,
                                QWidget* window);

    // True when the dialog can show over the window, rather than the classic input dialog
    static bool canShowOver(QWidget* widget);

    // The name typed, trimmed
    QString name() const;
    // True when the name belongs to an existing connection, whose key saving replaces
    bool replacesExisting() const;
    // True while the helper text under the field warns, in the caution color
    bool hintIsCaution() const;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void updateName();

    std::function<bool(const QString&)> m_nameInUse;
    VaultLineEdit* m_nameEdit = nullptr;
    BrowserKeyAssociationHint* m_nameHint = nullptr;
    QAbstractButton* m_saveButton = nullptr;
    QString m_saveText;
    QString m_replaceText;
    QString m_defaultHint;
};

#endif // KEEPASSXC_BROWSERKEYASSOCIATIONDIALOG_H
