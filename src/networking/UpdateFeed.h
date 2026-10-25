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

#ifndef KEEPASSXC_UPDATEFEED_H
#define KEEPASSXC_UPDATEFEED_H

#include <QString>
#include <QUrl>

/**
 * Update channels and their feeds. Each channel publishes one Sparkle appcast per platform and
 * architecture, attached to a GitHub release: stable feeds to the latest release and nightly feeds
 * to the rolling "nightly" pre-release. Sparkle, WinSparkle and the update checker read the same feeds.
 */
namespace UpdateFeed
{
    extern const QString StableChannel;
    extern const QString NightlyChannel;

    struct Release
    {
        QString version; // sparkle:version, compared with KEEPASSXC_UPDATE_VERSION
        QString displayVersion; // sparkle:shortVersionString, or version if missing
        QUrl downloadUrl;
        QUrl infoUrl;

        bool isValid() const
        {
            return !version.isEmpty();
        }
    };

    // The channel this build was made for, or empty if it does not update (local and snapshot builds)
    QString buildChannel();
    bool isEnabled();
    // The channel the user picked in the settings, falling back to the build's channel
    QString channel();

    // File name of this platform's feed, e.g. appcast-macos-arm64.xml
    QString feedFileName();
    QUrl feedUrl(const QString& channel);

    // Compares dotted numeric versions such as 2.8.0 and 2.8.0.26102503; missing parts count as 0.
    // Returns <0, 0 or >0; versions that are not dotted numbers compare as equal to everything.
    int compareVersions(const QString& left, const QString& right);

    // The newest item of an appcast, or an invalid release if it has none
    Release latestRelease(const QByteArray& appcast);
} // namespace UpdateFeed

#endif // KEEPASSXC_UPDATEFEED_H
