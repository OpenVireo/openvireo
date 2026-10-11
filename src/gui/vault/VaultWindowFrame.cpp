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

#include "VaultWindowFrame.h"

#include "gui/styles/FluentTheme.h"

#include <compatibility/WindowChromeCompat.h>
#include <components/windowing/TitleBar.h>

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QPainter>
#include <QTimer>

namespace
{
    constexpr int TitleBarHeight = 48;

    // Title bar painted with the Vault canvas so it joins the header seamlessly
    class VaultTitleBar : public fluent::windowing::TitleBar
    {
    public:
        using TitleBar::TitleBar;

    protected:
        void paintEvent(QPaintEvent* event) override
        {
            Q_UNUSED(event)
            QPainter painter(this);
            painter.fillRect(rect(), FluentTheme::palette().canvas);
        }
    };
} // namespace

VaultWindowFrame::VaultWindowFrame(QWidget* window, QWidget* header)
    : QObject(window)
    , m_window(window)
    , m_header(header)
{
    if (!isSupported()) {
        return;
    }

    m_chrome = std::make_unique<compatibility::WindowChromeCompat>(window);

    m_titleBar = new VaultTitleBar(window);
    m_titleBar->setObjectName("vaultTitleBar");
    m_titleBar->setTitleBarHeight(TitleBarHeight);
    m_titleBar->setContentWidget(header);
    // The title bar already reserves the space for the window controls
    if (header->layout()) {
        header->layout()->setContentsMargins(4, 3, 12, 3);
    }

    connect(m_titleBar, &fluent::windowing::TitleBar::dragStarted, this, [this](const QPoint& globalPos) {
        if (m_window && !m_window->isFullScreen()) {
            m_chrome->beginSystemMove(globalPos);
        }
    });
    connect(m_titleBar, &fluent::windowing::TitleBar::doubleClicked, this, [this] {
        m_chrome->performTitleBarDoubleClick();
    });
    connect(m_titleBar, &fluent::windowing::TitleBar::chromeGeometryChanged, this, &VaultWindowFrame::scheduleSync);

    // Window flags must be in place before the native window is shown
    applyWindowFlags();
    window->installEventFilter(this);
}

VaultWindowFrame::~VaultWindowFrame() = default;

bool VaultWindowFrame::isSupported()
{
    return compatibility::WindowChromeCompat::currentPlatform() == compatibility::WindowChromeCompat::Platform::MacOS
           && QGuiApplication::platformName() == QStringLiteral("cocoa");
}

QWidget* VaultWindowFrame::widget() const
{
    if (m_titleBar) {
        return m_titleBar;
    }
    return m_header;
}

bool VaultWindowFrame::isUnified() const
{
    return m_titleBar;
}

bool VaultWindowFrame::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::WinIdChange:
            // Recreating the native window (e.g. toggling Always on Top) resets its title bar style
            QTimer::singleShot(0, this, [this] {
                applyWindowFlags();
                syncChrome();
            });
            break;
        case QEvent::Resize:
        case QEvent::WindowStateChange:
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
        case QEvent::WindowTitleChange:
        case QEvent::ModifiedChange:
        case QEvent::ThemeChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
        case QEvent::DevicePixelRatioChange:
            // macOS lays the window controls out again after these, so align them afterwards
            scheduleSync();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void VaultWindowFrame::applyWindowFlags()
{
    if (m_chrome) {
        m_chrome->applyPlatformWindowFlags();
    }
}

void VaultWindowFrame::syncChrome()
{
    if (!m_chrome || !m_window || !m_titleBar) {
        return;
    }

    // Full screen hides the window controls, so the header can start at the edge
    m_titleBar->setSystemReservedLeadingWidth(m_window->isFullScreen() ? 0 : m_chrome->nativeTitleBarLeadingInset());

    auto options = m_chrome->options();
    options.titleBarRect = QRect(m_titleBar->mapTo(m_window, QPoint(0, 0)), m_titleBar->size());
    m_chrome->configure(options);
}

void VaultWindowFrame::scheduleSync()
{
    if (m_syncPending) {
        return;
    }
    m_syncPending = true;
    QTimer::singleShot(0, this, [this] {
        m_syncPending = false;
        syncChrome();
    });
}
