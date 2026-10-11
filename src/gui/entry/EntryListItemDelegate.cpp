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

#include "EntryListItemDelegate.h"

#include "core/Entry.h"
#include "core/PasswordHealth.h"
#include "gui/entry/EntryModel.h"
#include "gui/entry/EntryView.h"
#include "gui/styles/FluentTheme.h"

#include <FluentQt/Foundation.h>

#include <QPainter>
#include <QPainterPath>

namespace
{
    constexpr int RowInset = 4;
    constexpr int TileSize = 32;
    constexpr int IconSize = 20;
    constexpr int BadgeHeight = 20;

    QFont fluentFont(Typography::FontRole role)
    {
        return fluent::ThemeRegistry::instance().resolvedFontStyle(role).toQFont();
    }

    QString columnText(const QModelIndex& index, int column)
    {
        return index.sibling(index.row(), column).data(Qt::DisplayRole).toString();
    }
} // namespace

EntryListItemDelegate::EntryListItemDelegate(EntryView* view)
    : QStyledItemDelegate(view)
    , m_view(view)
{
}

QSize EntryListItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    Q_UNUSED(index)
    return {option.rect.width(), RowHeight};
}

void EntryListItemDelegate::paint(QPainter* painter,
                                  const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const
{
    const auto p = FluentTheme::palette();
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    // Row background: accent tint for the selection, subtle fill on hover
    QRectF row = QRectF(option.rect).adjusted(RowInset, 2, -RowInset, -2);
    if (selected || hovered) {
        QPainterPath path;
        path.addRoundedRect(row, 4, 4);
        painter->fillPath(path, selected ? p.accentSubtle : p.subtleHover);
    }
    if (selected) {
        QRectF pill(row.left(), row.center().y() - 8, 3, 16);
        QPainterPath path;
        path.addRoundedRect(pill, 1.5, 1.5);
        painter->fillPath(path, p.accent);
    }

    // Icon tile
    QRectF tile(row.left() + 10, row.center().y() - TileSize / 2.0, TileSize, TileSize);
    QPainterPath tilePath;
    tilePath.addRoundedRect(tile, 6, 6);
    painter->fillPath(tilePath, p.inset);
    painter->setPen(QPen(p.stroke, 1));
    painter->drawPath(tilePath);
    const auto icon = index.data(Qt::DecorationRole);
    QPixmap pixmap;
    if (icon.canConvert<QPixmap>()) {
        pixmap = icon.value<QPixmap>();
    } else if (icon.canConvert<QIcon>()) {
        pixmap = icon.value<QIcon>().pixmap(IconSize, IconSize);
    }
    if (!pixmap.isNull()) {
        QRectF iconRect(tile.center().x() - IconSize / 2.0, tile.center().y() - IconSize / 2.0, IconSize, IconSize);
        painter->drawPixmap(iconRect.toRect(), pixmap);
    }

    // State badge on the right
    auto* entry = m_view->entryFromIndex(index);
    QString badgeText;
    QColor badgeFg, badgeBg;
    if (entry && entry->isExpired()) {
        badgeText = tr("Expired");
        badgeFg = p.critical;
        badgeBg = p.criticalBg;
    } else if (entry && !entry->excludeFromReports() && !entry->password().isEmpty()) {
        auto health = entry->passwordHealth();
        if (health && health->quality() <= PasswordHealth::Quality::Weak) {
            badgeText = tr("Weak");
            badgeFg = p.caution;
            badgeBg = p.cautionBg;
        }
    }

    qreal textRight = row.right() - 10;
    if (!badgeText.isEmpty()) {
        const auto badgeFont = fluentFont(Typography::FontRole::Caption);
        QFont strong(badgeFont);
        strong.setWeight(QFont::DemiBold);
        const int badgeWidth = QFontMetrics(strong).horizontalAdvance(badgeText) + 16;
        QRectF badge(row.right() - 10 - badgeWidth, row.center().y() - BadgeHeight / 2.0, badgeWidth, BadgeHeight);
        QPainterPath badgePath;
        badgePath.addRoundedRect(badge, BadgeHeight / 2.0, BadgeHeight / 2.0);
        painter->fillPath(badgePath, badgeBg);
        painter->setFont(strong);
        painter->setPen(badgeFg);
        painter->drawText(badge, Qt::AlignCenter, badgeText);
        textRight = badge.left() - 8;
    }

    // Title and "username · URL" (or group while searching)
    const qreal textLeft = tile.right() + 12;
    const qreal textWidth = qMax<qreal>(0, textRight - textLeft);

    QStringList details;
    const auto username = columnText(index, EntryModel::Username);
    if (!username.isEmpty()) {
        details << username;
    }
    const auto url = columnText(index, EntryModel::Url);
    if (!url.isEmpty()) {
        details << url;
    }
    if (m_view->inSearchMode()) {
        const auto group = columnText(index, EntryModel::ParentGroup);
        if (!group.isEmpty()) {
            details << group;
        }
    }

    auto titleFont = fluentFont(Typography::FontRole::BodyStrong);
    auto detailFont = fluentFont(Typography::FontRole::Caption);
    QFontMetrics titleMetrics(titleFont);
    QFontMetrics detailMetrics(detailFont);
    const qreal blockHeight = titleMetrics.height() + (details.isEmpty() ? 0 : detailMetrics.height());
    qreal y = row.center().y() - blockHeight / 2.0;

    painter->setFont(titleFont);
    painter->setPen(p.text);
    const auto title = titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(textWidth));
    painter->drawText(QRectF(textLeft, y, textWidth, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter, title);
    y += titleMetrics.height();

    if (!details.isEmpty()) {
        painter->setFont(detailFont);
        painter->setPen(p.textSecondary);
        const auto detail =
            detailMetrics.elidedText(details.join(QStringLiteral(" · ")), Qt::ElideRight, int(textWidth));
        painter->drawText(
            QRectF(textLeft, y, textWidth, detailMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter, detail);
    }

    painter->restore();
}
