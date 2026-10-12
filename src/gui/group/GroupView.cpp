/*
 *  Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2010 Felix Geyer <debfx@fobos.de>
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

#include "GroupView.h"

#include <QDragMoveEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QShortcut>
#include <QStyledItemDelegate>

#include "core/Config.h"
#include "core/Group.h"
#include "gui/group/GroupModel.h"
#include "gui/styles/FluentTheme.h"

#include <FluentQt/Foundation.h>

namespace
{
    constexpr int NavRowHeight = 36;

    QFont fluentFont(Typography::FontRole role)
    {
        return fluent::ThemeRegistry::instance().resolvedFontStyle(role).toQFont();
    }

    // Paints a group as a navigation item: icon, name and its entry count.
    // The row background and selection pill are drawn by GroupView::drawRow.
    class GroupNavItemDelegate : public QStyledItemDelegate
    {
    public:
        GroupNavItemDelegate(GroupModel* model, QObject* parent)
            : QStyledItemDelegate(parent)
            , m_model(model)
        {
        }

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            auto size = QStyledItemDelegate::sizeHint(option, index);
            size.setHeight(NavRowHeight);
            return size;
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto p = FluentTheme::palette();
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            QRect rect = option.rect.adjusted(4, 0, -12, 0);
            const auto icon = index.data(Qt::DecorationRole);
            QPixmap pixmap;
            if (icon.canConvert<QPixmap>()) {
                pixmap = icon.value<QPixmap>();
            } else if (icon.canConvert<QIcon>()) {
                pixmap = icon.value<QIcon>().pixmap(16, 16);
            }
            if (!pixmap.isNull()) {
                painter->drawPixmap(QRect(rect.left(), rect.center().y() - 8, 16, 16), pixmap);
            }
            rect.setLeft(rect.left() + 28);

            auto* group = m_model->groupFromIndex(index);
            if (group) {
                const auto count = QString::number(group->entries().size());
                const auto countFont = fluentFont(Typography::FontRole::Caption);
                painter->setFont(countFont);
                painter->setPen(p.textTertiary);
                const int countWidth = QFontMetrics(countFont).horizontalAdvance(count);
                painter->drawText(rect, Qt::AlignRight | Qt::AlignVCenter, count);
                rect.setRight(rect.right() - countWidth - 8);
            }

            const auto nameFont = fluentFont(Typography::FontRole::Body);
            painter->setFont(nameFont);
            painter->setPen(index.data(Qt::ForegroundRole).canConvert<QColor>()
                                ? index.data(Qt::ForegroundRole).value<QColor>()
                                : p.text);
            const auto name =
                QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, rect.width());
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, name);

            painter->restore();
        }

    private:
        GroupModel* m_model;
    };
} // namespace

GroupView::GroupView(Database* db, QWidget* parent)
    : QTreeView(parent)
    , m_model(new GroupModel(db, this))
    , m_updatingExpanded(false)
{
    QTreeView::setModel(m_model);
    setHeaderHidden(true);
    setUniformRowHeights(true);
    setTextElideMode(Qt::ElideNone);

    // clang-format off
    connect(this, SIGNAL(expanded(QModelIndex)), SLOT(expandedChanged(QModelIndex)));
    connect(this, SIGNAL(collapsed(QModelIndex)), SLOT(expandedChanged(QModelIndex)));
    connect(this, SIGNAL(clicked(QModelIndex)), SIGNAL(groupSelectionChanged()));
    connect(m_model, SIGNAL(rowsInserted(QModelIndex,int,int)), SLOT(syncExpandedState(QModelIndex,int,int)));
    connect(m_model, SIGNAL(modelReset()), SLOT(modelReset()));
    connect(selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)), SIGNAL(groupSelectionChanged()));
    // clang-format on

    new QShortcut(Qt::CTRL | Qt::Key_F10, this, SLOT(contextMenuShortcutPressed()), nullptr, Qt::WidgetShortcut);
    new QShortcut(
        Qt::CTRL | Qt::SHIFT | Qt::Key_PageUp, this, SLOT(selectPreviousGroup()), nullptr, Qt::WindowShortcut);
    new QShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_PageDown, this, SLOT(selectNextGroup()), nullptr, Qt::WindowShortcut);

    // keyboard shortcuts to sort children of a group
    auto shortcut = new QShortcut(Qt::CTRL | Qt::Key_Down, this, nullptr, nullptr, Qt::WidgetShortcut);
    connect(shortcut, &QShortcut::activated, this, [this]() { sortGroups(false); });

    shortcut = new QShortcut(Qt::CTRL | Qt::Key_Up, this, nullptr, nullptr, Qt::WidgetShortcut);
    connect(shortcut, &QShortcut::activated, this, [this]() { sortGroups(true); });

    modelReset();

    setDragEnabled(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDefaultDropAction(Qt::MoveAction);
}

void GroupView::selectPreviousGroup()
{
    auto previousIndex = indexAbove(currentIndex());
    if (previousIndex.isValid()) {
        auto previousGroup = m_model->groupFromIndex(previousIndex);
        setCurrentGroup(previousGroup);
    }
}

void GroupView::selectNextGroup()
{
    auto nextIndex = indexBelow(currentIndex());
    if (nextIndex.isValid()) {
        auto nextGroup = m_model->groupFromIndex(nextIndex);
        setCurrentGroup(nextGroup);
    }
}

void GroupView::contextMenuShortcutPressed()
{
    auto index = currentIndex();
    if (hasFocus() && index.isValid()) {
        emit customContextMenuRequested(visualRect(index).bottomLeft());
    }
}

void GroupView::changeDatabase(const QSharedPointer<Database>& newDb)
{
    m_model->changeDatabase(newDb.data());
    setColumnWidth(0, sizeHintForColumn(0));
}

void GroupView::setNavigationStyle(bool enabled)
{
    if (m_navigationStyle == enabled) {
        return;
    }
    m_navigationStyle = enabled;

    setItemDelegate(enabled ? new GroupNavItemDelegate(m_model, this) : new QStyledItemDelegate(this));
    setFrameShape(enabled ? QFrame::NoFrame : QFrame::StyledPanel);
    setIndentation(enabled ? 16 : style()->pixelMetric(QStyle::PM_TreeViewIndentation, nullptr, this));
    setMouseTracking(enabled);
    viewport()->setAttribute(Qt::WA_Hover, enabled);
    // Let the navigation pane's surface show through
    viewport()->setAutoFillBackground(!enabled);
}

void GroupView::drawRow(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    if (!m_navigationStyle) {
        QTreeView::drawRow(painter, option, index);
        return;
    }

    const auto p = FluentTheme::palette();
    const bool selected = selectionModel()->isSelected(index);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QRectF row = QRectF(option.rect).adjusted(4, 2, -4, -2);
    if (selected || hovered) {
        QPainterPath path;
        path.addRoundedRect(row, 4, 4);
        painter->fillPath(path, selected && hovered ? p.subtlePressed : p.subtleHover);
    }
    // The root's chevron sits where the pill would, so the root shows only the highlight
    if (selected && index.parent().isValid()) {
        QPainterPath pill;
        pill.addRoundedRect(QRectF(row.left(), row.center().y() - 8, 3, 16), 1.5, 1.5);
        painter->fillPath(pill, p.accent);
    }
    painter->restore();

    // The style must not paint its own selection or hover highlight over ours
    QStyleOptionViewItem plain(option);
    plain.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    QTreeView::drawRow(painter, plain, index);
}

void GroupView::drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const
{
    if (!m_navigationStyle) {
        QTreeView::drawBranches(painter, rect, index);
        return;
    }

    if (!model()->hasChildren(index)) {
        return;
    }

    // Fluent chevron in place of the platform branch indicator
    QFont iconFont(Typography::FontFamily::FluentIcons);
    iconFont.setPixelSize(Typography::IconSize::Compact);
    painter->save();
    painter->setFont(iconFont);
    painter->setPen(FluentTheme::palette().textSecondary);
    QRect chevron(rect.right() - indentation() + 1, rect.top(), indentation(), rect.height());
    painter->drawText(chevron,
                      Qt::AlignCenter,
                      isExpanded(index) ? Typography::Icons::ChevronDown : Typography::Icons::ChevronRight);
    painter->restore();
}

void GroupView::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->keyboardModifiers() & Qt::ControlModifier) {
        event->setDropAction(Qt::CopyAction);
    } else {
        event->setDropAction(Qt::MoveAction);
    }

    QTreeView::dragMoveEvent(event);

    // entries may only be dropped on groups
    if (event->isAccepted() && event->mimeData()->hasFormat("application/x-keepassx-entry")
        && (dropIndicatorPosition() == AboveItem || dropIndicatorPosition() == BelowItem)) {
        event->ignore();
    }
}

void GroupView::focusInEvent(QFocusEvent* event)
{
    emit groupFocused();
    QTreeView::focusInEvent(event);
}

Group* GroupView::currentGroup()
{
    if (currentIndex() == QModelIndex()) {
        return nullptr;
    } else {
        return m_model->groupFromIndex(currentIndex());
    }
}

void GroupView::expandedChanged(const QModelIndex& index)
{
    if (m_updatingExpanded) {
        return;
    }

    Group* group = m_model->groupFromIndex(index);
    group->setExpanded(isExpanded(index));
    setColumnWidth(0, sizeHintForColumn(0));
}

void GroupView::recInitExpanded(Group* group)
{
    m_updatingExpanded = true;
    expandGroup(group, group->isExpanded());
    m_updatingExpanded = false;

    const QList<Group*> children = group->children();
    for (Group* child : children) {
        recInitExpanded(child);
    }
}

void GroupView::expandGroup(Group* group, bool expand)
{
    QModelIndex index = m_model->index(group);
    setExpanded(index, expand);
}

void GroupView::sortGroups(bool reverse)
{
    Group* group = currentGroup();
    if (group) {
        m_model->sortChildren(group, reverse);
    }
}

void GroupView::setModel(QAbstractItemModel* model)
{
    Q_UNUSED(model);
    Q_ASSERT(false);
}

void GroupView::syncExpandedState(const QModelIndex& parent, int start, int end)
{
    for (int row = start; row <= end; row++) {
        Group* group = m_model->groupFromIndex(m_model->index(row, 0, parent));
        recInitExpanded(group);
    }
}

void GroupView::setCurrentGroup(Group* group)
{
    if (group == nullptr) {
        setCurrentIndex(QModelIndex());
    } else {
        setCurrentIndex(m_model->index(group));
    }
}

void GroupView::modelReset()
{
    recInitExpanded(m_model->groupFromIndex(m_model->index(0, 0)));
    setCurrentIndex(m_model->index(0, 0));
}
