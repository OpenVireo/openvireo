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

#ifndef KEEPASSXC_SHAREDLINKSDIALOG_H
#define KEEPASSXC_SHAREDLINKSDIALOG_H

#include "account/ItemShare.h"

#include <QDialog>
#include <QPointer>

class MessageWidget;
class QCheckBox;
class QPushButton;
class QTreeWidget;

// The share links this account made (SPEC §22): what each came from, how often it was opened, Revoke,
// and making a link for some text. Links that ended stay listed for 30 days.
class SharedLinksDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharedLinksDialog(ItemShare* shares, QWidget* parent = nullptr);

    static QString describe(const ItemShare::Listing& share, const QDateTime& now);

private slots:
    void reload();
    void revokeSelected();
    void shareText();

private:
    void fill();

    QPointer<ItemShare> m_shares;
    QList<ItemShare::Listing> m_list;
    MessageWidget* m_message;
    QTreeWidget* m_tree;
    QCheckBox* m_showEnded;
    QPushButton* m_revoke;
};

#endif // KEEPASSXC_SHAREDLINKSDIALOG_H
