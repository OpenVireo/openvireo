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

#include "FluentTheme.h"

#include "core/Config.h"
#include "gui/ActionCollection.h"

#include <FluentQt/FluentQt.h>

namespace
{
    // Accent taken from the green key in the KeePassXC logo, tuned per mode for contrast
    const QColor LightAccent(0x3d, 0x7a, 0x28);
    const QColor LightAccentText(0x2f, 0x64, 0x20);
    const QColor DarkAccent(0x8f, 0xcd, 0x6e);
    const QColor DarkAccentText(0xa8, 0xdc, 0x8d);
    const QColor DarkOnAccent(0x0f, 0x1a, 0x0b);

    bool g_initialized = false;

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

        g_initialized = true;
    }
} // namespace

bool FluentTheme::vaultLayoutEnabled()
{
    // Read once: switching layouts takes effect after a restart
    static const bool enabled = config()->get(Config::GUI_VaultLayout).toBool();
    return enabled;
}

void FluentTheme::apply(bool dark)
{
    if (!g_initialized) {
        initialize();
    }
    fluent::FluentElement::setTheme(dark ? fluent::FluentElement::Dark : fluent::FluentElement::Light);
}

bool FluentTheme::isDark()
{
    return fluent::FluentElement::themeUsesDarkAppearance(fluent::FluentElement::currentTheme());
}

FluentTheme::Palette FluentTheme::palette()
{
    const auto theme = fluent::FluentElement::currentTheme();
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
    p.accent = c.accentDefault;
    p.accentText = c.textAccentPrimary;
    // Selected rows get a quiet accent tint rather than a solid fill
    p.accentSubtle = withAlpha(c.accentDefault, dark ? 46 : 36);
    p.critical = c.systemCritical;
    p.criticalBg = c.systemCriticalBg;
    p.caution = c.systemCaution;
    p.cautionBg = c.systemCautionBg;
    p.success = c.systemSuccess;
    p.successBg = c.systemSuccessBg;
    return p;
}

QAction* FluentTheme::action(const QString& objectName)
{
    for (auto* action : ActionCollection::instance()->actions()) {
        if (action->objectName() == objectName) {
            return action;
        }
    }
    return nullptr;
}
