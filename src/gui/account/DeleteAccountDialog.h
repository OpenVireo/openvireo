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

#ifndef OPENVIREO_DELETEACCOUNTDIALOG_H
#define OPENVIREO_DELETEACCOUNTDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class AccountManager;
class VaultButton;
class VaultInfoBar;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

// Account › Delete Account… (SPEC §23): what happens to each vault, organization, the plan and share
// links; organizations you are the only owner of to hand over or delete first, and shared vaults to give
// away; then the typed email and master password. The account is locked at once and erased after the
// grace period. Over the main window it is a card (VaultModal).
class DeleteAccountDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeleteAccountDialog(AccountManager* account, QWidget* parent = nullptr);

    // Opens the dialog as a card over the parent's window, or as a window when it can't be one
    static DeleteAccountDialog* present(AccountManager* account, QWidget* parent);

    // One thing that changes: a glyph (or a letter for a vault), a title and a line under it
    struct SummaryRow
    {
        QString glyph;
        QString letter;
        QString title;
        QString detail;
        bool shared = false;
    };
    static QList<SummaryRow> summary(const QJsonObject& preview);

private slots:
    void reload();
    void deleteAccount();

private:
    void fill();
    void setBusy(bool busy);
    void updateDeleteButton();
    void applyColors();
    QWidget* summaryRow(const SummaryRow& row, bool first, QWidget* parent);

    QPointer<AccountManager> m_account;
    QJsonObject m_preview;
    VaultInfoBar* m_message;
    QLabel* m_intro;
    QLabel* m_summaryCaption;
    QWidget* m_summary;
    QVBoxLayout* m_summaryLayout;
    QLabel* m_billing;
    QWidget* m_actions;
    QVBoxLayout* m_actionsLayout;
    QLabel* m_confirmCaption;
    QLabel* m_confirmLabel;
    QLabel* m_passwordLabel;
    QLineEdit* m_confirm;
    QLineEdit* m_password;
    QScrollArea* m_scroll;
    QWidget* m_buttonRow;
    QLabel* m_footHint;
    QPushButton* m_cancel;
    VaultButton* m_delete;
    bool m_busy = false;
};

// Signed in to an account scheduled for deletion: Restore Account (with the master password) or Sign Out. Over the
// main window it is a card (VaultModal), with the deletion date on a calendar tile.
class PendingDeletionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PendingDeletionDialog(AccountManager* account, QWidget* parent = nullptr);

    // Opens the dialog as a card over the parent's window, or as a window when it can't be one
    static PendingDeletionDialog* present(AccountManager* account, QWidget* parent);

private:
    void applyColors();

    QPointer<AccountManager> m_account;
    VaultInfoBar* m_message;
    QLabel* m_intro;
    QLabel* m_month;
    QLabel* m_day;
    QWidget* m_calendar;
    QLabel* m_dateTitle;
    QLabel* m_dateDetail;
    QList<QLabel*> m_points;
    QList<QLabel*> m_pointIcons;
    QLabel* m_passwordLabel;
    QLineEdit* m_password;
    QWidget* m_buttonRow;
    QPushButton* m_signOut;
    QPushButton* m_restore;
};

#endif // OPENVIREO_DELETEACCOUNTDIALOG_H
