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

#include "VaultResponsive.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/EntryDetailCard.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultPanes.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>

namespace
{
    std::optional<Vault::Layout> g_testLayout;

    // The flyout's panel: as wide as the groups pane usually is, and clear of the host's edges
    constexpr int FlyoutWidth = 280;
    constexpr int FlyoutMargin = 12;
} // namespace

Vault::Layout Vault::layoutForWidth(int width)
{
    if (g_testLayout) {
        return *g_testLayout;
    }
    if (width >= WideWidth) {
        return Layout::Wide;
    }
    return width >= NarrowWidth ? Layout::Medium : Layout::Narrow;
}

void Vault::setLayoutForTests(std::optional<Layout> layout)
{
    g_testLayout = layout;
}

VaultGroupsFlyout::VaultGroupsFlyout(QWidget* host)
    : QWidget(host)
    , m_panel(new QWidget(this))
{
    setObjectName("vaultGroupsFlyout");
    m_panel->setObjectName("vaultGroupsFlyoutPanel");
    auto* layout = new QVBoxLayout(m_panel);
    layout->setContentsMargins(2, 6, 2, 4);
    layout->setSpacing(0);
    host->installEventFilter(this);
    hide();
}

void VaultGroupsFlyout::setPane(QWidget* pane)
{
    m_pane = pane;
    if (pane) {
        m_panel->layout()->addWidget(pane);
        pane->show();
    }
}

QWidget* VaultGroupsFlyout::pane() const
{
    return m_pane;
}

void VaultGroupsFlyout::setAnchor(const QPoint& topLeft)
{
    m_anchor = topLeft;
    m_panel->setGeometry(panelRect());
    update();
}

QRect VaultGroupsFlyout::panelRect() const
{
    const int x = qMax(FlyoutMargin, m_anchor.x());
    const int y = qMax(0, m_anchor.y());
    const int width = qMin(FlyoutWidth, qMax(0, this->width() - x - FlyoutMargin));
    const int height = qMax(0, this->height() - y - FlyoutMargin);
    return {x, y, width, height};
}

void VaultGroupsFlyout::open()
{
    if (!parentWidget()) {
        return;
    }
    setGeometry(parentWidget()->rect());
    m_panel->setGeometry(panelRect());
    raise();
    show();
    qApp->installEventFilter(this);
    // The keyboard goes to the groups, so Escape and the arrow keys work straight away
    if (m_pane) {
        if (auto* view = m_pane->findChild<QAbstractItemView*>()) {
            view->setFocus(Qt::PopupFocusReason);
        }
    }
}

void VaultGroupsFlyout::close()
{
    if (isHidden()) {
        return;
    }
    qApp->removeEventFilter(this);
    hide();
    emit closed();
}

bool VaultGroupsFlyout::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        setGeometry(parentWidget()->rect());
        return false;
    }
    if (isVisible() && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        if (keyEvent->key() == Qt::Key_Escape && widget && isAncestorOf(widget)) {
            // Escape closes the flyout before any window shortcut sees it
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
            } else {
                close();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VaultGroupsFlyout::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // A light scrim over the entries, as in the web app
    painter.fillRect(rect(), QColor(11, 15, 12, FluentTheme::isDark() ? 90 : 31));

    const QRectF r = QRectF(panelRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    constexpr qreal radius = VaultMetrics::SurfaceRadius;
    if (!p.highContrast) {
        for (int i = 1; i <= 8; ++i) {
            QPainterPath shadow;
            shadow.addRoundedRect(r.adjusted(-i / 2.0, i, i / 2.0, i * 1.5), radius + i, radius + i);
            painter.fillPath(shadow, QColor(14, 25, 10, 6));
        }
    }
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);
    painter.fillPath(path, p.surface);
    painter.setPen(QPen(p.highContrast ? p.strokeStrong : p.stroke, 1));
    painter.drawPath(path);
}

void VaultGroupsFlyout::mousePressEvent(QMouseEvent* event)
{
    // Only the scrim reaches here: the panel's children take their own clicks
    if (!panelRect().contains(event->position().toPoint())) {
        close();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VaultGroupsFlyout::resizeEvent(QResizeEvent* event)
{
    m_panel->setGeometry(panelRect());
    QWidget::resizeEvent(event);
}

VaultResponsiveLayout::VaultResponsiveLayout(const Parts& parts, QObject* parent)
    : QObject(parent)
    , m_parts(parts)
    , m_flyout(new VaultGroupsFlyout(parts.host))
{
    m_parts.host->installEventFilter(this);

    connect(m_parts.commandBar, &VaultCommandBar::groupsRequested, this, [this] {
        if (isGroupsFlyoutOpen()) {
            closeGroups();
        } else {
            openGroups();
        }
    });
    connect(m_parts.commandBar, &VaultCommandBar::backRequested, this, &VaultResponsiveLayout::showList);
    connect(m_flyout, &VaultGroupsFlyout::closed, this, [this] { m_parts.commandBar->setGroupsOpen(false); });

    // Picking an entry from the list opens its details when they don't fit beside it
    connect(m_parts.entryView, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
        if (index.isValid() && m_layout == Vault::Layout::Narrow) {
            showDetails();
        }
    });
    // Picking a group, even the current one, closes the flyout
    for (auto* view : m_parts.groupViews) {
        connect(view, &QAbstractItemView::clicked, this, [this] { closeGroups(); });
        connect(view, &QAbstractItemView::activated, this, [this] { closeGroups(); });
    }
}

Vault::Layout VaultResponsiveLayout::layout() const
{
    return m_layout;
}

bool VaultResponsiveLayout::isShowingDetails() const
{
    return m_details;
}

bool VaultResponsiveLayout::isGroupsFlyoutOpen() const
{
    return m_flyout->isVisible();
}

QList<int> VaultResponsiveLayout::mainSplitterSizes() const
{
    return m_layout == Vault::Layout::Wide ? m_parts.mainSplitter->sizes() : m_wideMainSizes;
}

void VaultResponsiveLayout::setMainSplitterSizes(const QList<int>& sizes)
{
    if (m_layout == Vault::Layout::Wide) {
        m_parts.mainSplitter->setSizes(sizes);
    } else {
        m_wideMainSizes = sizes;
    }
}

QList<int> VaultResponsiveLayout::previewSplitterSizes() const
{
    return m_layout == Vault::Layout::Narrow ? m_previewSizes : m_parts.previewSplitter->sizes();
}

void VaultResponsiveLayout::setPreviewSplitterSizes(const QList<int>& sizes)
{
    if (m_layout == Vault::Layout::Narrow) {
        m_previewSizes = sizes;
    } else {
        m_parts.previewSplitter->setSizes(sizes);
    }
}

void VaultResponsiveLayout::setEditing(bool editing)
{
    m_editing = editing;
    if (editing) {
        closeGroups();
    }
    updatePanes();
}

void VaultResponsiveLayout::setHasEntry(bool hasEntry)
{
    if (!hasEntry && m_details) {
        showList();
    }
}

void VaultResponsiveLayout::showList()
{
    m_details = false;
    updatePanes();
}

void VaultResponsiveLayout::showDetails()
{
    m_details = true;
    updatePanes();
}

void VaultResponsiveLayout::openGroups()
{
    if (m_layout == Vault::Layout::Wide || m_editing) {
        return;
    }
    placeFlyout();
    m_flyout->open();
    m_parts.commandBar->setGroupsOpen(true);
}

void VaultResponsiveLayout::closeGroups()
{
    m_flyout->close();
}

void VaultResponsiveLayout::update()
{
    apply(Vault::layoutForWidth(m_parts.host->width()));
}

bool VaultResponsiveLayout::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_parts.host && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        update();
        if (isGroupsFlyoutOpen()) {
            placeFlyout();
        }
    }
    return QObject::eventFilter(watched, event);
}

void VaultResponsiveLayout::apply(Vault::Layout layout)
{
    if (layout == m_layout) {
        return;
    }
    const auto previous = m_layout;
    m_layout = layout;

    // The groups pane: beside the surface when wide, otherwise in the flyout
    auto* splitter = m_parts.mainSplitter;
    if (layout == Vault::Layout::Wide) {
        closeGroups();
        m_flyout->setPane(nullptr);
        splitter->insertWidget(0, m_parts.navPane);
        m_parts.navPane->show();
        if (m_wideMainSizes.size() == 2) {
            splitter->setSizes(m_wideMainSizes);
        }
    } else if (previous == Vault::Layout::Wide) {
        m_wideMainSizes = splitter->sizes();
        m_flyout->setPane(m_parts.navPane);
    }

    // The previous two-pane split is kept while the panes take turns
    if (layout == Vault::Layout::Narrow) {
        m_previewSizes = m_parts.previewSplitter->sizes();
    }

    switch (layout) {
    case Vault::Layout::Wide:
        m_parts.surface->setGutters(0, VaultSurface::Gutter, VaultSurface::Gutter);
        break;
    case Vault::Layout::Medium:
        m_parts.surface->setGutters(VaultSurface::Gutter, VaultSurface::Gutter, VaultSurface::Gutter);
        break;
    case Vault::Layout::Narrow:
        m_parts.surface->setGutters(8, 8, 8);
        break;
    }

    const bool narrow = layout == Vault::Layout::Narrow;
    if (auto* listLayout = m_parts.listHost->layout()) {
        listLayout->setContentsMargins(narrow ? QMargins(4, 0, 4, 8) : QMargins(6, 0, 0, 14));
    }
    if (auto* cardLayout = m_parts.cardHost->layout()) {
        cardLayout->setContentsMargins(narrow ? QMargins(6, 0, 6, 8) : QMargins(4, 0, 14, 14));
    }
    m_parts.detailCard->setStacked(narrow);

    updatePanes();
    if (previous == Vault::Layout::Narrow && m_previewSizes.size() == 2) {
        m_parts.previewSplitter->setSizes(m_previewSizes);
    }
    emit layoutChanged(layout);
}

void VaultResponsiveLayout::updatePanes()
{
    const bool narrow = m_layout == Vault::Layout::Narrow;
    // Editing shows the editor where the details go, so it needs the details pane
    const bool details = narrow && (m_details || m_editing);
    m_parts.listHost->setVisible(!details);
    m_parts.cardHost->setVisible(!narrow || details);
    m_parts.commandBar->setLayoutMode(m_layout, details);
}

void VaultResponsiveLayout::placeFlyout()
{
    // Just below the view title, in line with the surface's content
    const auto* bar = m_parts.commandBar;
    const QPoint below = bar->mapTo(m_parts.host, QPoint(0, bar->height()));
    m_flyout->setGeometry(m_parts.host->rect());
    m_flyout->setAnchor(QPoint(below.x() + 8, below.y() - 4));
}
