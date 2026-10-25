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

#include "AutoUpdater.h"

#include "config-keepassx.h"
#include "core/Config.h"
#include "networking/UpdateFeed.h"

#if defined(KPXC_SPARKLE)
AutoUpdater* createSparkleUpdater();
#elif defined(KPXC_WINSPARKLE)
AutoUpdater* createWinSparkleUpdater();
#endif

AutoUpdater::AutoUpdater(QObject* parent)
    : QObject(parent)
{
}

AutoUpdater* AutoUpdater::instance()
{
#if defined(KPXC_SPARKLE)
    static AutoUpdater* updater = UpdateFeed::isEnabled() ? createSparkleUpdater() : nullptr;
#elif defined(KPXC_WINSPARKLE)
    static AutoUpdater* updater = UpdateFeed::isEnabled() ? createWinSparkleUpdater() : nullptr;
#else
    static AutoUpdater* updater = nullptr;
#endif
    return updater;
}

void AutoUpdater::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    // The updater must know its feed and schedule before it starts, or it checks with stale settings
    setFeedUrl(UpdateFeed::feedUrl(UpdateFeed::channel()));
    setAutomaticChecks(config()->get(Config::GUI_CheckForUpdates).toBool());
    startUpdater();
}

void AutoUpdater::applySettings()
{
    if (!m_started) {
        return;
    }
    setFeedUrl(UpdateFeed::feedUrl(UpdateFeed::channel()));
    setAutomaticChecks(config()->get(Config::GUI_CheckForUpdates).toBool());
}
