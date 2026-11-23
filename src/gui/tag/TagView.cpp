/*
 * Copyright (C) 2022 KeePassXC Team <team@keepassxc.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or (at your option)
 * version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "TagView.h"

#include "TagModel.h"
#include "core/Database.h"
#include "gui/Icons.h"
#include "gui/MessageBox.h"
#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultTable.h"
#include "gui/vault/VaultWidgets.h"

#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>

namespace
{
    constexpr int TagRowHeight = 32;

    // A tag as a navigation item on the navigation pane, matching the groups above it
    class TagNavItemDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            auto size = QStyledItemDelegate::sizeHint(option, index);
            size.setHeight(TagRowHeight);
            return size;
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto p = FluentTheme::palette();
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            const QRectF row = QRectF(option.rect).adjusted(4, 2, -4, -2);
            Vault::paintNavigationRow(painter, row, selected, hovered);

            painter->save();
            const bool focused = option.state.testFlag(QStyle::State_HasFocus) && option.widget
                                 && option.widget->hasFocus() && !selected;
            if (focused) {
                // Moving through the tags with Ctrl held changes focus without selecting
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(QPen(p.focus, 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(row.adjusted(1, 1, -1, -1), 4, 4);
            }
            QRect rect = option.rect.adjusted(16, 0, -12, 0);
            const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull()) {
                icon.paint(painter, QRect(rect.left(), rect.center().y() - 8, 16, 16));
            }
            rect.setLeft(rect.left() + 28);
            const auto font = Vault::font(Typography::FontRole::Body);
            painter->setFont(font);
            painter->setPen(option.state.testFlag(QStyle::State_Enabled) ? p.text : p.textDisabled);
            painter->drawText(rect,
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(
                                  index.data(Qt::DisplayRole).toString(), Qt::ElideRight, rect.width()));
            painter->restore();
        }
    };
} // namespace

TagView::TagView(QWidget* parent)
    : QListView(parent)
    , m_model(new TagModel(this))
{
    setModel(m_model);
    setFrameStyle(QFrame::NoFrame);
    setSelectionMode(QListView::ExtendedSelection);
    setSelectionBehavior(QListView::SelectRows);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setItemDelegate(new TagNavItemDelegate(this));
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
    Vault::useFluentScrollBars(this);

    connect(this, &QListView::customContextMenuRequested, this, &TagView::contextMenuRequested);
}

void TagView::setDatabase(QSharedPointer<Database> db)
{
    m_db = db;
    m_model->setDatabase(db);
}

void TagView::contextMenuRequested(const QPoint& pos)
{
    auto index = indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    // Allow removing tags from all entries in a database
    VaultMenu menu;
    auto action = menu.exec({new QAction(icons()->icon("trash"), tr("Remove Tag"), nullptr)}, mapToGlobal(pos));
    if (action) {
        auto tag = index.data(Qt::DisplayRole).toString();
        auto ans = MessageBox::question(this,
                                        tr("Confirm Remove Tag"),
                                        tr("Remove tag \"%1\" from all entries in this database?").arg(tag),
                                        MessageBox::Remove | MessageBox::Cancel);
        if (ans == MessageBox::Remove) {
            m_db->removeTag(tag);
        }
    }
}
