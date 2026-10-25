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

#include "gui/osutils/AutoUpdater.h"

#include "config-keepassx.h"

#include <QCoreApplication>

#include <winsparkle.h>

namespace
{
    class WinSparkleUpdater : public AutoUpdater
    {
    public:
        WinSparkleUpdater()
        {
            connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { stop(); });
        }

        void checkForUpdates() override
        {
            win_sparkle_check_update_with_ui();
        }

    protected:
        void startUpdater() override
        {
            win_sparkle_set_eddsa_public_key(KEEPASSXC_UPDATE_PUBLIC_KEY);
            win_sparkle_set_app_details(
                L"OpenVireo", L"OpenVireo", QStringLiteral(KEEPASSXC_VERSION).toStdWString().c_str());
            // Feeds compare this, not the display version: nightlies carry their build number in it
            win_sparkle_set_app_build_version(QStringLiteral(KEEPASSXC_UPDATE_VERSION).toStdWString().c_str());
            // WinSparkle calls these from its own thread once the installer is downloaded and verified.
            // Quitting goes through the usual path, which saves or asks about unsaved databases.
            win_sparkle_set_can_shutdown_callback([]() -> int { return 1; });
            win_sparkle_set_shutdown_request_callback([]() {
                QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
            });
            win_sparkle_init();
            m_running = true;
        }

        void setFeedUrl(const QUrl& url) override
        {
            auto encoded = url.toEncoded();
            if (encoded == m_feedUrl) {
                return;
            }
            m_feedUrl = encoded;
            // WinSparkle reads the feed URL when it starts, so a new channel needs a restart of the updater
            bool restart = m_running;
            stop();
            win_sparkle_set_appcast_url(m_feedUrl.constData());
            if (restart) {
                startUpdater();
            }
        }

        void setAutomaticChecks(bool enabled) override
        {
            win_sparkle_set_automatic_check_for_updates(enabled ? 1 : 0);
        }

    private:
        void stop()
        {
            if (m_running) {
                win_sparkle_cleanup();
                m_running = false;
            }
        }

        QByteArray m_feedUrl;
        bool m_running = false;
    };
} // namespace

AutoUpdater* createWinSparkleUpdater()
{
    return new WinSparkleUpdater();
}
