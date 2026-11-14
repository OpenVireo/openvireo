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

#ifndef KEEPASSXC_SHARELINKDIALOG_H
#define KEEPASSXC_SHARELINKDIALOG_H

#include "gui/MessageWidget.h"

#include <QDialog>
#include <QPointer>

class Entry;
class ItemShare;
class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class SquareSvgWidget;

namespace fluent::status_info
{
    class InfoBar;
}

// Makes a share link (SPEC §22) for the chosen fields of an entry of an account vault, or for some text
// when `entry` is null: anyone with the link can open a copy in a browser, without an account.
class ShareLinkDialog : public QDialog
{
    Q_OBJECT

public:
    ShareLinkDialog(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent = nullptr);

    // Opens the dialog, deleted once closed. In the Vault Index layout it is a modal card over the main window,
    // or a Fluent window of its own over another dialog.
    static ShareLinkDialog* present(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent);

private slots:
    void updateState();
    void create();

private:
    void setupClassicLayout();
    void setupVaultLayout();
    void addFieldChoices(QWidget* parent, QLayout* layout);
    void addTextFields(QWidget* parent, QLayout* layout);
    void addOptions(QWidget* parent);
    void showMessage(const QString& text, MessageWidget::MessageType type, bool persistent = false);
    void hideMessage();
    void showLink(const QString& url, const QString& expires);
    bool showAsModal() const;
    void presentAsModal();
    // The Fluent window takes its height at its width, since its text wraps
    void fitWindow();
    // Fit the window again after what it shows changed; nothing to do for the card over the main window
    void refitWindow();

    QPointer<ItemShare> m_shares;
    QString m_vaultId;
    QPointer<Entry> m_entry;
    QString m_blocked;
    bool m_vault;

    MessageWidget* m_message = nullptr;
    fluent::status_info::InfoBar* m_infoBar = nullptr;
    QWidget* m_formPage = nullptr;
    QWidget* m_resultPage = nullptr;
    QList<QCheckBox*> m_fieldBoxes;
    QWidget* m_totpWarning = nullptr;
    QLineEdit* m_title = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QComboBox* m_expiry = nullptr;
    QComboBox* m_opens = nullptr;
    QCheckBox* m_usePasscode = nullptr;
    QLineEdit* m_passcode = nullptr;
    QPushButton* m_regenerate = nullptr;
    QPushButton* m_createButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    QLineEdit* m_link = nullptr;
    QLabel* m_linkInfo = nullptr;
    QPushButton* m_copyLink = nullptr;
    QLabel* m_passcodeShown = nullptr;
    QPushButton* m_copyPasscode = nullptr;
    SquareSvgWidget* m_qr = nullptr;
    // The Fluent window's own title and commands, hidden when a modal card provides them
    QWidget* m_vaultTitle = nullptr;
    QWidget* m_vaultCommands = nullptr;
};

#endif // KEEPASSXC_SHARELINKDIALOG_H
