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

#include "VaultColorFlyout.h"

#include "gui/styles/FluentTheme.h"
#include "gui/vault/VaultControls.h"
#include "gui/vault/VaultMetrics.h"
#include "gui/vault/VaultWidgets.h"

#include <QAbstractButton>
#include <QColorDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QVBoxLayout>

namespace
{
    constexpr int Columns = 6;
    constexpr int SwatchSize = 40;
    constexpr int FlyoutWidth = 328;
    // Room around the card for its shadow
    constexpr int Shadow = 10;

    struct Swatch
    {
        const char* name;
        const char* hex;
    };

    // Names are for screen readers and tooltips
    const Swatch TextSwatches[] = {{QT_TRANSLATE_NOOP("VaultColorFlyout", "Red"), "#c42b1c"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Orange"), "#d86a00"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Amber"), "#b88500"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Green"), "#2f7d32"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Teal"), "#00777a"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Blue"), "#1f5bc1"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Indigo"), "#4b3fb5"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Purple"), "#7a3d8f"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Pink"), "#b8336a"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Brown"), "#7a5230"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Slate"), "#4c574f"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Black"), "#0b0f0c"}};
    const Swatch TintSwatches[] = {{QT_TRANSLATE_NOOP("VaultColorFlyout", "Red tint"), "#fde7e4"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Orange tint"), "#fdebd8"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Amber tint"), "#fbf1cf"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Green tint"), "#e7f1e1"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Teal tint"), "#dcf1f0"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Blue tint"), "#e1ebfb"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Indigo tint"), "#e7e4fa"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Purple tint"), "#f1e3f6"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Pink tint"), "#fbe3ee"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Brown tint"), "#f1e6db"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "Grey tint"), "#eef1ed"},
                                   {QT_TRANSLATE_NOOP("VaultColorFlyout", "White"), "#ffffff"}};

    // A round swatch, ringed in the accent with a check while it is the colour
    class SwatchButton : public QAbstractButton
    {
    public:
        SwatchButton(const QColor& color, const QString& name, QWidget* parent)
            : QAbstractButton(parent)
            , m_color(color)
        {
            setCheckable(true);
            setAutoExclusive(true);
            setFixedSize(SwatchSize, SwatchSize);
            setCursor(Qt::PointingHandCursor);
            setFocusPolicy(Qt::StrongFocus);
            setToolTip(name);
            setAccessibleName(name);
            setObjectName(QStringLiteral("colorSwatch-%1").arg(color.name().mid(1)));
        }

        QColor color() const
        {
            return m_color;
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            const auto p = FluentTheme::palette();
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const QRectF outer = QRectF(rect()).adjusted(1, 1, -1, -1);
            if (isChecked() || hasFocus()) {
                painter.setPen(QPen(isChecked() ? p.accent : p.focus, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(outer);
            }
            const QRectF dot = outer.adjusted(4, 4, -4, -4);
            painter.setPen(QPen(p.stroke, 1));
            painter.setBrush(m_color);
            painter.drawEllipse(dot);
            if (isChecked()) {
                // White on dark colours, near-black on light tints
                const bool light = m_color.lightnessF() > 0.6;
                QFont glyph(Typography::FontFamily::FluentIcons);
                glyph.setPixelSize(14);
                painter.setFont(glyph);
                painter.setPen(light ? QColor("#0b0f0c") : QColor(Qt::white));
                painter.drawText(dot, Qt::AlignCenter, Typography::Icons::CheckMark);
            }
        }

    private:
        QColor m_color;
    };
} // namespace

QList<QColor> VaultColorFlyout::swatchColors(Swatches swatches)
{
    QList<QColor> colors;
    if (swatches == TextColors) {
        for (const auto& swatch : TextSwatches) {
            colors << QColor(swatch.hex);
        }
    } else {
        for (const auto& swatch : TintSwatches) {
            colors << QColor(swatch.hex);
        }
    }
    return colors;
}

VaultColorFlyout* VaultColorFlyout::open(QWidget* anchor,
                                         const QColor& current,
                                         Swatches swatches,
                                         const QString& title,
                                         std::function<void(const QColor&)> picked)
{
    auto* flyout = new VaultColorFlyout(anchor, current, swatches, title, std::move(picked));
    flyout->place();
    flyout->show();
    flyout->raise();
    flyout->activateWindow();
    for (auto* swatch : flyout->m_swatches) {
        if (swatch->isChecked()) {
            swatch->setFocus(Qt::PopupFocusReason);
            return flyout;
        }
    }
    if (!flyout->m_swatches.isEmpty()) {
        flyout->m_swatches.first()->setFocus(Qt::PopupFocusReason);
    }
    return flyout;
}

VaultColorFlyout::VaultColorFlyout(QWidget* anchor,
                                   const QColor& current,
                                   Swatches swatches,
                                   const QString& title,
                                   std::function<void(const QColor&)> picked)
    : QWidget(anchor ? anchor->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_anchor(anchor)
    , m_color(current)
    , m_picked(std::move(picked))
    , m_title(new QLabel(title, this))
    , m_hexLabel(new QLabel(tr("Hex"), this))
    , m_hex(new VaultLineEdit(this))
{
    setObjectName("vaultColorFlyout");
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setAccessibleName(title);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Shadow + 16, Shadow + 16, Shadow + 16, Shadow + 16);
    layout->setSpacing(14);
    layout->addWidget(m_title);

    auto* grid = new QGridLayout();
    grid->setSpacing(8);
    const auto colors = swatchColors(swatches);
    const auto* names = swatches == TextColors ? TextSwatches : TintSwatches;
    for (int i = 0; i < colors.size(); ++i) {
        auto* swatch = new SwatchButton(colors[i], tr(names[i].name), this);
        swatch->setChecked(current.isValid() && current.name() == colors[i].name());
        connect(swatch, &QAbstractButton::clicked, this, [this, swatch] { pick(swatch->color()); });
        grid->addWidget(swatch, i / Columns, i % Columns, Qt::AlignCenter);
        m_swatches << swatch;
    }
    layout->addLayout(grid);

    // Any colour by its hex code, or the system's own dialog
    auto* hexRow = new QHBoxLayout();
    hexRow->setSpacing(8);
    m_hex->setObjectName("colorHex");
    m_hex->setAccessibleName(tr("Hex color"));
    m_hex->setPlaceholderText(QStringLiteral("#RRGGBB"));
    m_hex->setText(current.isValid() ? current.name().toUpper() : QString());
    m_hexLabel->setBuddy(m_hex);
    auto* more = new VaultButton(tr("More…"), this);
    more->setObjectName("colorMore");
    hexRow->addWidget(m_hexLabel);
    hexRow->addWidget(m_hex, 1);
    hexRow->addWidget(more);
    layout->addLayout(hexRow);
    connect(m_hex, &QLineEdit::textEdited, this, [this](const QString& text) {
        static const QRegularExpression hex(QStringLiteral("^#?[0-9a-fA-F]{6}$"));
        if (hex.match(text.trimmed()).hasMatch()) {
            auto code = text.trimmed();
            pick(QColor(code.startsWith('#') ? code : QLatin1Char('#') + code));
        }
    });
    connect(more, &QAbstractButton::clicked, this, [this] {
        // The popup closes first, as a modal dialog can't open over it
        QPointer<QWidget> anchor = m_anchor;
        const auto start = m_color;
        auto picked = m_picked;
        close();
        const auto color = QColorDialog::getColor(start, anchor ? anchor->window() : nullptr);
        if (color.isValid() && picked) {
            picked(color);
        }
    });

    auto* divider = new VaultDivider(this);
    layout->addWidget(divider);
    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    auto* reset = new VaultButton(tr("Use default"), this);
    reset->setObjectName("colorDefault");
    reset->setFluentStyle(fluent::basicinput::Button::Subtle);
    auto* done = new VaultButton(tr("Done"), this);
    done->setObjectName("colorDone");
    done->setFluentStyle(fluent::basicinput::Button::Accent);
    footer->addWidget(reset);
    footer->addStretch();
    footer->addWidget(done);
    layout->addLayout(footer);
    connect(reset, &QAbstractButton::clicked, this, [this] {
        pick(QColor());
        close();
    });
    connect(done, &QAbstractButton::clicked, this, &QWidget::close);

    Vault::followTheme(this, [this] { applyColors(); });
    setFixedWidth(FlyoutWidth + 2 * Shadow);
    adjustSize();
}

QColor VaultColorFlyout::color() const
{
    return m_color;
}

void VaultColorFlyout::pick(const QColor& color)
{
    m_color = color;
    for (auto* button : m_swatches) {
        auto* swatch = static_cast<SwatchButton*>(button);
        const bool match = color.isValid() && swatch->color().name() == color.name();
        if (swatch->isChecked() != match) {
            // Unchecking an exclusive button needs the exclusivity off for a moment
            swatch->setAutoExclusive(false);
            swatch->setChecked(match);
            swatch->setAutoExclusive(true);
        }
    }
    if (!m_hex->hasFocus()) {
        m_hex->setText(color.isValid() ? color.name().toUpper() : QString());
    }
    if (m_picked) {
        m_picked(color);
    }
}

void VaultColorFlyout::place()
{
    if (!m_anchor) {
        return;
    }
    // Under the anchor, left edges lined up, kept on the anchor's screen
    QPoint at = m_anchor->mapToGlobal(QPoint(0, m_anchor->height() + 6)) - QPoint(Shadow, Shadow);
    const auto screen = m_anchor->screen() ? m_anchor->screen()->availableGeometry() : QRect();
    if (screen.isValid()) {
        if (at.x() + width() > screen.right()) {
            at.setX(screen.right() - width());
        }
        if (at.y() + height() > screen.bottom()) {
            at.setY(m_anchor->mapToGlobal(QPoint(0, 0)).y() - height() - 6 + Shadow);
        }
        at.setX(qMax(at.x(), screen.left()));
    }
    move(at);
}

void VaultColorFlyout::applyColors()
{
    const auto p = FluentTheme::palette();
    Vault::styleLabel(m_title, Typography::FontRole::BodyStrong, p.text);
    Vault::styleLabel(m_hexLabel, Typography::FontRole::Body, p.textSecondary);
    QFont mono(FluentTheme::monoFontFamily());
    mono.setPixelSize(14);
    mono.setWeight(QFont::Medium);
    m_hex->setFont(mono);
    update();
}

void VaultColorFlyout::paintEvent(QPaintEvent*)
{
    const auto p = FluentTheme::palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF card = QRectF(rect()).adjusted(Shadow, Shadow, -Shadow, -Shadow);
    const qreal radius = VaultMetrics::CardRadius;
    // A soft shadow, then the card on the layer fill with a hairline
    painter.setPen(Qt::NoPen);
    for (int i = Shadow; i > 0; --i) {
        painter.setBrush(QColor(0, 0, 0, 4));
        painter.drawRoundedRect(card.adjusted(-i, -i + 3, i, i + 3), radius + i, radius + i);
    }
    QPainterPath path;
    path.addRoundedRect(card, radius, radius);
    painter.fillPath(path, p.layer);
    painter.setPen(QPen(p.highContrast ? p.text : p.stroke, 1));
    painter.drawPath(path);
}

void VaultColorFlyout::keyPressEvent(QKeyEvent* event)
{
    // Enter closes like Done; Escape closes as a popup does
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}
