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

#ifndef KEEPASSXC_ENTRYLISTITEMDELEGATE_H
#define KEEPASSXC_ENTRYLISTITEMDELEGATE_H

#include <QStyledItemDelegate>

class EntryView;

/**
 * Paints an entry as a two-line Fluent list row for the Vault Index layout:
 * icon tile, title, "username · URL" and a badge for expired or weak entries.
 */
class EntryListItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit EntryListItemDelegate(EntryView* view);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    static constexpr int RowHeight = 54;

private:
    EntryView* m_view;
};

#endif // KEEPASSXC_ENTRYLISTITEMDELEGATE_H
