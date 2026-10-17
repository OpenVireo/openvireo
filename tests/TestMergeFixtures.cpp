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

#include "TestMergeFixtures.h"

#include "config-keepassx-tests.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/EntryAttributes.h"
#include "core/Group.h"
#include "crypto/Crypto.h"
#include "gui/remote/SyncEngine.h"
#include "mock/MockClock.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

QTEST_GUILESS_MAIN(TestMergeFixtures)

/*
 * The three-way merge cases in tests/data/merge-fixtures (see the README there), shared with the TS
 * port of SyncEngine's merge. Set KPXC_UPDATE_MERGE_FIXTURES=1 to write what this merge produces into
 * each fixture's "expected" instead of checking it.
 */
namespace
{
    const QString FixtureDir = QStringLiteral(KEEPASSX_TEST_DATA_DIR "/merge-fixtures");
    // Every copy shares the root group, as copies of one database do
    const QUuid RootUuid("{6e3a9f40-1c2b-4d5e-8f70-9a1b2c3d4e5f}");
    const QDateTime RootTime = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::utc());

    QDateTime parseTime(const QJsonValue& value)
    {
        return QDateTime::fromString(value.toString(), Qt::ISODate).toUTC();
    }

    QString formatTime(const QDateTime& time)
    {
        return time.toUTC().toString(Qt::ISODate);
    }

    QSharedPointer<Database> buildDatabase(const QJsonObject& json)
    {
        auto db = QSharedPointer<Database>::create();
        auto* root = db->rootGroup();
        root->setUpdateTimeinfo(false);
        root->setUuid(RootUuid);
        root->setName("Root");
        TimeInfo rootTime;
        rootTime.setCreationTime(RootTime);
        rootTime.setLastModificationTime(RootTime);
        rootTime.setLastAccessTime(RootTime);
        rootTime.setLocationChanged(RootTime);
        root->setTimeInfo(rootTime);

        for (const auto& value : json.value("entries").toArray()) {
            const auto object = value.toObject();
            auto* entry = new Entry();
            entry->setUpdateTimeinfo(false);
            entry->setUuid(QUuid(object.value("uuid").toString()));
            const auto attributes = object.value("attributes").toObject();
            const auto protectedKeys = object.value("protected").toArray().toVariantList();
            for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                entry->attributes()->set(it.key(), it.value().toString(), protectedKeys.contains(it.key()));
            }
            entry->setTags(object.value("tags").toString());
            const auto attachments = object.value("attachments").toObject();
            for (auto it = attachments.begin(); it != attachments.end(); ++it) {
                entry->attachments()->set(it.key(), QByteArray::fromBase64(it.value().toString().toLatin1()));
            }
            const auto modified = parseTime(object.value("modified"));
            TimeInfo time;
            time.setCreationTime(RootTime);
            time.setLastModificationTime(modified);
            time.setLastAccessTime(modified);
            time.setLocationChanged(RootTime);
            entry->setTimeInfo(time);
            entry->setGroup(root);
            entry->setUpdateTimeinfo(true);
        }
        for (const auto& value : json.value("deleted").toArray()) {
            const auto object = value.toObject();
            db->addDeletedObject({QUuid(object.value("uuid").toString()), parseTime(object.value("time"))});
        }
        return db;
    }

    // What a merge result is compared by; the TS side builds the same projection
    QJsonObject project(const Database* db)
    {
        QList<const Entry*> entries;
        for (const auto* entry : db->rootGroup()->entriesRecursive()) {
            entries << entry;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry* a, const Entry* b) {
            return a->uuid().toString() < b->uuid().toString();
        });
        QJsonArray entryArray;
        for (const auto* entry : entries) {
            QJsonObject attributes;
            QJsonArray protectedKeys;
            for (const auto& key : entry->attributes()->keys()) {
                const auto value = entry->attributes()->value(key);
                // Standard fields always exist; an empty one is left out
                if (EntryAttributes::isDefaultAttribute(key) && value.isEmpty()) {
                    continue;
                }
                attributes[key] = value;
                if (!EntryAttributes::isDefaultAttribute(key) && entry->attributes()->isProtected(key)) {
                    protectedKeys << key;
                }
            }
            QJsonObject attachments;
            for (const auto& key : entry->attachments()->keys()) {
                attachments[key] = QString::fromLatin1(entry->attachments()->value(key).toBase64());
            }
            QJsonObject object{{"uuid", entry->uuid().toString(QUuid::WithoutBraces)},
                               {"modified", formatTime(entry->timeInfo().lastModificationTime())},
                               {"attributes", attributes},
                               {"history_count", entry->historyItems().size()}};
            if (!protectedKeys.isEmpty()) {
                object["protected"] = protectedKeys;
            }
            if (!entry->tags().isEmpty()) {
                object["tags"] = entry->tags();
            }
            if (!attachments.isEmpty()) {
                object["attachments"] = attachments;
            }
            entryArray << object;
        }
        QStringList deleted;
        for (const auto& object : db->deletedObjects()) {
            if (object.uuid != RootUuid) {
                deleted << object.uuid.toString(QUuid::WithoutBraces);
            }
        }
        deleted.sort();
        return {{"entries", entryArray}, {"deleted", QJsonArray::fromStringList(deleted)}};
    }
} // namespace

void TestMergeFixtures::initTestCase()
{
    QVERIFY(Crypto::init());
}

void TestMergeFixtures::cleanup()
{
    MockClock::teardown();
}

void TestMergeFixtures::testFixtures_data()
{
    QTest::addColumn<QString>("path");
    const auto files = QDir(FixtureDir).entryList({"*.json"}, QDir::Files, QDir::Name);
    QVERIFY(!files.isEmpty());
    for (const auto& file : files) {
        QTest::newRow(qPrintable(file)) << QDir(FixtureDir).filePath(file);
    }
}

void TestMergeFixtures::testFixtures()
{
    QFETCH(QString, path);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QVERIFY(!fixture.isEmpty());

    MockClock::setup(new MockClock(parseTime(fixture.value("now"))));
    auto local = buildDatabase(fixture.value("local").toObject());
    auto remote = buildDatabase(fixture.value("remote").toObject());
    const bool hasBase = fixture.value("base").isObject();
    auto base = hasBase ? buildDatabase(fixture.value("base").toObject()) : QSharedPointer<Database>();

    // The fields both sides changed differently, found before merging
    QJsonObject conflicts;
    if (base) {
        for (const auto* localEntry : local->rootGroup()->entriesRecursive()) {
            const auto fields = SyncEngine::conflictingFields(localEntry,
                                                              remote->rootGroup()->findEntryByUuid(localEntry->uuid()),
                                                              base->rootGroup()->findEntryByUuid(localEntry->uuid()));
            if (!fields.isEmpty()) {
                conflicts[localEntry->uuid().toString(QUuid::WithoutBraces)] = QJsonArray::fromStringList(fields);
            }
        }
    }

    SyncEngine::mergeDatabases(local.data(), remote.data(), base.data());
    const auto result = project(local.data());
    // Both sides end up the same
    auto remoteResult = project(remote.data());
    QCOMPARE(QJsonDocument(remoteResult.value("entries").toArray()).toJson(),
             QJsonDocument(result.value("entries").toArray()).toJson());

    const QJsonObject actual{{"result", result}, {"conflicts", conflicts}};
    if (qEnvironmentVariableIsSet("KPXC_UPDATE_MERGE_FIXTURES")) {
        fixture["expected"] = actual;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(fixture).toJson(QJsonDocument::Indented));
        return;
    }
    const auto expected = fixture.value("expected").toObject();
    QCOMPARE(QString::fromUtf8(QJsonDocument(actual).toJson(QJsonDocument::Indented)),
             QString::fromUtf8(QJsonDocument(expected).toJson(QJsonDocument::Indented)));
}
