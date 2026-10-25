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

#include "UpdateFeed.h"

#include "config-keepassx.h"
#include "core/Config.h"

#include <QRegularExpression>
#include <QSysInfo>
#include <QXmlStreamReader>

namespace UpdateFeed
{
    const QString StableChannel = QStringLiteral("stable");
    const QString NightlyChannel = QStringLiteral("nightly");

    namespace
    {
        const QString SparkleNamespace = QStringLiteral("http://www.andymatuschak.org/xml-namespaces/sparkle");

        QList<int> versionParts(const QString& version)
        {
            static const QRegularExpression dotted(R"(^\d+(\.\d+)*$)");
            QList<int> parts;
            if (!dotted.match(version).hasMatch()) {
                return parts;
            }
            for (const auto& part : version.split('.')) {
                bool ok = false;
                parts.append(part.toInt(&ok));
                if (!ok) {
                    return {};
                }
            }
            return parts;
        }
    } // namespace

    QString buildChannel()
    {
        return QStringLiteral(KEEPASSXC_UPDATE_CHANNEL);
    }

    bool isEnabled()
    {
        return !buildChannel().isEmpty();
    }

    QString channel()
    {
        auto picked = config()->get(Config::GUI_UpdateChannel).toString();
        if (picked == StableChannel || picked == NightlyChannel) {
            return picked;
        }
        return buildChannel();
    }

    QString feedFileName()
    {
#if defined(Q_OS_MACOS)
        const QString platform = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
        const QString platform = QStringLiteral("windows");
#else
        const QString platform = QStringLiteral("linux");
#endif
        return QStringLiteral("appcast-%1-%2.xml").arg(platform, QSysInfo::buildCpuArchitecture());
    }

    QUrl feedUrl(const QString& channel)
    {
        // GitHub redirects releases/latest to the newest published release, which skips drafts and pre-releases
        const QString base = QStringLiteral(KEEPASSXC_UPDATE_FEED_BASE);
        if (channel == NightlyChannel) {
            return QUrl(base + QStringLiteral("/download/nightly/") + feedFileName());
        }
        return QUrl(base + QStringLiteral("/latest/download/") + feedFileName());
    }

    int compareVersions(const QString& left, const QString& right)
    {
        auto l = versionParts(left);
        auto r = versionParts(right);
        if (l.isEmpty() || r.isEmpty()) {
            return 0;
        }
        for (int i = 0; i < qMax(l.size(), r.size()); ++i) {
            int a = l.value(i, 0);
            int b = r.value(i, 0);
            if (a != b) {
                return a < b ? -1 : 1;
            }
        }
        return 0;
    }

    Release latestRelease(const QByteArray& appcast)
    {
        Release latest;
        QXmlStreamReader xml(appcast);
        Release item;
        bool inItem = false;

        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name() == QLatin1String("item")) {
                    inItem = true;
                    item = {};
                } else if (!inItem) {
                    continue;
                } else if (xml.name() == QLatin1String("enclosure")) {
                    auto attributes = xml.attributes();
                    item.downloadUrl = QUrl(attributes.value("url").toString());
                    // Older appcasts carry the versions on the enclosure instead of in elements
                    if (item.version.isEmpty()) {
                        item.version = attributes.value(SparkleNamespace, "version").toString();
                    }
                    if (item.displayVersion.isEmpty()) {
                        item.displayVersion = attributes.value(SparkleNamespace, "shortVersionString").toString();
                    }
                } else if (xml.namespaceUri() == SparkleNamespace && xml.name() == QLatin1String("version")) {
                    item.version = xml.readElementText().trimmed();
                } else if (xml.namespaceUri() == SparkleNamespace
                           && xml.name() == QLatin1String("shortVersionString")) {
                    item.displayVersion = xml.readElementText().trimmed();
                } else if (xml.namespaceUri().isEmpty() && xml.name() == QLatin1String("link")) {
                    item.infoUrl = QUrl(xml.readElementText().trimmed());
                }
            } else if (xml.isEndElement() && xml.name() == QLatin1String("item")) {
                inItem = false;
                if (item.displayVersion.isEmpty()) {
                    item.displayVersion = item.version;
                }
                if (!versionParts(item.version).isEmpty()
                    && (!latest.isValid() || compareVersions(latest.version, item.version) < 0)) {
                    latest = item;
                }
            }
        }

        if (xml.hasError()) {
            return {};
        }
        return latest;
    }
} // namespace UpdateFeed
