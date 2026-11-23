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

#include "VaultTable.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultWidgets.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QEvent>
#include <QHeaderView>
#include <QListView>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScrollBar>
#include <QTableView>
#include <QTreeView>

namespace
{
    constexpr int RowInset = 4;
    // Room around cell content; the first cell leaves more for the selection pill
    constexpr int CellPadding = 6;
    constexpr int FirstCellPadding = 12;
    constexpr int RowRadius = 4;
    const char* const TrackerName = "vaultTableTracker";

    QHeaderView* horizontalHeaderOf(const QAbstractItemView* view)
    {
        if (auto* table = qobject_cast<const QTableView*>(view)) {
            return table->horizontalHeader();
        }
        if (auto* tree = qobject_cast<const QTreeView*>(view)) {
            return tree->header();
        }
        return nullptr;
    }

    // Whether the cell starts or ends its row, so the row's rounded ends and pill go on the outer cells only
    void columnEdges(const QAbstractItemView* view, const QModelIndex& index, bool& first, bool& last)
    {
        first = true;
        last = true;
        if (!view || view->selectionBehavior() != QAbstractItemView::SelectRows) {
            return;
        }
        auto* header = horizontalHeaderOf(view);
        if (!header || header->count() < 2) {
            return;
        }
        int firstVisual = -1;
        int lastVisual = -1;
        for (int visual = 0; visual < header->count(); ++visual) {
            if (!header->isSectionHidden(header->logicalIndex(visual))) {
                if (firstVisual < 0) {
                    firstVisual = visual;
                }
                lastVisual = visual;
            }
        }
        const int visual = header->visualIndex(index.column());
        first = visual == firstVisual;
        last = visual == lastVisual;
    }

    /**
     * Lives on a styled view: tracks the row under the mouse, since table views only report hover per cell,
     * and shows the empty state while the model has no rows.
     */
    class TableTracker : public QObject
    {
    public:
        explicit TableTracker(QAbstractItemView* view)
            : QObject(view)
            , m_view(view)
            , m_label(new QLabel(view->viewport()))
        {
            setObjectName(TrackerName);
            m_label->setObjectName(QStringLiteral("vaultEmptyState"));
            m_label->setAlignment(Qt::AlignCenter);
            m_label->setWordWrap(true);
            m_label->setTextFormat(Qt::PlainText);
            m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_label->hide();
            view->viewport()->installEventFilter(this);
            view->installEventFilter(this);
        }

        bool isHovered(const QModelIndex& index) const
        {
            return m_hovered.isValid() && index.row() == m_hovered.row() && index.parent() == m_hovered.parent();
        }

        QString emptyText() const
        {
            return m_emptyText;
        }

        bool isEmptyStateVisible() const
        {
            return m_label && !m_label->isHidden();
        }

        void setEmptyText(const QString& text)
        {
            m_emptyText = text;
            if (!m_label) {
                return;
            }
            m_label->setText(text);
            m_label->setAccessibleName(text);
            refresh();
        }

        void applyTheme()
        {
            if (!m_label) {
                return;
            }
            const auto p = FluentTheme::palette();
            m_label->setFont(Vault::font(Typography::FontRole::Body));
            m_label->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }")
                                       .arg((p.highContrast ? p.text : p.textSecondary).name(QColor::HexArgb)));
        }

        void refresh()
        {
            // The label goes with the viewport, before the view finishes tearing down and its model reports
            if (!m_label) {
                return;
            }
            hookModel();
            auto* model = m_view->model();
            const bool empty = !m_emptyText.isEmpty() && (!model || model->rowCount(m_view->rootIndex()) == 0);
            m_label->setVisible(empty);
            if (empty) {
                placeLabel();
            }
            if (auto* header = horizontalHeaderOf(m_view)) {
                if (empty && !header->isHidden()) {
                    header->hide();
                    m_headerHidden = true;
                } else if (!empty && m_headerHidden) {
                    header->show();
                    m_headerHidden = false;
                }
            }
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (watched == m_view->viewport()) {
                switch (event->type()) {
                case QEvent::MouseMove:
                case QEvent::HoverMove:
                case QEvent::HoverEnter:
                    if (auto* hover = dynamic_cast<QSinglePointEvent*>(event)) {
                        setHovered(m_view->indexAt(hover->position().toPoint()));
                    }
                    break;
                case QEvent::Leave:
                case QEvent::HoverLeave:
                    setHovered({});
                    break;
                case QEvent::Resize:
                    placeLabel();
                    break;
                case QEvent::Paint:
                    // Views take their model after they were styled, and may swap it later
                    if (m_view->model() != m_model) {
                        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
                    }
                    break;
                default:
                    break;
                }
            } else if (watched == m_view && event->type() == QEvent::Show) {
                refresh();
            } else if (watched == m_view && event->type() == QEvent::PaletteChange) {
                clearHighlight();
            }
            return QObject::eventFilter(watched, event);
        }

    public:
        // The view paints each selected cell with the palette's highlight before its delegate draws the row, and
        // cards over the window reset every palette inside them. Keep the highlight clear, on the view only: its
        // editors keep theirs for selected text.
        void clearHighlight()
        {
            auto palette = m_view->palette();
            if (palette.color(QPalette::Active, QPalette::Highlight).alpha() == 0
                && palette.color(QPalette::Inactive, QPalette::Highlight).alpha() == 0) {
                return;
            }
            for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                palette.setColor(group, QPalette::Highlight, Qt::transparent);
            }
            m_view->setPalette(palette);
        }

    private:
        void setHovered(const QModelIndex& index)
        {
            if (index.isValid() ? isHovered(index) : !m_hovered.isValid()) {
                return;
            }
            m_hovered = index;
            m_view->viewport()->update();
        }

        void placeLabel()
        {
            if (!m_label) {
                return;
            }
            m_label->setGeometry(m_view->viewport()->rect().adjusted(16, 16, -16, -16));
        }

        void hookModel()
        {
            auto* model = m_view->model();
            if (model == m_model) {
                return;
            }
            if (m_model) {
                disconnect(m_model, nullptr, this, nullptr);
            }
            m_model = model;
            m_hovered = QPersistentModelIndex();
            if (!model) {
                return;
            }
            const auto update = [this] { refresh(); };
            connect(model, &QAbstractItemModel::rowsInserted, this, update);
            connect(model, &QAbstractItemModel::rowsRemoved, this, update);
            connect(model, &QAbstractItemModel::modelReset, this, update);
            connect(model, &QAbstractItemModel::layoutChanged, this, update);
        }

        QAbstractItemView* m_view;
        QPointer<QLabel> m_label;
        QPointer<QAbstractItemModel> m_model;
        QPersistentModelIndex m_hovered;
        QString m_emptyText;
        bool m_headerHidden = false;
    };

    TableTracker* trackerOf(const QAbstractItemView* view)
    {
        // Found by name: the tracker has no meta-object of its own
        return view ? static_cast<TableTracker*>(view->findChild<QObject*>(TrackerName, Qt::FindDirectChildrenOnly))
                    : nullptr;
    }

    QString tableStyleSheet()
    {
        const auto p = FluentTheme::palette();
        const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
        // Matched by a property so editors and combo box pop-ups inside the view keep their own look
        return QStringLiteral("*[vaultTable=\"true\"] { background-color: %1; border: 1px solid %2;"
                              " border-radius: 8px; outline: none; color: %3;"
                              " selection-background-color: transparent; selection-color: %3;"
                              // The rows paint their own selection; without this the style fills the cell first
                              " show-decoration-selected: 0; }"
                              "*[vaultTable=\"true\"]::item, *[vaultTable=\"true\"]::item:selected,"
                              " *[vaultTable=\"true\"]::item:hover { background: transparent; border: none; }"
                              "*[vaultTable=\"true\"]::branch { background: transparent; }"
                              "*[vaultTable=\"true\"] QHeaderView { background: transparent; border: none; }"
                              "*[vaultTable=\"true\"] QHeaderView::section { background: transparent; border: none;"
                              " border-bottom: 1px solid %4; padding: 6px 8px 6px 14px; color: %5; }"
                              "*[vaultTable=\"true\"] QHeaderView::section:hover { background-color: %6; }"
                              "*[vaultTable=\"true\"] QTableCornerButton::section { background: transparent;"
                              " border: none; }")
            .arg(argb(p.layer),
                 argb(p.stroke),
                 argb(p.text),
                 argb(p.divider),
                 argb(p.highContrast ? p.text : p.textSecondary),
                 argb(p.subtleHover));
    }
} // namespace

void Vault::styleTable(QAbstractItemView* view, const QString& emptyText)
{
    if (!view) {
        return;
    }
    auto* tracker = trackerOf(view);
    if (!tracker) {
        tracker = new TableTracker(view);
        view->setProperty("vaultTable", true);
        view->setAlternatingRowColors(false);
        view->setMouseTracking(true);
        view->viewport()->setAttribute(Qt::WA_Hover);
        view->setFrameShape(QFrame::StyledPanel);
        // Icon grids keep the stock delegate, which lays out a tile per item; rows get the Fluent delegate
        const auto* list = qobject_cast<QListView*>(view);
        const bool iconGrid = list && list->viewMode() == QListView::IconMode;
        if (!iconGrid && !qobject_cast<VaultTableDelegate*>(view->itemDelegate())) {
            view->setItemDelegate(new VaultTableDelegate(view));
        }
        if (auto* table = qobject_cast<QTableView*>(view)) {
            table->setShowGrid(false);
            table->setCornerButtonEnabled(false);
            table->verticalHeader()->hide();
            table->verticalHeader()->setMinimumSectionSize(VaultTableDelegate::MinimumRowHeight);
            table->verticalHeader()->setDefaultSectionSize(VaultTableDelegate::MinimumRowHeight);
        }
        if (auto* header = horizontalHeaderOf(view)) {
            header->setHighlightSections(false);
            // Rows run the full width of the box instead of stopping after the last column
            header->setStretchLastSection(true);
            header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        useFluentScrollBars(view);
        Vault::followTheme(view, [view, tracker] {
            view->setStyleSheet(tableStyleSheet());
            view->setFont(Vault::font(Typography::FontRole::Body));
            if (auto* header = horizontalHeaderOf(view)) {
                auto font = Vault::font(Typography::FontRole::Caption);
                font.setWeight(QFont::DemiBold);
                header->setFont(font);
            }
            tracker->applyTheme();
            tracker->clearHighlight();
            view->viewport()->update();
        });
    }
    tracker->setEmptyText(emptyText);
}

void Vault::setEmptyText(QAbstractItemView* view, const QString& text)
{
    if (auto* tracker = trackerOf(view)) {
        tracker->setEmptyText(text);
    }
}

QString Vault::emptyText(const QAbstractItemView* view)
{
    auto* tracker = trackerOf(view);
    return tracker ? tracker->emptyText() : QString();
}

bool Vault::isEmptyStateVisible(const QAbstractItemView* view)
{
    auto* tracker = trackerOf(view);
    return tracker && tracker->isEmptyStateVisible();
}

void Vault::useFluentScrollBars(QAbstractScrollArea* area)
{
    if (!area || qobject_cast<VaultScrollBar*>(area->verticalScrollBar())) {
        return;
    }
    area->setVerticalScrollBar(new VaultScrollBar(Qt::Vertical, area));
    area->setHorizontalScrollBar(new VaultScrollBar(Qt::Horizontal, area));
}

void Vault::paintNavigationRow(QPainter* painter, const QRectF& row, bool selected, bool hovered, bool pill)
{
    const auto p = FluentTheme::palette();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (selected || hovered) {
        QPainterPath path;
        path.addRoundedRect(row, RowRadius, RowRadius);
        painter->fillPath(path, selected && hovered ? p.subtlePressed : p.subtleHover);
        QPainterPath outline;
        outline.addRoundedRect(row.adjusted(1, 1, -1, -1), RowRadius, RowRadius);
        FluentTheme::paintRowStateOutline(painter, outline, selected, hovered);
    }
    if (selected && pill) {
        QPainterPath indicator;
        indicator.addRoundedRect(QRectF(row.left(), row.center().y() - 8, 3, 16), 1.5, 1.5);
        painter->fillPath(indicator, p.accent);
    }
    painter->restore();
}

VaultTableDelegate::VaultTableDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

QStyleOptionViewItem
VaultTableDelegate::paintRowBackground(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index)
{
    const auto p = FluentTheme::palette();
    const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
    const auto* tracker = trackerOf(view);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = tracker ? tracker->isHovered(index) : option.state.testFlag(QStyle::State_MouseOver);
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);
    const bool focused = view && view->hasFocus() && view->currentIndex().isValid()
                         && view->currentIndex().row() == index.row()
                         && view->currentIndex().parent() == index.parent();
    bool first = true;
    bool last = true;
    columnEdges(view, index, first, last);

    // The row's shape, running on past the edges of inner cells so neighbouring cells join up
    const QRectF cell(option.rect);
    const QRectF row = cell.adjusted(first ? RowInset : -RowRadius * 4, 1, last ? -RowInset : RowRadius * 4, -1);
    QPainterPath shape;
    shape.addRoundedRect(row, RowRadius, RowRadius);

    painter->save();
    painter->setClipRect(option.rect);
    painter->setRenderHint(QPainter::Antialiasing);
    if (enabled && (selected || hovered)) {
        painter->fillPath(shape, selected ? p.accentSubtle : p.subtleHover);
        QPainterPath outline;
        outline.addRoundedRect(row.adjusted(1, 1, -1, -1), RowRadius, RowRadius);
        FluentTheme::paintRowStateOutline(painter, outline, selected, hovered);
    }
    if (focused && !selected) {
        painter->setPen(QPen(p.focus, 2));
        painter->setBrush(Qt::NoBrush);
        QPainterPath ring;
        ring.addRoundedRect(row.adjusted(1, 1, -1, -1), RowRadius, RowRadius);
        painter->drawPath(ring);
    }
    if (selected && first) {
        // WinUI's selection indicator: a short accent pill at the start of the row
        const qreal height = qMin<qreal>(16, row.height() - 8);
        QPainterPath pill;
        pill.addRoundedRect(QRectF(row.left(), row.center().y() - height / 2, 3, height), 1.5, 1.5);
        painter->fillPath(pill, p.accent);
    }
    painter->restore();

    QStyleOptionViewItem content(option);
    content.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    const auto text = enabled ? p.text : p.textDisabled;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        content.palette.setColor(group, QPalette::Text, group == QPalette::Disabled ? p.textDisabled : text);
        content.palette.setColor(group, QPalette::WindowText, group == QPalette::Disabled ? p.textDisabled : text);
        content.palette.setColor(group, QPalette::HighlightedText, text);
    }
    content.rect.adjust(first ? FirstCellPadding : CellPadding, 0, last ? -CellPadding : 0, 0);
    return content;
}

void VaultTableDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const auto content = paintRowBackground(painter, option, index);
    QStyledItemDelegate::paint(painter, content, index);
}

QSize VaultTableDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    auto size = QStyledItemDelegate::sizeHint(option, index);
    size.setWidth(size.width() + FirstCellPadding + CellPadding);
    size.setHeight(qMax(size.height(), MinimumRowHeight));
    return size;
}

VaultScrollBar::VaultScrollBar(Qt::Orientation orientation, QWidget* parent)
    : fluent::scrolling::ScrollBar(orientation, parent)
{
    setThickness(8);
}

void VaultScrollBar::paintEvent(QPaintEvent* event)
{
    // A bar that fades out hides where the user is; high contrast keeps it in view
    if (FluentTheme::isHighContrast() && opacity() < 1.0) {
        setOpacity(1.0);
    }
    fluent::scrolling::ScrollBar::paintEvent(event);
}
