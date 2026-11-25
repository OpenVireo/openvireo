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

#include "VaultWidgets.h"

#include "VaultMetrics.h"
#include "gui/styles/FluentTheme.h"

#include <components/foundation/FluentElement.h>
#include <components/foundation/ThemeRegistry.h>

#include <QAction>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

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

void Vault::paintCardSurface(QWidget* card, int radius, bool border)
{
    const auto p = FluentTheme::palette();
    // Fluent-Qt's Card publishes the fill for its appearance (layer, alt or canvas) as this property
    const auto published = card->property("fluentSurfaceColor").value<QColor>();
    const QColor fill = published.isValid() ? published : p.layer;
    QPainter painter(card);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(card->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.fillPath(path, fill);
    if (border) {
        painter.setPen(QPen(p.highContrast ? p.text : p.stroke, 1));
        painter.drawPath(path);
    }
}

void Vault::makePill(fluent::basicinput::Button* button)
{
    // Fluent-Qt clamps each corner to half the button's height, so this always draws a pill (or a circle)
    button->setCornerRadii(QMargins(999, 999, 999, 999));
}

QString Vault::formStyleSheet()
{
    const auto p = FluentTheme::palette();
    const auto argb = [](const QColor& color) { return color.name(QColor::HexArgb); };
    return QStringLiteral("QGroupBox { background-color: %1; border: 1px solid %2; border-radius: 16px;"
                          " margin-top: 30px; padding: 10px 8px 8px 8px; }"
                          "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;"
                          " margin: 0; left: 2px; top: 4px; color: %3; }"
                          "QPlainTextEdit, QTextEdit { background-color: %4; border: 1px solid %2;"
                          " border-radius: 6px; padding: 4px 6px; }"
                          "QToolButton { background-color: %4; border: 1px solid %2; border-radius: 6px;"
                          " padding: 4px; }"
                          "QToolButton:hover { background-color: %5; }"
                          "QToolButton:pressed, QToolButton:checked { background-color: %6; }"
                          "QAbstractSpinBox { background-color: %4; border: 1px solid %2; border-radius: 6px;"
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

namespace
{
    void makeHeading(QFont& font)
    {
        const auto family = FluentTheme::displayFontFamily();
        if (family.isEmpty()) {
            return;
        }
        font.setFamilies({family});
        font.setWeight(QFont::Bold);
        font.setLetterSpacing(QFont::PercentageSpacing, 97.5);
    }

    bool isHeadingRole(Typography::FontRole role)
    {
        using Typography::FontRole;
        return role == FontRole::Subtitle || role == FontRole::Title || role == FontRole::TitleLarge
               || role == FontRole::Display;
    }
} // namespace

QFont Vault::font(Typography::FontRole role)
{
    auto font = fluent::ThemeRegistry::instance().resolvedFontStyle(role).toQFont();
    if (isHeadingRole(role)) {
        makeHeading(font);
    }
    return font;
}

QFont Vault::headingFont(int pixelSize)
{
    auto font = Vault::font(Typography::FontRole::Subtitle);
    font.setPixelSize(pixelSize);
    return font;
}

void Vault::styleHeading(QLabel* label, int pixelSize, const QColor& color)
{
    label->setFont(headingFont(pixelSize));
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
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
    makePill(button);
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

    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(14, 6, 8, 6);
    m_grid->setHorizontalSpacing(12);
    m_grid->setVerticalSpacing(2);
    m_grid->addWidget(m_caption, 0, 0);
    m_grid->addWidget(m_value, 0, 1);
    m_buttons->setSpacing(2);
    m_grid->addLayout(m_buttons, 0, 2);
    m_grid->setColumnStretch(1, 1);

    applyColors();
}

void VaultFieldRow::setStacked(bool stacked)
{
    if (stacked == m_stacked) {
        return;
    }
    m_stacked = stacked;
    m_grid->removeWidget(m_caption);
    m_grid->removeWidget(m_value);
    m_grid->removeItem(m_buttons);
    m_grid->setColumnStretch(0, 0);
    m_grid->setColumnStretch(1, 0);
    if (stacked) {
        // Caption on its own line; the value and its buttons share the line below
        m_caption->setMinimumWidth(0);
        m_caption->setMaximumWidth(QWIDGETSIZE_MAX);
        m_grid->addWidget(m_caption, 0, 0, 1, 2);
        m_grid->addWidget(m_value, 1, 0);
        m_grid->addLayout(m_buttons, 1, 1);
        m_grid->setColumnStretch(0, 1);
        m_grid->setContentsMargins(14, 8, 8, 6);
    } else {
        m_caption->setFixedWidth(96);
        m_grid->addWidget(m_caption, 0, 0);
        m_grid->addWidget(m_value, 0, 1);
        m_grid->addLayout(m_buttons, 0, 2);
        m_grid->setColumnStretch(1, 1);
        m_grid->setContentsMargins(14, 6, 8, 6);
    }
}

bool VaultFieldRow::isStacked() const
{
    return m_stacked;
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
    // Inside the 16px field panel: rounder than a control, tighter than the panel
    path.addRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 12, 12);
    painter.fillPath(path, FluentTheme::palette().subtleHover);
}

VaultFittedContent::VaultFittedContent(QWidget* content, QWidget* parent)
    : QWidget(parent)
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);

    // Content built while hidden hasn't laid itself out yet, so its height for a width would be stale. Lay it out
    // from the inside out, so each layout sees the sizes of the ones inside it.
    auto widgets = content->findChildren<QWidget*>();
    widgets.prepend(content);
    for (auto it = widgets.crbegin(); it != widgets.crend(); ++it) {
        if (auto widgetLayout = (*it)->layout()) {
            widgetLayout->invalidate();
            widgetLayout->activate();
        }
    }
}

QSize VaultFittedContent::sizeHint() const
{
    return {QWidget::sizeHint().width(), fittedHeight()};
}

QSize VaultFittedContent::minimumSizeHint() const
{
    return {QWidget::minimumSizeHint().width(), fittedHeight()};
}

int VaultFittedContent::fittedHeight() const
{
    const int width = this->width() > 0 ? this->width() : QWidget::sizeHint().width();
    const int height = heightForWidth(width);
    return height > 0 ? height : QWidget::sizeHint().height();
}

VaultPanel::VaultPanel(Fill fill, QWidget* parent)
    : QWidget(parent)
    , m_fill(fill)
{
    Vault::followTheme(this, [this] { update(); });
}

void VaultPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    const qreal radius = VaultMetrics::PanelRadius;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    if (m_fill == Caution) {
        painter.fillPath(path, p.cautionBg);
        if (p.highContrast) {
            painter.setPen(QPen(p.text, 1));
            painter.drawPath(path);
        }
        return;
    }
    painter.fillPath(path, m_fill == Inset ? p.inset : p.layer);
    painter.setPen(QPen(p.highContrast ? p.text : p.stroke, 1));
    painter.drawPath(path);
}

namespace
{
    constexpr int PillPaddingX = 10;
    constexpr int PillPaddingY = 2;
} // namespace

VaultPill::VaultPill(const QString& text, QWidget* parent)
    : QLabel(text, parent)
{
    setAlignment(Qt::AlignCenter);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFont(Vault::font(Typography::FontRole::Caption));
}

void VaultPill::setColors(const QColor& fill, const QColor& text)
{
    m_fill = fill;
    m_textColor = text;
    update();
}

void VaultPill::setRadius(int radius)
{
    m_radius = radius;
    update();
}

QSize VaultPill::sizeHint() const
{
    // The padding is part of the size rather than contents margins, which a style sheet's QLabel padding would reset
    const auto metrics = fontMetrics();
    return {metrics.horizontalAdvance(text()) + 2 * PillPaddingX, metrics.height() + 2 * PillPaddingY};
}

QSize VaultPill::minimumSizeHint() const
{
    return sizeHint();
}

void VaultPill::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    // Drawn here rather than by QLabel, whose colors a style sheet on a parent would override
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    const qreal radius = m_radius >= 0 ? m_radius : height() / 2.0;
    path.addRoundedRect(QRectF(rect()), radius, radius);
    painter.fillPath(path, m_fill);
    painter.setFont(font());
    painter.setPen(m_textColor);
    painter.drawText(rect(), Qt::AlignCenter, text());
}

VaultDivider::VaultDivider(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    Vault::followTheme(this, [this] { update(); });
}

void VaultDivider::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.fillRect(rect(), p.highContrast ? p.text : p.divider);
}
