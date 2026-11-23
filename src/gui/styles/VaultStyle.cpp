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
    case CC_ScrollBar: {
        // Fusion draws the thumb in button colours, which match the window here: fill it with the text colour
        const auto slider = subControlRect(control, option, SC_ScrollBarSlider, widget);
        if (!slider.isEmpty() && (option->state & State_Enabled)) {
            painter->fillRect(slider.adjusted(2, 2, -2, -2), option->palette.color(QPalette::WindowText));
        }
        break;
    }
    default:
        break;
    }
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
