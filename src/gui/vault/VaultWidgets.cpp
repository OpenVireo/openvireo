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

#include "VaultWidgets.h"

#include "gui/styles/FluentTheme.h"

#include <components/foundation/ThemeRegistry.h>

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>

QFont Vault::font(Typography::FontRole role)
{
    return fluent::ThemeRegistry::instance().resolvedFontStyle(role).toQFont();
}

void Vault::styleLabel(QLabel* label, Typography::FontRole role, const QColor& color)
{
    label->setFont(font(role));
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
}

fluent::basicinput::Button* Vault::iconButton(const QString& glyph, const QString& toolTip, QWidget* parent)
{
    auto* button = new fluent::basicinput::Button(parent);
    button->setFluentStyle(fluent::basicinput::Button::Subtle);
    button->setFluentLayout(fluent::basicinput::Button::IconOnly);
    button->setIconGlyph(glyph);
    button->setToolTip(toolTip);
    button->setAccessibleName(toolTip);
    return button;
}

void Vault::bindAction(QAbstractButton* button, QAction* action)
{
    if (!action) {
        button->setVisible(false);
        return;
    }
    QObject::connect(button, &QAbstractButton::clicked, action, &QAction::trigger);
    auto sync = [button, action] {
        button->setEnabled(action->isEnabled());
        button->setVisible(action->isVisible());
    };
    QObject::connect(action, &QAction::changed, button, sync);
    sync();
}

void Vault::bindAction(QAbstractButton* button, const QString& actionName)
{
    bindAction(button, FluentTheme::action(actionName));
}

VaultFieldRow::VaultFieldRow(const QString& caption, QWidget* parent)
    : QWidget(parent)
    , m_caption(new QLabel(caption, this))
    , m_value(new QLabel(this))
    , m_buttons(new QHBoxLayout())
{
    setAttribute(Qt::WA_Hover);
    setMinimumHeight(48);

    m_caption->setFixedWidth(96);
    m_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_value->setMinimumWidth(0);
    m_value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 6, 8, 6);
    layout->setSpacing(12);
    layout->addWidget(m_caption);
    layout->addWidget(m_value, 1);
    m_buttons->setSpacing(2);
    layout->addLayout(m_buttons);

    applyColors();
}

QLabel* VaultFieldRow::caption() const
{
    return m_caption;
}

QLabel* VaultFieldRow::value() const
{
    return m_value;
}

fluent::basicinput::Button* VaultFieldRow::addButton(const QString& glyph, const QString& toolTip)
{
    auto* button = Vault::iconButton(glyph, toolTip, this);
    button->setFluentSize(fluent::basicinput::Button::Small);
    m_buttons->addWidget(button);
    return button;
}

void VaultFieldRow::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_caption, Typography::FontRole::Caption, p.textSecondary);
    Vault::styleLabel(m_value, Typography::FontRole::Body, p.text);
    update();
}

void VaultFieldRow::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void VaultFieldRow::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

void VaultFieldRow::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    if (!m_hovered) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 4, 4);
    painter.fillPath(path, FluentTheme::palette().subtleHover);
}
