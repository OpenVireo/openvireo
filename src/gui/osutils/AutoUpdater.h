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

#ifndef KEEPASSXC_AUTOUPDATER_H
#define KEEPASSXC_AUTOUPDATER_H

#include <QObject>
#include <QUrl>

/**
 * Downloads, verifies and installs updates through the platform's updater: Sparkle on macOS and
 * WinSparkle on Windows. Both check the EdDSA signature of each package in the feed against
 * KEEPASSXC_UPDATE_PUBLIC_KEY before installing it, and show their own update dialogs.
 */
class AutoUpdater : public QObject
{
    Q_OBJECT
public:
    // The updater of this platform, or nullptr where the update checker only reports new versions
    static AutoUpdater* instance();

    // Starts the updater with the current settings; call once the user has decided about update checks
    void start();
    // Applies changed update settings (scheduled checks and channel)
    void applySettings();
    // Checks now and shows the result, as from the "Check for Updates" menu item
    virtual void checkForUpdates() = 0;

protected:
    explicit AutoUpdater(QObject* parent = nullptr);

    virtual void startUpdater() = 0;
    virtual void setFeedUrl(const QUrl& url) = 0;
    virtual void setAutomaticChecks(bool enabled) = 0;

private:
    bool m_started = false;
};

#endif // KEEPASSXC_AUTOUPDATER_H
