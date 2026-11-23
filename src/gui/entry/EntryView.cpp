/*
 * Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
 * Copyright (C) 2010 Felix Geyer <debfx@fobos.de>
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

#include "EntryView.h"

#include <QAccessible>
#include <QDrag>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QScreen>
#include <QListWidget>
#include <QShortcut>

#include "gui/entry/EntryListItemDelegate.h"
#include "gui/vault/VaultTable.h"
#include "gui/SortFilterHideProxyModel.h"

EntryView::EntryView(QWidget* parent)
    : QTreeView(parent)
    , m_model(new EntryModel(this))
    , m_sortModel(new SortFilterHideProxyModel(this))
{
    m_sortModel->setSourceModel(m_model);
    m_sortModel->setDynamicSortFilter(true);
    m_sortModel->setSortLocaleAware(true);
    m_sortModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    // Use Qt::UserRole as sort role, see EntryModel::data()
    m_sortModel->setSortRole(Qt::UserRole);
    QTreeView::setModel(m_sortModel);

    // One two-line row per entry, drawn by EntryListItemDelegate from the title column and its siblings.
    // The other columns stay in the model for the delegate, sorting and drag and drop, but are never shown.
    setUniformRowHeights(true);
    setRootIsDecorated(false);
    setDragEnabled(true);
    setSortingEnabled(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    header()->setHidden(true);
    header()->setStretchLastSection(true);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
    setItemDelegateForColumn(EntryModel::Title, new EntryListItemDelegate(this));
    for (int column = 0; column < header()->count(); ++column) {
        setColumnHidden(column, column != EntryModel::Title);
    }
    // Keep the list in title order; there is no header to change it
    sortByColumn(EntryModel::Title, Qt::AscendingOrder);
    Vault::useFluentScrollBars(this);

    // QAbstractItemView::startDrag() uses this property as the default drag action
    setDefaultDropAction(Qt::MoveAction);

    connect(this, SIGNAL(doubleClicked(QModelIndex)), SLOT(emitEntryActivated(QModelIndex)));
    connect(selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        emit entrySelectionChanged(currentEntry());
    });

    new QShortcut(Qt::CTRL | Qt::Key_F10, this, SLOT(contextMenuShortcutPressed()), nullptr, Qt::WidgetShortcut);
    new QShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_J, this, SLOT(jumpToGroupShortcut()), nullptr, Qt::WidgetShortcut);
}

void EntryView::contextMenuShortcutPressed()
{
    auto index = currentIndex();
    if (hasFocus() && index.isValid()) {
        emit customContextMenuRequested(visualRect(index).bottomLeft());
    }
}

void EntryView::keyPressEvent(QKeyEvent* event)
{
    if ((event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return) && currentIndex().isValid()) {
        emitEntryActivated(currentIndex());
#ifdef Q_OS_MACOS
        // Pressing return does not emit the QTreeView::activated signal on mac os
        emit activated(currentIndex());
#endif
    }

    int last = m_model->rowCount() - 1;
    if (last > 0) {
        QAccessibleEvent accessibleEvent(this, QAccessible::PageChanged);
        if (event->key() == Qt::Key_Up && currentIndex().row() == 0) {
            QModelIndex index = m_sortModel->mapToSource(m_sortModel->index(last, 0));
            setCurrentEntry(m_model->entryFromIndex(index));
            QAccessible::updateAccessibility(&accessibleEvent);
            return;
        }

        if (event->key() == Qt::Key_Down && currentIndex().row() == last) {
            QModelIndex index = m_sortModel->mapToSource(m_sortModel->index(0, 0));
            setCurrentEntry(m_model->entryFromIndex(index));
            QAccessible::updateAccessibility(&accessibleEvent);
            return;
        }
    }

    QTreeView::keyPressEvent(event);
}

void EntryView::focusInEvent(QFocusEvent* event)
{
    emit entrySelectionChanged(currentEntry());
    QTreeView::focusInEvent(event);
}

void EntryView::displayGroup(Group* group)
{
    m_model->setGroup(group);
    // A search sorts by group; a group's entries are listed by title
    sortByColumn(EntryModel::Title, Qt::AscendingOrder);
    setFirstEntryActive();
    m_inSearchMode = false;
}

void EntryView::displaySearch(const QList<Entry*>& entries)
{
    m_model->setEntries(entries);
    setFirstEntryActive();

    // Reset sort column to 'Group', overrides DatabaseWidgetStateSync
    m_sortModel->sort(EntryModel::ParentGroup, Qt::AscendingOrder);
    sortByColumn(EntryModel::ParentGroup, Qt::AscendingOrder);

    m_inSearchMode = true;
}

void EntryView::setFirstEntryActive()
{
    if (m_model->rowCount() > 0) {
        QModelIndex index = m_sortModel->mapToSource(m_sortModel->index(0, 0));
        setCurrentEntry(m_model->entryFromIndex(index));
    } else {
        emit entrySelectionChanged(currentEntry());
    }
}

bool EntryView::inSearchMode()
{
    return m_inSearchMode;
}

bool EntryView::isSorted()
{
    return header()->sortIndicatorSection() != -1;
}

void EntryView::emitEntryActivated(const QModelIndex& index)
{
    Entry* entry = entryFromIndex(index);
    emit entryActivated(entry, static_cast<EntryModel::ModelColumn>(m_sortModel->mapToSource(index).column()));
}

void EntryView::setModel(QAbstractItemModel* model)
{
    Q_UNUSED(model);
    Q_ASSERT(false);
}

Entry* EntryView::currentEntry()
{
    QModelIndexList list = selectionModel()->selectedRows();
    if (list.size() == 1) {
        return m_model->entryFromIndex(m_sortModel->mapToSource(list.first()));
    } else {
        return nullptr;
    }
}

QList<Entry*> EntryView::selectedEntries()
{
    QList<Entry*> list;
    for (auto row : selectionModel()->selectedRows()) {
        list.append(m_model->entryFromIndex(m_sortModel->mapToSource(row)));
    }
    return list;
}

int EntryView::numberOfSelectedEntries()
{
    return selectionModel()->selectedRows().size();
}

void EntryView::setCurrentEntry(Entry* entry)
{
    auto index = m_model->indexFromEntry(entry);
    if (index.isValid()) {
        selectionModel()->setCurrentIndex(m_sortModel->mapFromSource(index),
                                          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

Entry* EntryView::entryFromIndex(const QModelIndex& index)
{
    if (index.isValid()) {
        return m_model->entryFromIndex(m_sortModel->mapToSource(index));
    } else {
        return nullptr;
    }
}

QModelIndex EntryView::indexFromEntry(Entry* entry)
{
    return m_sortModel->mapFromSource(m_model->indexFromEntry(entry));
}

int EntryView::currentEntryIndex()
{
    QModelIndexList list = selectionModel()->selectedRows();
    if (list.size() == 1) {
        auto index = m_sortModel->mapToSource(list.first());
        return index.row();
    } else {
        return -1;
    }
}

void EntryView::startDrag(Qt::DropActions supportedActions)
{
    auto selectedIndexes = selectionModel()->selectedRows(EntryModel::Title);
    if (selectedIndexes.isEmpty()) {
        return;
    }

    // Create a mime data object for the selected rows
    auto mimeData = m_sortModel->mimeData(selectedIndexes);
    if (!mimeData) {
        return;
    }

    // Create a temporary list widget to display the dragged items
    int i = 0;
    QListWidget listWidget;
    for (auto& index : selectedIndexes) {
        if (++i > 4) {
            int remaining = selectedIndexes.size() - i + 1;
            listWidget.addItem(tr("+ %1 entry(s)...", "", remaining).arg(remaining));
            break;
        }

        QIcon icon;
        icon.addPixmap(m_sortModel->data(index, Qt::DecorationRole).value<QPixmap>());

        auto item = new QListWidgetItem;
        item->setText(m_sortModel->data(index, Qt::DisplayRole).toString());
        item->setIcon(icon);
        listWidget.addItem(item);
    }

    listWidget.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listWidget.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listWidget.setStyleSheet("QListWidget { background-color: palette(highlight); border: 1px solid palette(dark); "
                             "padding: 4px; color: palette(highlighted-text); }");
    auto width = listWidget.sizeHintForColumn(0) + 2 * listWidget.frameWidth();
    auto height = listWidget.sizeHintForRow(0) * listWidget.count() + 2 * listWidget.frameWidth();
    listWidget.setFixedWidth(width);
    listWidget.setFixedHeight(height);

    // Grab the screen pixel ratio where the window resides
    // TODO: Use direct call to screen() when moving to Qt 6
    auto screen = QGuiApplication::screenAt(window()->geometry().center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    auto pixelRatio = screen->devicePixelRatio();

    // Render the list widget to a pixmap
    QPixmap pixmap(QSize(width, height) * pixelRatio);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(pixelRatio);
    listWidget.render(&pixmap);

    // Create a drag object and start the drag
    auto drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->setPixmap(pixmap);
    drag->exec(supportedActions, defaultDropAction());
}

void EntryView::jumpToGroupShortcut()
{
    // Only allow jump to group in search mode
    if (!inSearchMode()) {
        return;
    }

    auto entry = currentEntry();
    if (entry) {
        // Emit the entryActivated signal with ParentGroup column to trigger jump to group
        emit entryActivated(entry, EntryModel::ParentGroup);
    }
}
