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

#ifndef OPENVIREO_VAULTRESPONSIVE_H
#define OPENVIREO_VAULTRESPONSIVE_H

#include <QPointer>
#include <QWidget>

#include <optional>

class EntryDetailCard;
class QAbstractItemView;
class QSplitter;
class VaultCommandBar;
class VaultSurface;

namespace Vault
{
    // How the vault is laid out at a window width, at the same breakpoints as the web app
    enum class Layout
    {
        // Groups, entries and details side by side
        Wide,
        // Groups open from the view title over the entries; entries and details side by side
        Medium,
        // One pane at a time: the entries, or one entry's details
        Narrow
    };

    // Widths in device-independent pixels: Wide from WideWidth up, Narrow below NarrowWidth
    constexpr int WideWidth = 1100;
    constexpr int NarrowWidth = 760;
    // The main window's narrowest width
    constexpr int MinimumWindowWidth = 400;

    Layout layoutForWidth(int width);
    // Tests pin one layout whatever the window's width (CI screens can be narrower than WideWidth);
    // std::nullopt follows the width again
    void setLayoutForTests(std::optional<Layout> layout);
} // namespace Vault

/**
 * The groups, dropped down from the view title over the entries while the window is too narrow for the groups pane.
 * Covers its host with a light scrim; clicking the scrim or pressing Escape closes it.
 */
class VaultGroupsFlyout : public QWidget
{
    Q_OBJECT

public:
    explicit VaultGroupsFlyout(QWidget* host);

    // Holds the groups pane, taken out of the window's layout
    void setPane(QWidget* pane);
    QWidget* pane() const;
    // The panel's top left corner, in the host's coordinates
    void setAnchor(const QPoint& topLeft);
    QRect panelRect() const;

    void open();
    void close();

signals:
    void closed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* m_panel;
    QPointer<QWidget> m_pane;
    QPoint m_anchor;
};

/**
 * Lays the vault out for the window's width: in Wide all three panes side by side, in Medium the groups pane folds
 * into a flyout opened from the view title, in Narrow the entries and details take turns.
 */
class VaultResponsiveLayout : public QObject
{
    Q_OBJECT

public:
    struct Parts
    {
        QWidget* host;
        QSplitter* mainSplitter;
        QWidget* navPane;
        VaultSurface* surface;
        QSplitter* previewSplitter;
        QWidget* listHost;
        QWidget* cardHost;
        VaultCommandBar* commandBar;
        EntryDetailCard* detailCard;
        QAbstractItemView* entryView;
        QList<QAbstractItemView*> groupViews;
    };

    VaultResponsiveLayout(const Parts& parts, QObject* parent = nullptr);

    Vault::Layout layout() const;
    // In Narrow: the entry's details are shown in place of the list
    bool isShowingDetails() const;
    bool isGroupsFlyoutOpen() const;

    // The splitters' sizes in the wide layout, also while the window is narrower
    QList<int> mainSplitterSizes() const;
    void setMainSplitterSizes(const QList<int>& sizes);
    QList<int> previewSplitterSizes() const;
    void setPreviewSplitterSizes(const QList<int>& sizes);

    // An entry is being edited in place of its details
    void setEditing(bool editing);
    // The current entry changed; without one there are no details to show
    void setHasEntry(bool hasEntry);

public slots:
    void showList();
    void showDetails();
    void openGroups();
    void closeGroups();
    // Lay out again for the host's width
    void update();

signals:
    void layoutChanged(Vault::Layout layout);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void apply(Vault::Layout layout);
    void updatePanes();
    void placeFlyout();

    Parts m_parts;
    VaultGroupsFlyout* m_flyout;
    Vault::Layout m_layout = Vault::Layout::Wide;
    QList<int> m_wideMainSizes;
    QList<int> m_previewSizes;
    bool m_details = false;
    bool m_editing = false;
};

#endif // OPENVIREO_VAULTRESPONSIVE_H
