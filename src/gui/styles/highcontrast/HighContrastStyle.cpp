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

#include "HighContrastStyle.h"

#include <QDialog>
#include <QFile>
#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>

HighContrastStyle::HighContrastStyle(const FluentTheme::HighContrastColors& colors)
    : BaseStyle()
    , m_colors(colors)
{
    setHighContrastOutlines(true);
#ifdef Q_OS_MACOS
    m_drawNativeMacOsToolBar = false;
#endif
}

QPalette HighContrastStyle::paletteFor(const FluentTheme::HighContrastColors& c)
{
    QPalette palette;
    // High contrast doesn't dim inactive windows: Active and Inactive share every colour
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
    // The 3D roles BaseStyle derives bevels and frames from
    palette.setColor(QPalette::All, QPalette::Light, c.window);
    palette.setColor(QPalette::All, QPalette::Midlight, c.window);
    palette.setColor(QPalette::All, QPalette::Mid, c.windowText);
    palette.setColor(QPalette::All, QPalette::Dark, c.windowText);
    palette.setColor(QPalette::All, QPalette::Shadow, c.windowText);
    return palette;
}

QPalette HighContrastStyle::standardPalette() const
{
    return paletteFor(m_colors);
}

QString HighContrastStyle::getAppStyleSheet() const
{
    QFile file(QStringLiteral(":/styles/highcontrast/highcontraststyle.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return file.readAll();
    }
    qWarning("Failed to load high contrast theme stylesheet.");
    return {};
}

void HighContrastStyle::polish(QWidget* widget)
{
    // LightStyle and DarkStyle tint top-level windows; high contrast keeps the scheme's window colour
    if (qobject_cast<QMainWindow*>(widget) || qobject_cast<QDialog*>(widget) || qobject_cast<QMenuBar*>(widget)
        || qobject_cast<QToolBar*>(widget)) {
        widget->setPalette(standardPalette());
    }
}
