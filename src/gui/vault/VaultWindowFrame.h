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

#ifndef KEEPASSXC_VAULTWINDOWFRAME_H
#define KEEPASSXC_VAULTWINDOWFRAME_H

#include <QObject>
#include <QPointer>

#include <memory>

namespace compatibility
{
    class WindowChromeCompat;
}

namespace fluent::windowing
{
    class TitleBar;
}

/**
 * Fluent window frame for the Vault Index layout: the header becomes the
 * window's title bar, drawn into the title bar area next to the native window
 * controls, using Fluent-Qt's window chrome.
 *
 * Only macOS keeps native window controls this way; elsewhere the window keeps
 * its native frame and the header sits below it.
 */
class VaultWindowFrame : public QObject
{
    Q_OBJECT

public:
    VaultWindowFrame(QWidget* window, QWidget* header);
    ~VaultWindowFrame() override;

    static bool isSupported();

    // The widget to place at the top of the window: the title bar, or the header itself
    QWidget* widget() const;
    bool isUnified() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyWindowFlags();
    void syncChrome();
    void scheduleSync();

    QPointer<QWidget> m_window;
    QPointer<QWidget> m_header;
    QPointer<fluent::windowing::TitleBar> m_titleBar;
    std::unique_ptr<compatibility::WindowChromeCompat> m_chrome;
    bool m_syncPending = false;
};

#endif // KEEPASSXC_VAULTWINDOWFRAME_H
