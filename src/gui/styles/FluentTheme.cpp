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

#include "FluentTheme.h"

#include "gui/ActionCollection.h"
#include "gui/styles/VaultStyle.h"

#include <FluentQt/FluentQt.h>

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace
{
    // Accent taken from the green key in the KeePassXC logo, tuned per mode for contrast
    const QColor LightAccent(0x3d, 0x7a, 0x28);
    const QColor LightAccentText(0x2f, 0x64, 0x20);
    const QColor DarkAccent(0x8f, 0xcd, 0x6e);
    const QColor DarkAccentText(0xa8, 0xdc, 0x8d);
    const QColor DarkOnAccent(0x0f, 0x1a, 0x0b);

    bool g_initialized = false;
    bool g_highContrast = false;
    FluentTheme::HighContrastColors g_highContrastColors;
    // The light palette with the KeePassXC accent; the light high-contrast scheme replaces it while active
    fluent::FluentElement::Colors g_lightColors;

    QColor withAlpha(QColor color, int alpha)
    {
        color.setAlpha(alpha);
        return color;
    }

    QColor blend(const QColor& from, const QColor& to, qreal amount)
    {
        return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                                from.greenF() + (to.greenF() - from.greenF()) * amount,
                                from.blueF() + (to.blueF() - from.blueF()) * amount);
    }

    void setAccent(fluent::FluentElement::Colors& colors,
                   const QColor& accent,
                   const QColor& accentText,
                   const QColor& onAccent)
    {
        colors.accentDefault = accent;
        colors.accentSecondary = withAlpha(accent, 230);
        colors.accentTertiary = withAlpha(accent, 204);
        colors.textAccentPrimary = accentText;
        colors.textOnAccent = onAccent;
    }

    void initialize()
    {
        fluent::initializeResources();

        auto& registry = fluent::ThemeRegistry::instance();
        auto snapshot = registry.snapshot();
        setAccent(snapshot.lightColors, LightAccent, LightAccentText, Qt::white);
        setAccent(snapshot.darkColors, DarkAccent, DarkAccentText, DarkOnAccent);
        registry.applySnapshot(snapshot);
        g_lightColors = snapshot.lightColors;

        g_initialized = true;
    }

    qreal relativeLuminance(const QColor& color)
    {
        auto channel = [](qreal c) { return c <= 0.03928 ? c / 12.92 : qPow((c + 0.055) / 1.055, 2.4); };
        const auto rgb = color.toRgb();
        return 0.2126 * channel(rgb.redF()) + 0.7152 * channel(rgb.greenF()) + 0.0722 * channel(rgb.blueF());
    }

    // The first candidate that reaches the ratio against the background, else the best one
    QColor firstLegible(const QColor& background, std::initializer_list<QColor> candidates, qreal ratio)
    {
        QColor best;
        qreal bestRatio = 0;
        for (const auto& candidate : candidates) {
            const auto r = FluentTheme::contrastRatio(candidate, background);
            if (r >= ratio) {
                return candidate;
            }
            if (r > bestRatio) {
                best = candidate;
                bestRatio = r;
            }
        }
        return best;
    }

    // Fluent-Qt tokens for a high-contrast scheme. Every colour is opaque, every stroke is the text colour.
    fluent::FluentElement::Colors highContrastTokens(const FluentTheme::HighContrastColors& hc,
                                                     fluent::FluentElement::Colors c)
    {
        const bool dark = hc.isDark();
        // Hover and press fills stay solid; text on them keeps well above 7:1
        const QColor hover = blend(hc.buttonFace, hc.buttonText, 0.12);
        const QColor pressed = blend(hc.buttonFace, hc.buttonText, 0.24);
        const QColor focus = firstLegible(hc.window, {hc.highlight, hc.windowText}, 3.0);

        c.accentDefault = hc.highlight;
        c.accentSecondary = blend(hc.highlight, hc.highlightText, 0.1);
        c.accentTertiary = blend(hc.highlight, hc.highlightText, 0.2);
        c.accentDisabled = hc.grayText;
        c.controlDefault = hc.buttonFace;
        c.controlSecondary = hover;
        c.controlTertiary = pressed;
        c.controlDisabled = hc.window;
        c.controlAltSecondary = hover;
        c.controlAltTertiary = pressed;
        c.subtleTransparent = QColor(0, 0, 0, 0);
        c.subtleSecondary = hover;
        c.subtleTertiary = pressed;

        c.strokeDefault = hc.buttonText;
        c.strokeSecondary = hc.buttonText;
        c.strokeStrong = hc.windowText;
        c.strokeCard = hc.windowText;
        c.strokeDivider = hc.windowText;
        c.strokeSurface = hc.windowText;
        c.strokeFocusOuter = focus;
        c.strokeFocusInner = hc.window;

        c.textPrimary = hc.windowText;
        c.textSecondary = hc.windowText;
        c.textTertiary = hc.windowText;
        c.textDisabled = hc.grayText;
        c.textOnAccent = hc.highlightText;
        c.textAccentPrimary = hc.hotlight;

        c.bgCanvas = hc.window;
        c.bgLayer = hc.window;
        c.bgLayerAlt = hc.window;
        c.bgSolid = hc.window;
        c.bgLayerOverlay = hc.window;
        // Fluent-Qt's greys run from the lightest (10) to the darkest (190)
        const QColor lightGrey = dark ? hc.windowText : hc.window;
        const QColor darkGrey = dark ? hc.window : hc.windowText;
        c.grey10 = c.grey20 = c.grey30 = c.grey40 = c.grey50 = c.grey60 = lightGrey;
        c.grey90 = c.grey130 = c.grey160 = c.grey190 = darkGrey;

        c.systemCritical = hc.critical;
        c.systemCriticalBg = hc.window;
        c.systemCaution = hc.caution;
        c.systemCautionBg = hc.window;
        c.systemInfo = hc.windowText;
        c.systemInfoBg = hc.window;
        c.systemSuccess = hc.success;
        c.systemSuccessBg = hc.window;
        return c;
    }

    // An opaque colour that looks like the translucent one drawn over the background
    QColor solid(const QColor& color, const QColor& background)
    {
        const auto a = color.alphaF();
        return QColor::fromRgbF(color.redF() * a + background.redF() * (1 - a),
                                color.greenF() * a + background.greenF() * (1 - a),
                                color.blueF() * a + background.blueF() * (1 - a));
    }

    FluentTheme::Palette paletteFor(fluent::FluentElement::Theme theme);
    QPalette qtPaletteFor(fluent::FluentElement::Theme theme);

    // Switch Fluent-Qt's theme, and repaint even when neither the theme nor its tokens changed: the caller has just
    // installed a new QStyle, and re-polishing restores the palettes widgets had when they were first polished, so
    // they must restyle their labels again in onThemeUpdated()
    void setTheme(fluent::FluentElement::Theme theme, bool tokensChanged)
    {
        // Stock widgets first: the palette, then a fresh style so every widget is polished again with it, style
        // sheets included. Fluent widgets restyling themselves below then see the new application palette.
        if (qobject_cast<QApplication*>(QCoreApplication::instance())) {
            QApplication::setPalette(qtPaletteFor(theme));
            QApplication::setStyle(new VaultStyle);
        }
        if (fluent::FluentElement::currentTheme() != theme) {
            fluent::FluentElement::setTheme(theme);
        } else if (!tokensChanged) {
            fluent::FluentElement::refreshTheme();
        }
    }
} // namespace

void FluentTheme::apply(bool dark)
{
    if (!g_initialized) {
        initialize();
    }
    g_highContrast = false;
    auto& registry = fluent::ThemeRegistry::instance();
    auto snapshot = registry.extendedSnapshot();
    snapshot.base.lightColors = g_lightColors;
    const bool changed = registry.applyExtendedSnapshot(snapshot);
    fluent::MotionPolicy::instance().setMode(fluent::MotionPolicy::Mode::Full);
    setTheme(dark ? fluent::FluentElement::Dark : fluent::FluentElement::Light, changed);
}

bool FluentTheme::HighContrastColors::isDark() const
{
    return relativeLuminance(window) < relativeLuminance(windowText);
}

FluentTheme::HighContrastColors FluentTheme::highContrastColors(bool dark)
{
    HighContrastColors c;
    if (dark) {
        // Windows' "High Contrast Black" / "Night sky" colours
        c.window = QColor(0x00, 0x00, 0x00);
        c.windowText = QColor(0xff, 0xff, 0xff);
        c.highlight = QColor(0x1a, 0xeb, 0xff);
        c.highlightText = QColor(0x00, 0x00, 0x00);
        c.buttonFace = QColor(0x00, 0x00, 0x00);
        c.buttonText = QColor(0xff, 0xff, 0xff);
        c.grayText = QColor(0xa6, 0xa6, 0xa6);
        c.hotlight = QColor(0xff, 0xff, 0x00);
        c.critical = QColor(0xff, 0x99, 0xa4);
        c.caution = QColor(0xff, 0xff, 0x00);
        c.success = QColor(0x3f, 0xf2, 0x3f);
    } else {
        // Windows' "High Contrast White" colours, with a neutral grey for disabled text
        c.window = QColor(0xff, 0xff, 0xff);
        c.windowText = QColor(0x00, 0x00, 0x00);
        c.highlight = QColor(0x37, 0x00, 0x6e);
        c.highlightText = QColor(0xff, 0xff, 0xff);
        c.buttonFace = QColor(0xff, 0xff, 0xff);
        c.buttonText = QColor(0x00, 0x00, 0x00);
        c.grayText = QColor(0x57, 0x57, 0x57);
        c.hotlight = QColor(0x00, 0x00, 0x9f);
        c.critical = QColor(0xa3, 0x00, 0x0f);
        c.caution = QColor(0x6b, 0x4e, 0x00);
        c.success = QColor(0x0a, 0x5c, 0x0a);
    }
    return c;
}

void FluentTheme::fillStatusColors(HighContrastColors& colors)
{
    const auto fixed = highContrastColors(colors.isDark());
    colors.critical = firstLegible(colors.window, {fixed.critical, colors.windowText}, 7.0);
    colors.caution = firstLegible(colors.window, {fixed.caution, colors.windowText}, 7.0);
    colors.success = firstLegible(colors.window, {fixed.success, colors.windowText}, 7.0);
}

void FluentTheme::applyHighContrast(const HighContrastColors& colors)
{
    if (!g_initialized) {
        initialize();
    }
    g_highContrast = true;
    g_highContrastColors = colors;

    // Fluent-Qt's HighContrast theme always uses dark chrome, so a dark-on-light scheme replaces the light palette
    const bool dark = colors.isDark();
    auto& registry = fluent::ThemeRegistry::instance();
    auto snapshot = registry.extendedSnapshot();
    const auto defaults = fluent::ThemeRegistry::defaultExtendedSnapshot();
    if (dark) {
        snapshot.base.lightColors = g_lightColors;
        snapshot.contrastColors = highContrastTokens(colors, defaults.contrastColors);
    } else {
        snapshot.base.lightColors = highContrastTokens(colors, defaults.base.lightColors);
    }
    const bool changed = registry.applyExtendedSnapshot(snapshot);
    fluent::MotionPolicy::instance().setMode(fluent::MotionPolicy::Mode::Disabled);
    setTheme(dark ? fluent::FluentElement::HighContrast : fluent::FluentElement::Light, changed);
}

bool FluentTheme::isHighContrast()
{
    return g_highContrast;
}

FluentTheme::HighContrastColors FluentTheme::currentHighContrastColors()
{
    return g_highContrast ? g_highContrastColors : highContrastColors(true);
}

qreal FluentTheme::contrastRatio(const QColor& a, const QColor& b)
{
    const auto la = relativeLuminance(a);
    const auto lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

void FluentTheme::paintRowStateOutline(QPainter* painter, const QPainterPath& row, bool selected, bool hovered)
{
    if (!g_highContrast || (!selected && !hovered)) {
        return;
    }
    const auto p = palette();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::NoBrush);
    if (selected) {
        // Keep the 2px stroke inside the row
        QPen pen(p.selectionStroke, 2);
        painter->setPen(pen);
        painter->drawPath(row);
    } else {
        painter->setPen(QPen(p.text, 1));
        painter->drawPath(row);
    }
    painter->restore();
}

bool FluentTheme::isDark()
{
    return fluent::FluentElement::themeUsesDarkAppearance(fluent::FluentElement::currentTheme());
}

FluentTheme::Palette FluentTheme::palette()
{
    return paletteFor(fluent::FluentElement::currentTheme());
}

QPalette FluentTheme::qtPalette()
{
    return qtPaletteFor(fluent::FluentElement::currentTheme());
}

namespace
{
    FluentTheme::Palette paletteFor(fluent::FluentElement::Theme theme)
    {
        using FluentTheme::contrastRatio;
        using FluentTheme::Palette;
        const auto& c = fluent::ThemeRegistry::instance().colors(theme);
        const bool dark = fluent::FluentElement::themeUsesDarkAppearance(theme);

        Palette p;
        p.canvas = c.bgCanvas;
        p.surface = blend(c.bgCanvas, c.bgLayer, 0.5);
        p.layer = c.bgLayer;
        p.inset = blend(c.bgLayer, c.bgCanvas, 0.35);
        p.stroke = c.strokeCard;
        p.strokeStrong = c.strokeStrong;
        p.divider = c.strokeDivider;
        p.control = c.controlDefault;
        p.controlHover = c.controlSecondary;
        p.controlPressed = c.controlTertiary;
        p.subtleHover = c.subtleSecondary;
        p.subtlePressed = c.subtleTertiary;
        p.text = c.textPrimary;
        p.textSecondary = c.textSecondary;
        p.textTertiary = c.textTertiary;
        p.textDisabled = c.textDisabled;
        p.accent = c.accentDefault;
        p.accentText = c.textAccentPrimary;
        // Selected rows get a quiet accent tint rather than a solid fill
        p.accentSubtle = withAlpha(c.accentDefault, dark ? 46 : 36);
        p.onAccent = c.textOnAccent;
        p.critical = c.systemCritical;
        p.criticalBg = c.systemCriticalBg;
        p.caution = c.systemCaution;
        p.cautionBg = c.systemCautionBg;
        p.success = c.systemSuccess;
        p.successBg = c.systemSuccessBg;
        p.infoBg = c.systemInfoBg;
        p.link = c.textAccentPrimary;
        p.focus = c.strokeFocusOuter;
        p.selectionStroke = c.accentDefault;
        if (g_highContrast) {
            const auto& hc = g_highContrastColors;
            p.highContrast = true;
            p.surface = hc.window;
            p.inset = hc.window;
            // A solid tint instead of a translucent one; fall back to no tint if the text would lose contrast
            const auto tint = blend(hc.window, hc.highlight, 0.2);
            p.accentSubtle = contrastRatio(tint, hc.windowText) >= 7.0 ? tint : hc.window;
            p.selectionStroke = firstLegible(hc.window, {hc.highlight, hc.windowText}, 3.0);
        }
        return p;
    }

    QPalette qtPaletteFor(fluent::FluentElement::Theme theme)
    {
        QPalette palette;
        if (g_highContrast) {
            const auto& c = g_highContrastColors;
            // High contrast doesn't dim inactive windows
            for (auto group : {QPalette::Active, QPalette::Inactive}) {
                palette.setColor(group, QPalette::Window, c.window);
                palette.setColor(group, QPalette::WindowText, c.windowText);
                palette.setColor(group, QPalette::Base, c.window);
                palette.setColor(group, QPalette::AlternateBase, c.window);
                palette.setColor(group, QPalette::Text, c.windowText);
                palette.setColor(group, QPalette::PlaceholderText, c.grayText);
                palette.setColor(group, QPalette::Button, c.buttonFace);
                palette.setColor(group, QPalette::ButtonText, c.buttonText);
                palette.setColor(group, QPalette::BrightText, c.highlightText);
                palette.setColor(group, QPalette::Highlight, c.highlight);
                palette.setColor(group, QPalette::HighlightedText, c.highlightText);
                palette.setColor(group, QPalette::Link, c.hotlight);
                palette.setColor(group, QPalette::LinkVisited, c.hotlight);
            }
            palette.setColor(QPalette::Disabled, QPalette::Window, c.window);
            palette.setColor(QPalette::Disabled, QPalette::Base, c.window);
            palette.setColor(QPalette::Disabled, QPalette::AlternateBase, c.window);
            palette.setColor(QPalette::Disabled, QPalette::Button, c.window);
            palette.setColor(QPalette::Disabled, QPalette::WindowText, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::Text, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::BrightText, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::Highlight, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::HighlightedText, c.window);
            palette.setColor(QPalette::Disabled, QPalette::Link, c.grayText);
            palette.setColor(QPalette::Disabled, QPalette::LinkVisited, c.grayText);

            palette.setColor(QPalette::All, QPalette::ToolTipBase, c.window);
            palette.setColor(QPalette::All, QPalette::ToolTipText, c.windowText);
            // The 3D roles Fusion derives bevels and frames from: no shading, text-coloured edges
            palette.setColor(QPalette::All, QPalette::Light, c.window);
            palette.setColor(QPalette::All, QPalette::Midlight, c.window);
            palette.setColor(QPalette::All, QPalette::Mid, c.windowText);
            palette.setColor(QPalette::All, QPalette::Dark, c.windowText);
            palette.setColor(QPalette::All, QPalette::Shadow, c.windowText);
            return palette;
        }

        const auto p = paletteFor(theme);
        const bool dark = fluent::FluentElement::themeUsesDarkAppearance(theme);
        const auto window = solid(p.surface, p.canvas);
        const auto base = solid(p.layer, p.canvas);
        const auto button = solid(p.control, window);
        const auto text = solid(p.text, window);
        const auto disabledText = solid(p.textDisabled, window);
        const auto stroke = solid(p.strokeStrong, window);
        // Fluent doesn't dim inactive windows either
        for (auto group : {QPalette::Active, QPalette::Inactive}) {
            palette.setColor(group, QPalette::Window, window);
            palette.setColor(group, QPalette::WindowText, text);
            palette.setColor(group, QPalette::Base, base);
            palette.setColor(group, QPalette::AlternateBase, solid(p.inset, window));
            palette.setColor(group, QPalette::Text, text);
            palette.setColor(group, QPalette::PlaceholderText, solid(p.textSecondary, base));
            palette.setColor(group, QPalette::Button, button);
            palette.setColor(group, QPalette::ButtonText, text);
            palette.setColor(group, QPalette::BrightText, p.onAccent);
            palette.setColor(group, QPalette::Highlight, p.accent);
            palette.setColor(group, QPalette::HighlightedText, p.onAccent);
            palette.setColor(group, QPalette::Link, p.link);
            palette.setColor(group, QPalette::LinkVisited, p.link);
        }
        palette.setColor(QPalette::Disabled, QPalette::Window, window);
        palette.setColor(QPalette::Disabled, QPalette::Base, window);
        palette.setColor(QPalette::Disabled, QPalette::AlternateBase, window);
        palette.setColor(QPalette::Disabled, QPalette::Button, window);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::BrightText, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::Highlight, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, window);
        palette.setColor(QPalette::Disabled, QPalette::Link, disabledText);
        palette.setColor(QPalette::Disabled, QPalette::LinkVisited, disabledText);

        // Fluent tooltips: a solid layer with the text colour
        palette.setColor(
            QPalette::All, QPalette::ToolTipBase, dark ? QColor(0x2c, 0x2c, 0x2c) : QColor(0xf9, 0xf9, 0xf9));
        palette.setColor(QPalette::All, QPalette::ToolTipText, text);
        palette.setColor(QPalette::All, QPalette::Light, dark ? button.lighter(130) : QColor(Qt::white));
        palette.setColor(QPalette::All, QPalette::Midlight, button);
        palette.setColor(QPalette::All, QPalette::Mid, stroke);
        palette.setColor(QPalette::All, QPalette::Dark, stroke.darker(130));
        palette.setColor(QPalette::All, QPalette::Shadow, dark ? QColor(Qt::black) : stroke.darker(160));
        return palette;
    }
} // namespace

QAction* FluentTheme::action(const QString& objectName)
{
    for (auto* action : ActionCollection::instance()->actions()) {
        if (action->objectName() == objectName) {
            return action;
        }
    }
    return nullptr;
}
