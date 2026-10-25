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

#include "TestUpdateCheck.h"
#include "core/Config.h"
#include "crypto/Crypto.h"
#include "networking/UpdateFeed.h"

#include <QTemporaryDir>
#include <QTest>

QTEST_GUILESS_MAIN(TestUpdateCheck)

using UpdateFeed::compareVersions;

void TestUpdateCheck::initTestCase()
{
    QVERIFY(Crypto::init());
    QLocale::setDefault(QLocale::c());

    // Keep the channel setting out of the user's configuration
    static QTemporaryDir configDir;
    QVERIFY(configDir.isValid());
    Config::createConfigFromFile(configDir.filePath("config.ini"), configDir.filePath("local.ini"));
}

void TestUpdateCheck::testCompareVersion()
{
    // No upgrade
    QCOMPARE(compareVersions("2.3.0", "2.3.0"), 0);

    // First, second and third digit upgrades
    QCOMPARE(compareVersions("2.4.0", "3.0.0"), -1);
    QCOMPARE(compareVersions("3.0.0", "2.4.0"), 1);
    QCOMPARE(compareVersions("2.3.4", "2.4.0"), -1);
    QCOMPARE(compareVersions("2.4.0", "2.3.4"), 1);
    QCOMPARE(compareVersions("2.3.0", "2.3.1"), -1);
    QCOMPARE(compareVersions("2.3.10", "2.3.9"), 1);

    // Nightlies append their build number: later nightlies and the next release sort above them
    QCOMPARE(compareVersions("2.8.0.26102503", "2.8.0.26102603"), -1);
    QCOMPARE(compareVersions("2.8.0.26102503", "2.8.1"), -1);
    QCOMPARE(compareVersions("2.8.0", "2.8.0.26102503"), -1);
    QCOMPARE(compareVersions("2.8.0", "2.8.0.0"), 0);

    // Versions that are not dotted numbers never count as newer
    QCOMPARE(compareVersions("2.3.4-snapshot", "2.4.0"), 0);
    QCOMPARE(compareVersions("2.4.0", "invalid"), 0);
    QCOMPARE(compareVersions("2.4.0", ""), 0);
    QCOMPARE(compareVersions("2.4.0", "2.4.99999999999"), 0);
}

void TestUpdateCheck::testLatestRelease()
{
    // Items in any order, versions as elements or as enclosure attributes
    const QByteArray appcast = R"(<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>OpenVireo</title>
    <link>https://openvireo.com</link>
    <item>
      <title>OpenVireo 2.8.0-nightly-20261024</title>
      <link>https://github.com/openvireo/openvireo/releases/tag/nightly</link>
      <sparkle:version>2.8.0.26102403</sparkle:version>
      <sparkle:shortVersionString>2.8.0-nightly-20261024</sparkle:shortVersionString>
      <enclosure url="https://example.com/old.dmg" length="1" type="application/octet-stream"
                 sparkle:edSignature="c2ln"/>
    </item>
    <item>
      <title>OpenVireo 2.8.0-nightly-20261025</title>
      <link>https://github.com/openvireo/openvireo/releases/tag/nightly</link>
      <enclosure url="https://example.com/new.dmg" length="1" type="application/octet-stream"
                 sparkle:version="2.8.0.26102503" sparkle:shortVersionString="2.8.0-nightly-20261025"
                 sparkle:edSignature="c2ln"/>
    </item>
    <item>
      <title>Broken</title>
      <sparkle:version>not-a-version</sparkle:version>
    </item>
  </channel>
</rss>)";

    auto release = UpdateFeed::latestRelease(appcast);
    QVERIFY(release.isValid());
    QCOMPARE(release.version, QString("2.8.0.26102503"));
    QCOMPARE(release.displayVersion, QString("2.8.0-nightly-20261025"));
    QCOMPARE(release.downloadUrl, QUrl("https://example.com/new.dmg"));
    QCOMPARE(release.infoUrl, QUrl("https://github.com/openvireo/openvireo/releases/tag/nightly"));

    QVERIFY(!UpdateFeed::latestRelease("<rss><channel></channel></rss>").isValid());
    QVERIFY(!UpdateFeed::latestRelease("not xml").isValid());
    QVERIFY(!UpdateFeed::latestRelease("<rss><channel><item><sparkle:version>1.0").isValid());
}

void TestUpdateCheck::testFeedUrl()
{
    auto name = UpdateFeed::feedFileName();
    QVERIFY(name.startsWith("appcast-"));
    QVERIFY(name.endsWith(".xml"));

    QCOMPARE(UpdateFeed::feedUrl(UpdateFeed::StableChannel).toString(),
             QString("https://github.com/openvireo/openvireo/releases/latest/download/") + name);
    QCOMPARE(UpdateFeed::feedUrl(UpdateFeed::NightlyChannel).toString(),
             QString("https://github.com/openvireo/openvireo/releases/download/nightly/") + name);

    // A picked channel wins over the build's; anything else falls back to it
    config()->set(Config::GUI_UpdateChannel, UpdateFeed::NightlyChannel);
    QCOMPARE(UpdateFeed::channel(), UpdateFeed::NightlyChannel);
    config()->set(Config::GUI_UpdateChannel, QString("beta"));
    QCOMPARE(UpdateFeed::channel(), UpdateFeed::buildChannel());
    config()->set(Config::GUI_UpdateChannel, QString());
    QCOMPARE(UpdateFeed::channel(), UpdateFeed::buildChannel());
}
