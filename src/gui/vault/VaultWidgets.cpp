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

#include <components/foundation/FluentElement.h>
#include <components/foundation/ThemeRegistry.h>

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>

namespace
{
    class ThemeFollower : public QObject, public fluent::FluentElement
    {
    public:
        ThemeFollower(QObject* owner, std::function<void()> apply)
            : QObject(owner)
            , m_apply(std::move(apply))
        {
        }

        void onThemeUpdated() override
        {
            m_apply();
        }

    private:
        std::function<void()> m_apply;
    };

    class CanvasPainter : public QObject
    {
    public:
        using QObject::QObject;

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Paint) {
                auto* widget = static_cast<QWidget*>(watched);
                QPainter painter(widget);
                painter.fillRect(widget->rect(), FluentTheme::palette().canvas);
            }
            return QObject::eventFilter(watched, event);
        }
    };
} // namespace

void Vault::followTheme(QObject* owner, std::function<void()> apply)
{
    apply();
    new ThemeFollower(owner, std::move(apply));
}

void Vault::paintCanvas(QWidget* widget)
{
    widget->setAutoFillBackground(false);
    widget->installEventFilter(new CanvasPainter(widget));
    // Repaint with the new canvas color when the theme changes
    followTheme(widget, [widget] { widget->update(); });
}

QString Vault::formStyleSheet()
{
    const auto p = FluentTheme::palette();
    const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
    return QStringLiteral("QGroupBox { background-color: %1; border: 1px solid %2; border-radius: 8px;"
                          " margin-top: 30px; padding: 10px 8px 8px 8px; }"
                          "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;"
                          " margin: 0; left: 2px; top: 4px; color: %3; }"
                          "QPlainTextEdit, QTextEdit { background-color: %4; border: 1px solid %2;"
                          " border-radius: 4px; padding: 4px 6px; }"
                          "QToolButton { background-color: %4; border: 1px solid %2; border-radius: 4px;"
                          " padding: 4px; }"
                          "QToolButton:hover { background-color: %5; }"
                          "QToolButton:pressed, QToolButton:checked { background-color: %6; }"
                          "QAbstractSpinBox { background-color: %4; border: 1px solid %2; border-radius: 4px;"
                          " padding: 4px 6px; }"
                          "QTabWidget::pane { border: none; border-top: 1px solid %7; }")
               .arg(argb(p.layer),
                    argb(p.stroke),
                    argb(p.text),
                    argb(p.control),
                    argb(p.controlHover),
                    argb(p.controlPressed),
                    argb(p.divider))
           + pivotStyleSheet();
}

QString Vault::pivotStyleSheet()
{
    const auto p = FluentTheme::palette();
    const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
    // Tabs read as a Fluent pivot: plain labels with an accent underline on the current one
    return QStringLiteral("QTabBar::tab { background: transparent; border: none; border-bottom: 3px solid transparent;"
                          " padding: 6px 12px; margin-right: 4px; color: %1; }"
                          "QTabBar::tab:hover { color: %2; }"
                          "QTabBar::tab:selected { color: %2; border-bottom-color: %3; }")
        .arg(argb(p.textSecondary), argb(p.text), argb(p.accent));
}

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
