/*
 *  Copyright (C) 2023 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSXC_BROWSERPASSKEYSCONFIRMATIONDIALOG_H
#define KEEPASSXC_BROWSERPASSKEYSCONFIRMATIONDIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QTimer>

class Entry;
class QButtonGroup;

namespace Ui
{
    class BrowserPasskeysConfirmationDialog;
}

namespace fluent::status_info
{
    class ProgressBar;
}

namespace fluent::textfields
{
    class Label;
}

class BrowserPasskeysConfirmationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BrowserPasskeysConfirmationDialog(QWidget* parent = nullptr);
    ~BrowserPasskeysConfirmationDialog() override;

    void registerCredential(const QString& username,
                            const QString& relyingParty,
                            const QList<Entry*>& existingEntries,
                            int timeout);
    void authenticateCredential(const QList<Entry*>& entries, const QString& relyingParty, int timeout);
    Entry* getSelectedEntry() const;
    bool isPasskeyUpdated() const;

    int exec() override;

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void updatePasskey();
    void updateProgressBar();
    void updateSeconds();

private:
    void startCounter(int timeout);
    void updateTimeoutLabel();
    void updateEntriesToTable(const QList<Entry*>& entries);
    void setupVaultLayout();
    void setVaultText(const QString& title, const QString& message, const QString& hint = {});
    int execVaultDialog();

private:
    QScopedPointer<Ui::BrowserPasskeysConfirmationDialog> m_ui;
    QList<Entry*> m_entries;
    QTimer m_timer;
    int m_counter;
    bool m_passkeyUpdated;

    // In the Vault Index layout, the dialog shows as a Fluent content dialog over the window, holding these cards
    QWidget* m_vaultCards = nullptr;
    QString m_vaultTitle;
    fluent::textfields::Label* m_vaultMessage = nullptr;
    fluent::textfields::Label* m_vaultRelyingParty = nullptr;
    fluent::textfields::Label* m_vaultUsername = nullptr;
    fluent::textfields::Label* m_vaultHint = nullptr;
    fluent::textfields::Label* m_vaultTimeout = nullptr;
    fluent::status_info::ProgressBar* m_vaultProgress = nullptr;
    QWidget* m_vaultEntryList = nullptr;
    QButtonGroup* m_vaultEntryChoices = nullptr;
};

#endif // KEEPASSXC_BROWSERPASSKEYSCONFIRMATIONDIALOG_H
