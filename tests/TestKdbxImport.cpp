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

#include "TestKdbxImport.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/KdbxImport.h"
#include "core/Metadata.h"
#include "crypto/Crypto.h"
#include "crypto/Random.h"
#include "keys/FileKey.h"
#include "keys/PasswordKey.h"

#include <QTest>

QTEST_GUILESS_MAIN(TestKdbxImport)

namespace
{
    Entry* addEntry(Group* group, const QString& title, const QString& password)
    {
        auto entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setTitle(title);
        entry->setPassword(password);
        entry->setGroup(group);
        return entry;
    }

    Group* addGroup(Group* parent, const QString& name)
    {
        auto group = new Group();
        group->setUuid(QUuid::createUuid());
        group->setName(name);
        group->setParent(parent);
        return group;
    }

    Entry* byTitle(Group* root, const QString& title)
    {
        for (auto entry : root->entriesRecursive()) {
            if (entry->title() == title) {
                return entry;
            }
        }
        return nullptr;
    }

    QSharedPointer<Database> vault()
    {
        auto db = QSharedPointer<Database>::create();
        db->rootGroup()->setName("Work");
        addEntry(db->rootGroup(), "Existing", "x");
        db->metadata()->customData()->set("KPXC_REMOTE_SYNC_SETTINGS", "vault's own");
        return db;
    }

    QString hex(const QUuid& uuid)
    {
        return QString::fromLatin1(uuid.toRfc4122().toHex()).toUpper();
    }
} // namespace

void TestKdbxImport::initTestCase()
{
    QVERIFY(Crypto::init());
    QVERIFY(m_dir.isValid());
    m_keyFile = m_dir.filePath("source.key");
    QFile key(m_keyFile);
    QVERIFY(key.open(QIODevice::WriteOnly));
    key.write(randomGen()->randomArray(32));
}

// A password and key file protected .kdbx with a recycle bin, custom data, a custom icon and a reference
QString TestKdbxImport::writeSource()
{
    Database db;
    db.rootGroup()->setName("Home");
    auto web = addGroup(db.rootGroup(), "Web");
    auto mail = addEntry(web, "Mail", "mail-pw");
    const auto icon = QUuid::createUuid();
    db.metadata()->addCustomIcon(icon, QByteArray("icon"));
    mail->setIcon(icon);
    auto alias = addEntry(web, "Mail alias", QString("{REF:P@I:%1}").arg(hex(mail->uuid())));
    Q_UNUSED(alias)
    auto binned = addEntry(db.rootGroup(), "Binned", "gone");
    db.recycleEntry(binned);
    db.metadata()->customData()->set("KPXC_REMOTE_SYNC_SETTINGS", "file's server");
    db.metadata()->customData()->set("KPXC_BROWSER_key", "secret");

    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create("source-pw"));
    auto fileKey = QSharedPointer<FileKey>::create();
    fileKey->load(m_keyFile);
    key->addKey(fileKey);
    db.setKey(key);
    const auto path = m_dir.filePath(QString("source-%1.kdbx").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QString error;
    if (!db.saveAs(path, Database::Atomic, {}, &error)) {
        qWarning() << error;
    }
    return path;
}

void TestKdbxImport::testRead()
{
    const auto path = writeSource();
    QString error;
    auto db = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    QVERIFY2(db, qPrintable(error));
    QVERIFY(db->filePath().isEmpty());
    QCOMPARE(db->rootGroup()->name(), QString("Home"));

    QVERIFY(!KdbxImport::read(path, "wrong", m_keyFile, &error));
    QVERIFY(error.contains("hardware key"));
    QVERIFY(!KdbxImport::read(path, "source-pw", {}, &error));
}

void TestKdbxImport::testMergeInto()
{
    QString error;
    auto source = KdbxImport::read(writeSource(), "source-pw", m_keyFile, &error);
    QVERIFY2(source, qPrintable(error));
    auto target = vault();
    const auto s = KdbxImport::mergeInto(source.data(), target.data());
    QCOMPARE(s.added, 2);
    QCOMPARE(s.updated, 0);
    QCOMPARE(s.unchanged, 0);
    QCOMPARE(s.groupsAdded, 1);

    auto root = target->rootGroup();
    QCOMPARE(root->name(), QString("Work"));
    QVERIFY(byTitle(root, "Mail"));
    QVERIFY(byTitle(root, "Mail alias"));
    QVERIFY(!byTitle(root, "Binned"));
    QCOMPARE(target->metadata()->customData()->value("KPXC_REMOTE_SYNC_SETTINGS"), QString("vault's own"));
    QVERIFY(!target->metadata()->customData()->contains("KPXC_BROWSER_key"));
    QVERIFY(target->metadata()->hasCustomIcon(byTitle(root, "Mail")->iconUuid()));
}

void TestKdbxImport::testMergeTwiceChangesNothing()
{
    const auto path = writeSource();
    QString error;
    auto target = vault();
    auto first = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    KdbxImport::mergeInto(first.data(), target.data());
    const auto count = target->rootGroup()->entriesRecursive().size();
    target->markAsClean();

    auto second = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    const auto s = KdbxImport::mergeInto(second.data(), target.data());
    QCOMPARE(s.added, 0);
    QCOMPARE(s.updated, 0);
    QCOMPARE(s.unchanged, 2);
    QCOMPARE(target->rootGroup()->entriesRecursive().size(), count);
    QVERIFY(!target->isModified());
}

void TestKdbxImport::testMergeNewerWinsAndNothingIsDeleted()
{
    const auto path = writeSource();
    QString error;
    auto target = vault();
    // Even a target that syncs with Synchronize keeps what the file no longer has
    target->rootGroup()->setMergeMode(Group::Synchronize);
    auto first = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    KdbxImport::mergeInto(first.data(), target.data());

    auto second = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    auto mail = byTitle(second->rootGroup(), "Mail");
    QTest::qSleep(1100);
    mail->beginUpdate();
    mail->setPassword("newer-in-file");
    mail->endUpdate();
    delete byTitle(second->rootGroup(), "Mail alias");

    const auto s = KdbxImport::mergeInto(second.data(), target.data());
    QCOMPARE(s.updated, 1);
    QCOMPARE(byTitle(target->rootGroup(), "Mail")->password(), QString("newer-in-file"));
    QVERIFY(byTitle(target->rootGroup(), "Mail alias"));
    QVERIFY(byTitle(target->rootGroup(), "Existing"));
}

void TestKdbxImport::testAddAsCopies()
{
    const auto path = writeSource();
    QString error;
    auto target = vault();
    auto imports = addGroup(target->rootGroup(), "Imports");

    auto source = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    const auto originalMail = byTitle(source->rootGroup(), "Mail")->uuid();
    const auto s = KdbxImport::addAsCopies(source.data(), imports);
    QCOMPARE(s.added, 2);
    QCOMPARE(s.groupsAdded, 2);

    QCOMPARE(imports->children().size(), 1);
    auto home = imports->children().first();
    QCOMPARE(home->name(), QString("Home"));
    QVERIFY(!byTitle(target->rootGroup(), "Binned"));
    auto mail = byTitle(home, "Mail");
    QVERIFY(mail->uuid() != originalMail);
    QCOMPARE(byTitle(home, "Mail alias")->password(), QString("{REF:P@I:%1}").arg(hex(mail->uuid())));
    QVERIFY(target->metadata()->hasCustomIcon(mail->iconUuid()));
    QCOMPARE(target->metadata()->customData()->value("KPXC_REMOTE_SYNC_SETTINGS"), QString("vault's own"));

    // A second import is a second copy
    auto again = KdbxImport::read(path, "source-pw", m_keyFile, &error);
    KdbxImport::addAsCopies(again.data(), imports);
    QCOMPARE(imports->children().size(), 2);
    QVERIFY(byTitle(imports->children().last(), "Mail")->uuid() != mail->uuid());
}

void TestKdbxImport::testSummaryText()
{
    KdbxImport::Summary s;
    s.added = 12;
    s.updated = 3;
    s.unchanged = 40;
    QCOMPARE(KdbxImport::summaryText("Work", false, s),
             QString("Imported into \"Work\": 12 new entries, 3 updated, 40 unchanged."));
    QCOMPARE(KdbxImport::summaryText("Work", false, {}),
             QString("Nothing to import: \"Work\" already has everything in this file."));
    KdbxImport::Summary copies;
    copies.added = 1;
    QCOMPARE(KdbxImport::summaryText("Work", true, copies), QString("Imported 1 entries into \"Work\" as copies."));
}
