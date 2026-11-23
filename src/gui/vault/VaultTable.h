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

#ifndef OPENVIREO_VAULTTABLE_H
#define OPENVIREO_VAULTTABLE_H

#include <QStyledItemDelegate>

#include <components/scrolling/ScrollBar.h>

class QAbstractItemView;
class QAbstractScrollArea;
class QPainter;

namespace Vault
{
    /**
     * Fluent look for a table, tree or list of records inside a card or form: a rounded box, a quiet header,
     * rows drawn by VaultTableDelegate (unless the view already has one) and Fluent scroll bars.
     * While the view has no rows, emptyText is shown in their place and the header is hidden.
     * Models, sorting, editing, drag and drop and keyboard navigation are left to the view.
     */
    void styleTable(QAbstractItemView* view, const QString& emptyText = {});
    // Change the text shown while a view styled with styleTable() has no rows
    void setEmptyText(QAbstractItemView* view, const QString& text);
    // The empty-state text of a view styled with styleTable(), and whether it is showing
    QString emptyText(const QAbstractItemView* view);
    bool isEmptyStateVisible(const QAbstractItemView* view);

    // Fluent scroll bars: thin, fading in while scrolling or hovered, always shown in high contrast
    void useFluentScrollBars(QAbstractScrollArea* area);

    // Background of a navigation pane row, such as a group or a tag: a subtle fill on hover and selection, the
    // accent pill on a selected row, and in high contrast an outline so neither depends on the fill alone
    void paintNavigationRow(QPainter* painter, const QRectF& row, bool selected, bool hovered, bool pill = true);
} // namespace Vault

/**
 * Paints the cells of a table row as one Fluent list row: a subtle fill on hover, an accent tint and pill on the
 * selected row, and in high contrast an outline around both so state never depends on a fill alone. Cell content
 * (text, icons, check boxes) is drawn as usual, in the theme's text colour unless the model sets one.
 *
 * Delegates with their own editors or decorations can derive from it, or call paintRowBackground().
 */
class VaultTableDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit VaultTableDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    // Paint the row state behind one cell. Returns the option to draw its content with: no stock selection,
    // hover or focus, the theme's text colours and room on either side.
    static QStyleOptionViewItem
    paintRowBackground(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index);

    static constexpr int MinimumRowHeight = 32;
};

/**
 * Fluent-Qt's scroll bar, kept visible in high contrast where a bar that fades out would hide where the user is.
 */
class VaultScrollBar : public fluent::scrolling::ScrollBar
{
    Q_OBJECT

public:
    VaultScrollBar(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

#endif // OPENVIREO_VAULTTABLE_H
