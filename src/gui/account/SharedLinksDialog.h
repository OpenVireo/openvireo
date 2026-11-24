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

#ifndef OPENVIREO_SHAREDLINKSDIALOG_H
#define OPENVIREO_SHAREDLINKSDIALOG_H

#include "account/ItemShare.h"

#include <QDialog>
#include <QPointer>

class VaultButton;
class VaultInfoBar;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace fluent::navigation
{
    class SelectorBar;
}

// The share links this account made (SPEC §22): what each came from, how often it was opened, Revoke,
// and making a link for some text. Links that ended stay listed for 30 days. Over the main window it is a card
// (VaultModal) with Active and Ended tabs; each active link has its own Revoke, confirmed in its row.
class SharedLinksDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharedLinksDialog(ItemShare* shares, QWidget* parent = nullptr);

    // Opens the dialog as a card over the parent's window, or as a window when it can't be one
    static SharedLinksDialog* present(ItemShare* shares, QWidget* parent);

    // An active link's line under its title: when it expires, how often it was opened, and whether it has a passcode
    static QString describe(const ItemShare::Listing& share, const QDateTime& now);
    // Why a link ended, for its status
    static QString endReasonText(const QString& reason);

    // For tests: the links as the server listed them
    void setListing(const QList<ItemShare::Listing>& shares);

private slots:
    void reload();
    void shareText();

private:
    void fill();
    void revoke(const QString& id);
    QWidget* makeRow(const ItemShare::Listing& share, const QDateTime& now);
    void applyColors();

    QPointer<ItemShare> m_shares;
    QList<ItemShare::Listing> m_list;
    bool m_loaded = false;
    bool m_showEnded = false;
    QString m_confirming;
    VaultInfoBar* m_message;
    QLabel* m_intro;
    fluent::navigation::SelectorBar* m_tabs;
    QWidget* m_listPanel;
    QWidget* m_scroll;
    QWidget* m_rows;
    QVBoxLayout* m_rowsLayout;
    QWidget* m_empty;
    QLabel* m_emptyTitle;
    QLabel* m_emptyText;
    QPushButton* m_emptyAction;
    QWidget* m_buttonRow;
    QLabel* m_hint;
    VaultButton* m_shareText;
    QPushButton* m_closeButton;
};

#endif // OPENVIREO_SHAREDLINKSDIALOG_H
