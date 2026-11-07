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

#include <QDialog>
#include <QPointer>

class Entry;
class ItemShare;
class MessageWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class SquareSvgWidget;

// Makes a share link (SPEC §22) for the chosen fields of an entry of an account vault, or for some text
// when `entry` is null: anyone with the link can open a copy in a browser, without an account.
class ShareLinkDialog : public QDialog
{
    Q_OBJECT

public:
    ShareLinkDialog(ItemShare* shares, const QString& vaultId, Entry* entry, QWidget* parent = nullptr);

private slots:
    void updateState();
    void create();

private:
    void showLink(const QString& url, const QString& expires);

    QPointer<ItemShare> m_shares;
    QString m_vaultId;
    QPointer<Entry> m_entry;
    QString m_blocked;

    MessageWidget* m_message;
    QStackedWidget* m_pages;
    QList<QCheckBox*> m_fieldBoxes;
    QLabel* m_totpWarning = nullptr;
    QLineEdit* m_title = nullptr;
    QPlainTextEdit* m_text = nullptr;
    QComboBox* m_expiry;
    QComboBox* m_opens;
    QCheckBox* m_usePasscode;
    QLineEdit* m_passcode;
    QPushButton* m_createButton;
    QLineEdit* m_link;
    QLabel* m_linkInfo;
    SquareSvgWidget* m_qr;
};

#endif // KEEPASSXC_SHARELINKDIALOG_H
