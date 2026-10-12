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

#include "VaultSettingsModal.h"

#include "gui/CategoryListWidget.h"
#include "gui/EditWidget.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>

namespace
{
    constexpr int NavigationWidth = 220;
    constexpr int NavigationRowHeight = 32;

    // Slack draws its icons in the text color, so recolor the theme icons the same way
    QPixmap tintedPixmap(const QIcon& icon, const QSize& size, qreal dpr, const QColor& color)
    {
        QPixmap pixmap = icon.pixmap(size, dpr);
        if (pixmap.isNull()) {
            return pixmap;
        }
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        return pixmap;
    }

    class NavigationDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(index)
            return {option.rect.width(), NavigationRowHeight};
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const auto colors = VaultModal::colors();
            const bool selected = option.state & QStyle::State_Selected;
            const bool hovered = option.state & QStyle::State_MouseOver;

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(option.rect);
            if (selected || hovered) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(selected ? colors.selected : colors.hover);
                painter->drawRoundedRect(box, 6, 6);
            }

            const QColor foreground = selected ? colors.onSelected : colors.text;
            const QSize iconSize(18, 18);
            const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
            const auto icon = tintedPixmap(index.data(Qt::DecorationRole).value<QIcon>(), iconSize, dpr, foreground);
            const QRect iconRect(option.rect.left() + 12,
                                 option.rect.top() + (option.rect.height() - iconSize.height()) / 2,
                                 iconSize.width(),
                                 iconSize.height());
            painter->drawPixmap(iconRect, icon);

            painter->setFont(VaultModal::pixelFont(option.font, 15, selected ? QFont::DemiBold : QFont::Normal));
            painter->setPen(foreground);
            const QRect textRect = option.rect.adjusted(iconRect.right() + 10 - option.rect.left(), 0, -8, 0);
            const auto text = painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
            painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);

            if (option.state & QStyle::State_HasFocus && !selected) {
                painter->setPen(QPen(colors.selected, 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(box.adjusted(1, 1, -1, -1), 5, 5);
            }
            painter->restore();
        }
    };
} // namespace

VaultSettingsModal::VaultSettingsModal(QWidget* window,
                                       EditWidget* settings,
                                       const QString& title,
                                       const QString& namePrefix)
    : VaultSettingsModal(window, settings, settings->categoryList(), settings->buttonBox(), title, namePrefix)
{
    settings->headlineLabel()->hide();
}

VaultSettingsModal::VaultSettingsModal(QWidget* window,
                                       QWidget* content,
                                       CategoryListWidget* categories,
                                       QDialogButtonBox* buttons,
                                       const QString& title,
                                       const QString& namePrefix)
    : VaultModal(window, content, title, namePrefix)
    , m_categories(categories)
    , m_buttons(buttons)
{
    // The sidebar shares the widget's own category model and selection
    auto list = categories->listWidget();
    m_navigation = new QListView(card());
    m_navigation->setObjectName(namePrefix + "Navigation");
    m_navigation->setModel(list->model());
    m_navigation->setSelectionModel(list->selectionModel());
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation));
    m_navigation->setFrameShape(QFrame::NoFrame);
    m_navigation->setFixedWidth(NavigationWidth);
    m_navigation->setSpacing(1);
    m_navigation->setMouseTracking(true);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_navigation->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_navigation->viewport()->setAutoFillBackground(false);
    m_navigation->viewport()->setCursor(Qt::PointingHandCursor);
    m_navigation->setAccessibleName(tr("%1 categories").arg(title));
    bodyLayout()->insertWidget(0, m_navigation);
    setInitialFocus(m_navigation);

    // The modal provides the title, categories and buttons
    categories->hide();
    buttons->hide();

    if (buttons->button(QDialogButtonBox::Ok)) {
        auto cancelButton = addFooterButton(tr("Cancel"), false, QStringLiteral("Cancel"));
        connect(cancelButton, &QPushButton::clicked, this, &VaultSettingsModal::reject);
        auto saveButton = addFooterButton(tr("Save"), true, QStringLiteral("Save"));
        connect(saveButton, &QPushButton::clicked, this, &VaultSettingsModal::accept);
    } else {
        auto closeButton = addFooterButton(tr("Close"), true, QStringLiteral("Done"));
        connect(closeButton, &QPushButton::clicked, this, &VaultSettingsModal::reject);
    }

    setTabOrder(m_navigation, content);
}

QListView* VaultSettingsModal::navigation() const
{
    return m_navigation;
}

void VaultSettingsModal::accept()
{
    if (auto button = m_buttons->button(QDialogButtonBox::Ok)) {
        button->click();
    }
}

void VaultSettingsModal::reject()
{
    for (auto which : {QDialogButtonBox::Cancel, QDialogButtonBox::Close}) {
        if (auto button = m_buttons->button(which)) {
            button->click();
            return;
        }
    }
}

void VaultSettingsModal::prepare()
{
    // Pages can be hidden on the widget's own list, which isn't shared with the sidebar
    auto categories = m_categories->listWidget();
    for (int row = 0; row < categories->count(); ++row) {
        m_navigation->setRowHidden(row, categories->isRowHidden(row));
    }
    if (!m_navigation->currentIndex().isValid() && categories->count() > 0) {
        categories->setCurrentRow(0);
    }
}
