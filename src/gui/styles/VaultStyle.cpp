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

#include "VaultStyle.h"

#include "gui/styles/FluentTheme.h"

#include <QAbstractItemView>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QStyleFactory>
#include <QStyleOption>
#include <QWizard>

namespace
{
    // A 1px outline in the text colour just inside the rectangle; high contrast only
    void outline(QPainter* painter, const QRect& rect, const QStyleOption* option, int radius = 0)
    {
        if (!FluentTheme::isHighContrast() || rect.isEmpty()) {
            return;
        }
        painter->save();
        painter->setBrush(Qt::NoBrush);
        const bool focused = option->state & QStyle::State_HasFocus;
        const auto color = option->palette.color(focused ? QPalette::Highlight : QPalette::WindowText);
        if (radius > 0) {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(color, 1));
            painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        } else {
            painter->setPen(QPen(color, 1));
            painter->drawRect(rect.adjusted(0, 0, -1, -1));
        }
        painter->restore();
    }

    // Slim scroll bars, matching VaultScrollBar (the Fluent ScrollBar at 8px) on the views that use it
    constexpr int ScrollBarExtent = 10;
    constexpr int ScrollBarThumbIdle = 4;
    constexpr int ScrollBarThumbHover = 8;
    constexpr int ScrollBarThumbMinimum = 32;
    constexpr int ScrollBarInset = 2;

    // The thumb's rectangle along a bar with no arrows: the groove is the whole bar
    QRect scrollBarSlider(const QStyleOptionSlider* bar)
    {
        const bool vertical = bar->orientation == Qt::Vertical;
        const QRect groove = bar->rect;
        const int length = vertical ? groove.height() : groove.width();
        const int range = bar->maximum - bar->minimum;
        int thumb = length;
        if (range > 0) {
            const qint64 page = qMax(1, bar->pageStep);
            thumb = int(qint64(length) * page / (range + page));
            thumb = qBound(qMin(ScrollBarThumbMinimum, length), thumb, length);
        }
        const int position = range > 0
                                 ? QStyle::sliderPositionFromValue(
                                       bar->minimum, bar->maximum, bar->sliderPosition, length - thumb, bar->upsideDown)
                                 : 0;
        return vertical ? QRect(groove.x(), groove.y() + position, groove.width(), thumb)
                        : QRect(groove.x() + position, groove.y(), thumb, groove.height());
    }
} // namespace

VaultStyle::VaultStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
}

void VaultStyle::drawPrimitive(PrimitiveElement element,
                               const QStyleOption* option,
                               QPainter* painter,
                               const QWidget* widget) const
{
    switch (element) {
    case PE_PanelTipLabel: {
        // Fluent tooltips: a solid layer with a hairline stroke
        const auto p = FluentTheme::palette();
        painter->save();
        painter->fillRect(option->rect, option->palette.color(QPalette::ToolTipBase));
        painter->setPen(p.highContrast ? option->palette.color(QPalette::ToolTipText) : p.stroke);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(option->rect.adjusted(0, 0, -1, -1));
        painter->restore();
        return;
    }
    case PE_IndicatorHeaderArrow: {
        // A small chevron instead of a filled triangle, as in Fluent lists
        const auto* header = qstyleoption_cast<const QStyleOptionHeader*>(option);
        if (!header) {
            break;
        }
        // Qt marks an ascending column SortDown; like Fusion, show it pointing up
        const bool up = header->sortIndicator == QStyleOptionHeader::SortDown;
        const bool down = header->sortIndicator == QStyleOptionHeader::SortUp;
        if (!up && !down) {
            return;
        }
        const auto p = FluentTheme::palette();
        const QPointF c = QRectF(option->rect).center();
        const qreal w = 4.0;
        const qreal h = 2.0;
        QPainterPath chevron;
        if (up) {
            chevron.moveTo(c.x() - w, c.y() + h);
            chevron.lineTo(c.x(), c.y() - h);
            chevron.lineTo(c.x() + w, c.y() + h);
        } else {
            chevron.moveTo(c.x() - w, c.y() - h);
            chevron.lineTo(c.x(), c.y() + h);
            chevron.lineTo(c.x() + w, c.y() - h);
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(
            QPen(p.highContrast ? p.text : p.textSecondary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(chevron);
        painter->restore();
        return;
    }
    default:
        break;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);

    switch (element) {
    case PE_FrameLineEdit:
    case PE_FrameGroupBox:
    case PE_FrameTabWidget:
    case PE_FrameMenu:
    case PE_FrameDockWidget:
    case PE_Frame:
        outline(painter, option->rect, option);
        break;
    case PE_PanelButtonCommand:
        outline(painter, option->rect, option, 2);
        break;
    case PE_PanelButtonTool:
        if (option->state & (State_Raised | State_Sunken | State_On | State_MouseOver)) {
            outline(painter, option->rect, option, 2);
        }
        break;
    default:
        break;
    }
}

void VaultStyle::drawControl(ControlElement element,
                             const QStyleOption* option,
                             QPainter* painter,
                             const QWidget* widget) const
{
    QProxyStyle::drawControl(element, option, painter, widget);

    switch (element) {
    case CE_ProgressBarGroove:
        outline(painter, option->rect, option);
        break;
    case CE_TabBarTabShape:
        if (!(option->state & State_Selected)) {
            outline(painter, option->rect, option);
        } else if (FluentTheme::isHighContrast()) {
            // The selected tab gets a heavier edge, so selection isn't shown by colour alone
            QStyleOption thick(*option);
            thick.state |= State_HasFocus;
            outline(painter, option->rect, &thick);
            outline(painter, option->rect.adjusted(1, 1, -1, -1), &thick);
        }
        break;
    default:
        break;
    }
}

void VaultStyle::drawComplexControl(ComplexControl control,
                                    const QStyleOptionComplex* option,
                                    QPainter* painter,
                                    const QWidget* widget) const
{
    if (control == CC_ScrollBar) {
        if (const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            // No arrows or track: a thin rounded thumb, wider while the bar is hovered or dragged
            const auto p = FluentTheme::palette();
            const bool vertical = bar->orientation == Qt::Vertical;
            const bool active = (bar->state & State_MouseOver) || (bar->state & State_Sunken)
                                || (bar->activeSubControls & SC_ScrollBarSlider);
            const bool highContrast = FluentTheme::isHighContrast();
            if (!(bar->state & State_Enabled) || bar->maximum <= bar->minimum) {
                return;
            }
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            const int thickness = active || highContrast ? ScrollBarThumbHover : ScrollBarThumbIdle;
            QRectF thumb = scrollBarSlider(bar);
            if (vertical) {
                thumb.setLeft(thumb.right() + 1 - ScrollBarInset - thickness);
                thumb.setWidth(thickness);
                thumb.adjust(0, ScrollBarInset, 0, -ScrollBarInset);
            } else {
                thumb.setTop(thumb.bottom() + 1 - ScrollBarInset - thickness);
                thumb.setHeight(thickness);
                thumb.adjust(ScrollBarInset, 0, -ScrollBarInset, 0);
            }
            QColor color = highContrast ? p.text : (active ? p.textSecondary : p.textTertiary);
            if (!highContrast) {
                color.setAlphaF(active ? 0.75 : 0.5);
            }
            painter->setBrush(color);
            const qreal radius = thickness / 2.0;
            painter->drawRoundedRect(thumb, radius, radius);
            painter->restore();
            return;
        }
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);

    if (!FluentTheme::isHighContrast()) {
        return;
    }
    switch (control) {
    case CC_SpinBox:
        if (const auto* spin = qstyleoption_cast<const QStyleOptionSpinBox*>(option); spin && spin->frame) {
            outline(painter, option->rect, option, 2);
        }
        break;
    case CC_ComboBox:
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option); combo && combo->frame) {
            outline(painter, option->rect, option, 2);
        }
        break;
    case CC_ScrollBar:
        // Drawn in full above, a solid thumb in the text colour in high contrast
        break;
    default:
        break;
    }
}

QRect VaultStyle::subControlRect(ComplexControl control,
                                 const QStyleOptionComplex* option,
                                 SubControl subControl,
                                 const QWidget* widget) const
{
    if (control == CC_ScrollBar) {
        if (const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option)) {
            const QRect slider = scrollBarSlider(bar);
            const bool vertical = bar->orientation == Qt::Vertical;
            switch (subControl) {
            case SC_ScrollBarAddLine:
            case SC_ScrollBarSubLine:
                return {};
            case SC_ScrollBarGroove:
                return bar->rect;
            case SC_ScrollBarSlider:
                return slider;
            case SC_ScrollBarSubPage:
                return vertical
                           ? QRect(bar->rect.left(), bar->rect.top(), bar->rect.width(), slider.top() - bar->rect.top())
                           : QRect(bar->rect.left(),
                                   bar->rect.top(),
                                   slider.left() - bar->rect.left(),
                                   bar->rect.height());
            case SC_ScrollBarAddPage:
                return vertical ? QRect(bar->rect.left(),
                                        slider.bottom() + 1,
                                        bar->rect.width(),
                                        bar->rect.bottom() - slider.bottom())
                                : QRect(slider.right() + 1,
                                        bar->rect.top(),
                                        bar->rect.right() - slider.right(),
                                        bar->rect.height());
            default:
                return {};
            }
        }
    }
    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

QStyle::SubControl VaultStyle::hitTestComplexControl(ComplexControl control,
                                                     const QStyleOptionComplex* option,
                                                     const QPoint& position,
                                                     const QWidget* widget) const
{
    if (control == CC_ScrollBar) {
        for (auto sub : {SC_ScrollBarSlider, SC_ScrollBarSubPage, SC_ScrollBarAddPage}) {
            if (subControlRect(control, option, sub, widget).contains(position)) {
                return sub;
            }
        }
        return SC_None;
    }
    return QProxyStyle::hitTestComplexControl(control, option, position, widget);
}

int VaultStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent:
        return ScrollBarExtent;
    case PM_ScrollBarSliderMin:
        return ScrollBarThumbMinimum;
    case PM_ScrollView_ScrollBarSpacing:
    case PM_ScrollView_ScrollBarOverlap:
        return 0;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

QRect VaultStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const
{
    switch (element) {
    // Fluent check boxes and radio buttons draw their own indicator and text, so Fusion's idea of where those are
    // doesn't match: the whole control takes the click, as in WinUI
    case SE_CheckBoxClickRect:
    case SE_RadioButtonClickRect:
        return option->rect;
    default:
        break;
    }
    return QProxyStyle::subElementRect(element, option, widget);
}

int VaultStyle::styleHint(StyleHint hint,
                          const QStyleOption* option,
                          const QWidget* widget,
                          QStyleHintReturn* returnData) const
{
    switch (hint) {
    // Row delegates draw the selection; Fusion would also fill the branch area beside a tree row. List views
    // keep it, or the selection would stop short of the icon.
    case SH_ItemView_ShowDecorationSelected:
        return qobject_cast<const QListView*>(widget) ? 1 : 0;
    // Thin scroll bars over the content that show while scrolling, as in Fluent
    case SH_ScrollBar_Transient:
        return 1;
    case SH_ItemView_ScrollMode:
        return QAbstractItemView::ScrollPerPixel;
    case SH_ItemView_ArrowKeysNavigateIntoChildren:
    case SH_ItemView_MovementWithoutUpdatingSelection:
    case SH_ItemView_ChangeHighlightOnFocus:
    case SH_ComboBox_ListMouseTracking:
    case SH_Menu_MouseTracking:
    case SH_ScrollBar_MiddleClickAbsolutePosition:
        return 1;
    case SH_DialogButtonBox_ButtonsHaveIcons:
    case SH_ScrollView_FrameOnlyAroundContents:
    case SH_UnderlineShortcut:
        return 0;
    // ImportWizard keeps a QWizard as its invisible page controller; its pages sit in a Fluent stepper
    case SH_WizardStyle:
        return QWizard::ClassicStyle;
    case SH_MessageBox_TextInteractionFlags:
        return Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}
