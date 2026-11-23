/*
 *  Copyright (C) 2026 Bongani Mbigi
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

#include "SyncEngine.h"

#include "core/Clock.h"
#include "core/CustomData.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttachments.h"
#include "core/EntryAttributes.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "crypto/Random.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
#include "account/AccountManager.h"
#include "gui/account/AccountController.h"
#include "keys/PasswordKey.h"

#include <botan/mem_ops.h>
#include <ovcore.h>

#include <QBuffer>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>

namespace
{
    /*
     * Opens a database file on a thread of its own and hands it to `context`'s thread, or null when it can't
     * be read with `key`. Reading derives the key, which takes a second or more by design, and a sync must not
     * hold up the window or wait in a nested event loop that lets the user change things meanwhile. Unlike
     * Database::open() the database doesn't watch its file.
     *
     * A thread of its own rather than the thread pool, because the reader waits for the pool to derive the key.
     */
    void openInBackground(const QString& filePath,
                          const QSharedPointer<const CompositeKey>& key,
                          QObject* context,
                          std::function<void(QSharedPointer<Database>)> done)
    {
        auto* target = context->thread();
        auto result = std::make_shared<Database*>(nullptr);
        auto* thread = QThread::create([filePath, key, target, result] {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) {
                return;
            }
            auto* db = new Database();
            db->setEmitModified(false);
            KeePass2Reader reader;
            if (!reader.readDatabase(&file, key, db)) {
                delete db;
                return;
            }
            db->setFilePath(filePath);
            db->markAsClean();
            db->setEmitModified(true);
            // Objects belong to the thread that made them. Groups and entries are the database's children and
            // move with it; history items aren't anyone's children, so they move one by one.
            for (auto* entry : db->rootGroup()->entriesRecursive()) {
                for (auto* item : entry->historyItems()) {
                    item->moveToThread(target);
                }
            }
            db->moveToThread(target);
            *result = db;
        });
        QPointer<QObject> guard(context);
        QObject::connect(thread, &QThread::finished, thread, [thread, result, guard, onDone = std::move(done)] {
            thread->deleteLater();
            QSharedPointer<Database> db(*result);
            if (guard) {
                onDone(db);
            }
        });
        thread->start();
    }

    bool readFile(const QString& filePath, QByteArray* data, QString* error)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = SyncEngine::tr("Failed to read database %1: %2").arg(filePath, file.errorString());
            return false;
        }
        *data = file.readAll();
        return true;
    }

    qint64 seconds(const Entry* entry)
    {
        return entry->timeInfo().lastModificationTime().toSecsSinceEpoch();
    }

    // The parts of an entry that are merged one by one, by name; a missing value means "absent"
    using Fields = QMap<QString, QVariant>;

    Fields fieldsOf(const Entry* entry)
    {
        Fields fields;
        const auto* attributes = entry->attributes();
        for (const auto& key : attributes->keys()) {
            // The file marks the standard fields protected by the database's settings, whatever the entry says,
            // so a copy read back from it can differ from the one in memory in that flag alone
            const bool protect = !EntryAttributes::isDefaultAttribute(key) && attributes->isProtected(key);
            fields["attribute/" + key] = QStringList{attributes->value(key), protect ? "protected" : QString()};
        }
        const auto* attachments = entry->attachments();
        for (const auto& key : attachments->keys()) {
            fields["attachment/" + key] = attachments->value(key);
        }
        fields["tags"] = entry->tags();
        const auto& timeInfo = entry->timeInfo();
        fields["expiry"] = timeInfo.expires() ? QVariant(timeInfo.expiryTime().toSecsSinceEpoch()) : QVariant("never");
        fields["icon"] = entry->iconUuid().isNull() ? QVariant(entry->iconNumber()) : QVariant(entry->iconUuid());
        fields["colors"] = QStringList{entry->foregroundColor(), entry->backgroundColor()};
        return fields;
    }

    QString fieldName(const QString& field)
    {
        if (field == "attribute/" + EntryAttributes::TitleKey) {
            return SyncEngine::tr("Title");
        } else if (field == "attribute/" + EntryAttributes::UserNameKey) {
            return SyncEngine::tr("Username");
        } else if (field == "attribute/" + EntryAttributes::PasswordKey) {
            return SyncEngine::tr("Password");
        } else if (field == "attribute/" + EntryAttributes::URLKey) {
            return SyncEngine::tr("URL");
        } else if (field == "attribute/" + EntryAttributes::NotesKey) {
            return SyncEngine::tr("Notes");
        } else if (field.startsWith("attribute/")) {
            return field.mid(QString("attribute/").size());
        } else if (field.startsWith("attachment/")) {
            return SyncEngine::tr("attachment %1").arg(field.mid(QString("attachment/").size()));
        } else if (field == "tags") {
            return SyncEngine::tr("Tags");
        } else if (field == "expiry") {
            return SyncEngine::tr("Expiry");
        } else if (field == "icon") {
            return SyncEngine::tr("Icon");
        }
        return SyncEngine::tr("Colors");
    }

    void applyField(Entry* entry, const QString& field, const QVariant& value)
    {
        if (field.startsWith("attribute/")) {
            const auto key = field.mid(QString("attribute/").size());
            if (!value.isValid()) {
                // The standard fields always exist
                EntryAttributes::isDefaultAttribute(key) ? entry->attributes()->set(key, {})
                                                         : entry->attributes()->remove(key);
            } else {
                const auto parts = value.toStringList();
                const bool protect = EntryAttributes::isDefaultAttribute(key) ? entry->attributes()->isProtected(key)
                                                                              : !parts.value(1).isEmpty();
                entry->attributes()->set(key, parts.value(0), protect);
            }
        } else if (field.startsWith("attachment/")) {
            const auto key = field.mid(QString("attachment/").size());
            value.isValid() ? entry->attachments()->set(key, value.toByteArray()) : entry->attachments()->remove(key);
        } else if (field == "tags") {
            entry->setTags(value.toString());
        } else if (field == "expiry") {
            auto timeInfo = entry->timeInfo();
            const bool expires = value.toString() != "never";
            timeInfo.setExpires(expires);
            if (expires) {
                timeInfo.setExpiryTime(QDateTime::fromSecsSinceEpoch(value.toLongLong(), QTimeZone::utc()).toUTC());
            }
            entry->setTimeInfo(timeInfo);
        } else if (field == "icon") {
            value.typeId() == QMetaType::QUuid ? entry->setIcon(value.toUuid()) : entry->setIcon(value.toInt());
        } else if (field == "colors") {
            const auto colors = value.toStringList();
            entry->setForegroundColor(colors.value(0));
            entry->setBackgroundColor(colors.value(1));
        }
    }

    struct Resolution
    {
        Fields merged;
        QStringList conflicts;
    };

    // Three-way merge of the fields: a field one side changed takes that change; one both changed
    // differently takes the newer side's value and counts as a conflict
    Resolution resolveFields(const Entry* local, const Entry* remote, const Entry* base, bool localIsNewer)
    {
        const auto localFields = fieldsOf(local);
        const auto remoteFields = fieldsOf(remote);
        const auto baseFields = fieldsOf(base);
        QStringList keys = localFields.keys() + remoteFields.keys() + baseFields.keys();
        keys.removeDuplicates();

        Resolution resolution;
        for (const auto& key : keys) {
            const auto localValue = localFields.value(key);
            const auto remoteValue = remoteFields.value(key);
            const auto baseValue = baseFields.value(key);
            QVariant value;
            if (localValue == remoteValue || remoteValue == baseValue) {
                value = localValue;
            } else if (localValue == baseValue) {
                value = remoteValue;
            } else {
                value = localIsNewer ? localValue : remoteValue;
                resolution.conflicts << fieldName(key);
            }
            if (value.isValid()) {
                resolution.merged.insert(key, value);
            }
        }
        return resolution;
    }

    // Gives both entries the combined fields and the same modification time, so Merger sees one version.
    // Each keeps its previous state in its history.
    void combineEntries(Entry* local, Entry* remote, const Entry* base)
    {
        const bool localIsNewer = seconds(local) >= seconds(remote);
        const auto resolution = resolveFields(local, remote, base, localIsNewer);
        auto* newer = localIsNewer ? local : remote;
        auto* older = localIsNewer ? remote : local;
        const auto when = QDateTime::fromSecsSinceEpoch(
            qMax(Clock::currentDateTimeUtc().toSecsSinceEpoch(), qMax(seconds(local), seconds(remote))),
            QTimeZone::utc()).toUTC();

        for (auto* entry : {older, newer}) {
            entry->beginUpdate();
            if (entry == older) {
                // Everything not merged field by field comes from the newer entry
                entry->copyDataFrom(newer);
            }
            entry->setUpdateTimeinfo(false);
            const auto current = fieldsOf(entry);
            QStringList keys = current.keys() + resolution.merged.keys();
            keys.removeDuplicates();
            for (const auto& key : keys) {
                if (current.value(key) != resolution.merged.value(key)) {
                    applyField(entry, key, resolution.merged.value(key));
                }
            }
            auto timeInfo = entry->timeInfo();
            timeInfo.setLastModificationTime(when);
            entry->setTimeInfo(timeInfo);
            entry->endUpdate();
            entry->setUpdateTimeinfo(true);
        }
    }

    bool sameFields(const Entry* a, const Entry* b)
    {
        return fieldsOf(a) == fieldsOf(b);
    }

    /*
     * Which side changed an entry since the last sync, from the snapshot taken then. Times alone can't
     * always tell: a file keeps whole seconds, so an edit in the same second as a sync looks unchanged.
     * This device's copy is in memory and keeps milliseconds; the server's is read from its file.
     */
    struct Changes
    {
        bool local = false;
        bool remote = false;
    };

    Changes changesSince(const Entry* local, const Entry* remote, const QDateTime& snapshot)
    {
        Changes changes;
        const auto localTime = local->timeInfo().lastModificationTime();
        // Read back from the file since the snapshot, the milliseconds are gone but it is the same version
        const bool reloaded =
            localTime.time().msec() == 0 && localTime.toSecsSinceEpoch() == snapshot.toSecsSinceEpoch();
        changes.local = localTime != snapshot && !reloaded;
        // If this side didn't change and the contents differ, the other side did, whatever the times say
        changes.remote =
            seconds(remote) != snapshot.toSecsSinceEpoch() || (!changes.local && !sameFields(local, remote));
        return changes;
    }

    /*
     * The deletions `from` made that `into` hasn't applied: recorded in `from`, gone there, still in `into`.
     * The root group is never deleted.
     */
    QHash<QUuid, DeletedObject> pendingDeletions(const Database* from, const Database* into)
    {
        QSet<QUuid> present;
        for (const auto* entry : from->rootGroup()->entriesRecursive()) {
            present.insert(entry->uuid());
        }
        for (const auto* group : from->rootGroup()->groupsRecursive(true)) {
            present.insert(group->uuid());
        }
        QSet<QUuid> theirs;
        for (const auto* entry : into->rootGroup()->entriesRecursive()) {
            theirs.insert(entry->uuid());
        }
        for (const auto* group : into->rootGroup()->groupsRecursive(false)) {
            theirs.insert(group->uuid());
        }

        QHash<QUuid, DeletedObject> pending;
        for (const auto& object : from->deletedObjects()) {
            if (!present.contains(object.uuid) && theirs.contains(object.uuid) && !pending.contains(object.uuid)) {
                pending.insert(object.uuid, object);
            }
        }
        return pending;
    }

    bool sameGroup(const Group* a, const Group* b)
    {
        return a->name() == b->name() && a->notes() == b->notes() && a->iconNumber() == b->iconNumber()
               && a->iconUuid() == b->iconUuid() && a->parentGroup()->uuid() == b->parentGroup()->uuid();
    }

    int depthOf(const Group* group)
    {
        int depth = 0;
        for (auto* parent = group->parentGroup(); parent; parent = parent->parentGroup()) {
            ++depth;
        }
        return depth;
    }

    // Deletes without touching the parent's times; the deletion is recorded by the caller
    template <class T> void erase(T* item, Group* parent)
    {
        const bool updateTimeinfo = parent->canUpdateTimeinfo();
        parent->setUpdateTimeinfo(false);
        delete item;
        parent->setUpdateTimeinfo(updateTimeinfo);
    }

    /*
     * Where an entry both sides have ends up: in the group of the copy moved last (newer location-changed
     * time, whole seconds as the files keep them; local wins a tie), whichever copy wins on its fields.
     * Merger replaces the older-modified copy with the newer one, location and all, so a move made on one
     * device and an edit made on the other would leave the copies in different groups with the same
     * location-changed time, and no later sync would bring them together. Found before anything is
     * merged, applied to both after Merger.
     */
    struct Location
    {
        QUuid group; // null for the root group, which Merger treats as the same group whatever its UUID
        QDateTime changed;
    };

    QHash<QUuid, Location> locationsOf(const Database* local, const Database* remote)
    {
        QHash<QUuid, Location> locations;
        for (const auto* localEntry : local->rootGroup()->entriesRecursive()) {
            const auto* remoteEntry = remote->rootGroup()->findEntryByUuid(localEntry->uuid());
            if (!remoteEntry || !localEntry->group() || !remoteEntry->group()) {
                continue;
            }
            const auto localTime = localEntry->timeInfo().locationChanged();
            const auto remoteTime = remoteEntry->timeInfo().locationChanged();
            const auto* winner =
                localTime.toSecsSinceEpoch() >= remoteTime.toSecsSinceEpoch() ? localEntry : remoteEntry;
            const auto* group = winner->group();
            const bool inRoot = group == group->database()->rootGroup();
            locations.insert(localEntry->uuid(),
                             {inRoot ? QUuid() : group->uuid(), winner->timeInfo().locationChanged()});
        }
        return locations;
    }

    // Puts each copy where `locations` says, without touching any modification times
    Merger::ChangeList applyLocations(Database* local, Database* remote, const QHash<QUuid, Location>& locations)
    {
        Merger::ChangeList changes;
        for (auto it = locations.begin(); it != locations.end(); ++it) {
            bool moved = false;
            Entry* localEntry = nullptr;
            for (auto* db : {local, remote}) {
                auto* entry = db->rootGroup()->findEntryByUuid(it.key());
                auto* group =
                    it.value().group.isNull() ? db->rootGroup() : db->rootGroup()->findGroupByUuid(it.value().group);
                if (!entry || !group) {
                    continue;
                }
                if (db == local) {
                    localEntry = entry;
                }
                auto* previous = entry->group();
                if (previous == group && entry->timeInfo().locationChanged() == it.value().changed) {
                    continue;
                }
                const bool entryUpdates = entry->canUpdateTimeinfo();
                const bool previousUpdates = previous->canUpdateTimeinfo();
                const bool groupUpdates = group->canUpdateTimeinfo();
                entry->setUpdateTimeinfo(false);
                previous->setUpdateTimeinfo(false);
                group->setUpdateTimeinfo(false);
                entry->setGroup(group);
                auto timeInfo = entry->timeInfo();
                timeInfo.setLocationChanged(it.value().changed);
                entry->setTimeInfo(timeInfo);
                group->setUpdateTimeinfo(groupUpdates);
                previous->setUpdateTimeinfo(previousUpdates);
                entry->setUpdateTimeinfo(entryUpdates);
                moved = moved || previous != group;
            }
            if (moved && localEntry) {
                changes << Merger::Change(
                    Merger::Change::Type::Moved, *localEntry, SyncEngine::tr("Moved to where it was moved last"));
            }
        }
        return changes;
    }

    void setDeletion(Database* db, const QUuid& uuid, const DeletedObject* object)
    {
        QList<DeletedObject> deletions;
        for (const auto& existing : db->deletedObjects()) {
            if (existing.uuid != uuid) {
                deletions << existing;
            }
        }
        if (object) {
            deletions << *object;
        }
        db->setDeletedObjects(deletions);
    }

    /*
     * Applies the deletions either side made since the last sync (`pending`, found before Merger ran and
     * brought the items back) to both. Whether to delete is decided by the base, not by deletion and
     * modification times, so a device with a wrong clock can't delete what another device changed: an
     * item goes only when both copies still match the base (same fields, or for a group the same name,
     * notes and icon; in the same parent group) and, for a group, nothing is left in it. A deletion held
     * back is dropped, so the item stays on both.
     */
    Merger::ChangeList
    applyDeletions(Database* local, Database* remote, const Database* base, const QHash<QUuid, DeletedObject>& pending)
    {
        Merger::ChangeList changes;
        const QList<Database*> both = {local, remote};

        for (auto it = pending.begin(); it != pending.end(); ++it) {
            const auto* baseEntry = base->rootGroup()->findEntryByUuid(it.key());
            QList<Entry*> copies;
            bool unchanged = baseEntry != nullptr;
            for (auto* db : both) {
                auto* entry = db->rootGroup()->findEntryByUuid(it.key());
                if (entry) {
                    copies << entry;
                    unchanged = unchanged && entry->group()->uuid() == baseEntry->group()->uuid()
                                && sameFields(entry, baseEntry);
                }
            }
            if (copies.isEmpty()) {
                continue;
            }
            if (unchanged) {
                changes << Merger::Change(
                    Merger::Change::Type::Deleted, *copies.first(), SyncEngine::tr("Deleted on another device"));
                for (auto* entry : copies) {
                    erase(entry, entry->group());
                }
            }
            for (auto* db : both) {
                setDeletion(db, it.key(), unchanged ? &it.value() : nullptr);
            }
        }

        // Deepest first, so a group's deleted subgroups are gone before it is looked at
        QList<Group*> groups;
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (auto* group = local->rootGroup()->findGroupByUuid(it.key())) {
                if (group != local->rootGroup()) {
                    groups << group;
                }
            }
        }
        std::stable_sort(
            groups.begin(), groups.end(), [](const Group* a, const Group* b) { return depthOf(a) > depthOf(b); });
        for (auto* localGroup : groups) {
            const auto uuid = localGroup->uuid();
            const auto object = pending.value(uuid);
            const auto* baseGroup = base->rootGroup()->findGroupByUuid(uuid);
            QList<Group*> copies;
            bool unchanged = baseGroup != nullptr && baseGroup != base->rootGroup();
            for (auto* db : both) {
                auto* group = db->rootGroup()->findGroupByUuid(uuid);
                if (group) {
                    copies << group;
                    unchanged = unchanged && group->entries().isEmpty() && group->children().isEmpty()
                                && sameGroup(group, baseGroup);
                }
            }
            if (unchanged) {
                changes << Merger::Change(
                    Merger::Change::Type::Deleted, *localGroup, SyncEngine::tr("Deleted on another device"));
                for (auto* group : copies) {
                    erase(group, group->parentGroup());
                }
            }
            for (auto* db : both) {
                setDeletion(db, uuid, unchanged ? &object : nullptr);
            }
        }
        return changes;
    }

    /*
     * Merger doesn't carry the Recycle Bin (KeePassXC's "missing handling of recycle bin"), so a bin made
     * on one device was a plain group on the others, and deleting from it there moved things into a bin
     * of their own. As in KeePass, the bin changed last wins on both sides; another bin is emptied into it
     * and deleted.
     */
    Merger::ChangeList mergeRecycleBins(Database* local, Database* remote)
    {
        Merger::ChangeList changes;
        const auto* localBin = local->metadata()->recycleBin();
        const auto* remoteBin = remote->metadata()->recycleBin();
        if (!localBin && !remoteBin) {
            return changes;
        }
        if (localBin && remoteBin && localBin->uuid() == remoteBin->uuid()) {
            return changes;
        }
        const Database* winner = !remoteBin ? local : !localBin ? remote : nullptr;
        if (!winner) {
            // Whole seconds, as the files keep them; the UUIDs' bytes break a tie
            const auto localTime = local->metadata()->recycleBinChanged().toSecsSinceEpoch();
            const auto remoteTime = remote->metadata()->recycleBinChanged().toSecsSinceEpoch();
            winner = localTime != remoteTime
                         ? (localTime > remoteTime ? local : remote)
                         : (localBin->uuid().toRfc4122() < remoteBin->uuid().toRfc4122() ? local : remote);
        }
        const auto binUuid = winner->metadata()->recycleBin()->uuid();
        const auto binChanged = winner->metadata()->recycleBinChanged();
        QSet<QUuid> others;
        for (const auto* bin : {localBin, remoteBin}) {
            if (bin && bin->uuid() != binUuid) {
                others.insert(bin->uuid());
            }
        }

        for (auto* db : {local, remote}) {
            auto* bin = db->rootGroup()->findGroupByUuid(binUuid);
            if (!bin) {
                continue;
            }
            for (const auto& uuid : others) {
                auto* other = db->rootGroup()->findGroupByUuid(uuid);
                if (!other || other == bin || other->findGroupByUuid(binUuid)) {
                    continue;
                }
                for (auto* entry : other->entries()) {
                    entry->setGroup(bin);
                }
                for (auto* child : other->children()) {
                    child->setParent(bin);
                }
                // Recorded, so the devices that sync next delete it too
                delete other;
            }
            if (db->metadata()->recycleBin() != bin) {
                db->metadata()->setRecycleBin(bin);
                db->metadata()->setRecycleBinChanged(binChanged);
                if (db == local) {
                    changes << Merger::Change(Merger::Change::Type::Metadata,
                                              SyncEngine::tr("Recycle Bin set from another device"));
                }
            }
        }
        if (changes.isEmpty() && !others.isEmpty()) {
            changes << Merger::Change(Merger::Change::Type::Metadata,
                                      SyncEngine::tr("Recycle Bin set from another device"));
        }
        return changes;
    }

    /*
     * Whether merging needs the common base: deletions to apply, an entry both sides changed, or one side changed but
     * doesn't look newer (a device whose clock is behind, or an edit in the same second as the last
     * sync). Merger alone keeps the newer-looking copy in those cases.
     */
    bool needsCommonBase(const Database* local, const Database* remote, const QHash<QUuid, QDateTime>& snapshot)
    {
        if (!pendingDeletions(local, remote).isEmpty() || !pendingDeletions(remote, local).isEmpty()) {
            return true;
        }
        for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
            const auto* localEntry = local->rootGroup()->findEntryByUuid(it.key());
            const auto* remoteEntry = remote->rootGroup()->findEntryByUuid(it.key());
            if (!localEntry || !remoteEntry || sameFields(localEntry, remoteEntry)) {
                continue;
            }
            const auto changes = changesSince(localEntry, remoteEntry, it.value());
            if ((changes.local && changes.remote) || (changes.local && seconds(localEntry) <= seconds(remoteEntry))
                || (changes.remote && seconds(remoteEntry) <= seconds(localEntry))) {
                return true;
            }
        }
        return false;
    }

    // ---- the account-vault merge in openvireo-core

    rust::Slice<const uint8_t> slice(const QByteArray& data)
    {
        return {reinterpret_cast<const uint8_t*>(data.constData()), static_cast<size_t>(data.size())};
    }

    QByteArray toByteArray(const rust::Vec<uint8_t>& data)
    {
        return QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
    }

    /*
     * The parts of `db` the sync merge reads and writes (groups, entries with history, custom data and icons,
     * deletion records, the Recycle Bin), as a new database with a throwaway key and a cheap KDF: the copies only
     * exist to hand the core serialized bytes, and the user's key never crosses.
     */
    QSharedPointer<Database> mergeCopyOf(const Database* db, const QSharedPointer<const CompositeKey>& key)
    {
        auto copy = QSharedPointer<Database>::create();
        copy->setEmitModified(false);
        copy->metadata()->copyAttributesFrom(db->metadata());
        copy->metadata()->customData()->copyDataFrom(db->metadata()->customData());
        // KeePassXC restamps these on every save; merged as data they would make each copy look newer than the
        // other forever
        copy->metadata()->customData()->removeStampItems();
        for (const auto& uuid : db->metadata()->customIconsOrder()) {
            copy->metadata()->addCustomIcon(uuid, db->metadata()->customIcon(uuid));
        }
        auto* root = db->rootGroup()->clone(Entry::CloneIncludeHistory, Group::CloneIncludeEntries);
        delete copy->setRootGroup(root);
        // Cloning puts the copies into their groups "now"; the merge needs the original times (LocationChanged
        // decides moves)
        for (auto* group : copy->rootGroup()->groupsRecursive(true)) {
            if (const auto* original = db->rootGroup()->findGroupByUuid(group->uuid())) {
                group->setUpdateTimeinfo(false);
                group->setTimeInfo(original->timeInfo());
            }
        }
        for (auto* entry : copy->rootGroup()->entriesRecursive()) {
            if (const auto* original = db->rootGroup()->findEntryByUuid(entry->uuid())) {
                entry->setUpdateTimeinfo(false);
                entry->setTimeInfo(original->timeInfo());
            }
        }
        copy->setDeletedObjects(db->deletedObjects());
        if (const auto* bin = db->metadata()->recycleBin()) {
            copy->metadata()->setRecycleBin(copy->rootGroup()->findGroupByUuid(bin->uuid()));
        }
        auto kdf = QSharedPointer<Argon2Kdf>::create(Argon2Kdf::Type::Argon2id);
        kdf->setMemory(1024);
        kdf->setRounds(1);
        kdf->setParallelism(1);
        copy->setKdf(kdf);
        copy->setKey(key);
        return copy;
    }

    bool serialize(const QSharedPointer<Database>& db, QByteArray* data)
    {
        QBuffer buffer(data);
        buffer.open(QIODevice::WriteOnly);
        KeePass2Writer writer;
        return writer.writeDatabase(&buffer, db.data());
    }

    QSharedPointer<Database> deserialize(const QByteArray& data, const QSharedPointer<const CompositeKey>& key)
    {
        auto db = QSharedPointer<Database>::create();
        db->setEmitModified(false);
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        KeePass2Reader reader;
        if (!reader.readDatabase(&buffer, key, db.data())) {
            return {};
        }
        return db;
    }

    void replaceHistory(Entry* target, const Entry* source)
    {
        target->removeHistoryItems(target->historyItems());
        for (const auto* item : source->historyItems()) {
            target->addHistoryItem(item->clone(Entry::CloneNoFlags));
        }
    }

    void applyGroup(const Group* source,
                    Group* target,
                    QHash<QUuid, Group*>& groups,
                    QHash<QUuid, Entry*>& entries,
                    QSet<QUuid>& keptGroups,
                    QSet<QUuid>& keptEntries)
    {
        keptGroups.insert(target->uuid());
        // Time updates stay off until applyMerged() is done: placing entries and groups must not touch it
        target->setUpdateTimeinfo(false);

        int index = 0;
        for (const auto* child : source->children()) {
            auto* group = groups.value(child->uuid());
            if (!group) {
                group = new Group();
                group->setUuid(child->uuid());
                groups.insert(child->uuid(), group);
            }
            group->setUpdateTimeinfo(false);
            group->setParent(target, index++, false);
            applyGroup(child, group, groups, entries, keptGroups, keptEntries);
        }

        for (const auto* sourceEntry : source->entries()) {
            keptEntries.insert(sourceEntry->uuid());
            auto* entry = entries.value(sourceEntry->uuid());
            if (entry && entry->group() == target
                && entry->equals(sourceEntry, CompareItemIgnoreMilliseconds | CompareItemIgnoreStatistics)) {
                // Unchanged: keep it as it is, milliseconds included
                continue;
            }
            if (!entry) {
                entry = new Entry();
                entry->setUuid(sourceEntry->uuid());
                entries.insert(sourceEntry->uuid(), entry);
            }
            entry->setUpdateTimeinfo(false);
            if (entry->group() != target) {
                entry->setGroup(target, false);
            }
            replaceHistory(entry, sourceEntry);
            // Also turns time updates back on
            entry->copyDataFrom(sourceEntry);
        }
        target->copyDataFrom(source);
    }

    /*
     * Makes `target` hold what `source` (the core's merge result) holds, changing its groups and entries in place
     * rather than replacing them: the same objects stay, so the views keep their selection (by UUID) and the
     * entry being edited. Entries the merge didn't change aren't touched.
     */
    void applyMerged(Database* target, const Database* source)
    {
        QHash<QUuid, Group*> groups;
        for (auto* group : target->rootGroup()->groupsRecursive(true)) {
            groups.insert(group->uuid(), group);
        }
        QHash<QUuid, Entry*> entries;
        for (auto* entry : target->rootGroup()->entriesRecursive()) {
            entries.insert(entry->uuid(), entry);
        }
        // Copies of one database share a root group whatever its UUID
        groups.insert(source->rootGroup()->uuid(), target->rootGroup());

        QSet<QUuid> keptGroups;
        QSet<QUuid> keptEntries;
        applyGroup(source->rootGroup(), target->rootGroup(), groups, entries, keptGroups, keptEntries);
        keptGroups.insert(target->rootGroup()->uuid());

        for (auto* entry : target->rootGroup()->entriesRecursive()) {
            if (!keptEntries.contains(entry->uuid())) {
                delete entry;
            }
        }
        // Children first
        auto stale = target->rootGroup()->groupsRecursive(false);
        std::reverse(stale.begin(), stale.end());
        for (auto* group : stale) {
            if (!keptGroups.contains(group->uuid())) {
                delete group;
            }
        }

        for (const auto& uuid : source->metadata()->customIconsOrder()) {
            if (!target->metadata()->hasCustomIcon(uuid)) {
                target->metadata()->addCustomIcon(uuid, source->metadata()->customIcon(uuid));
            }
        }
        // The merge copies carry no stamp items (see mergeCopyOf): the target keeps its own unless its data changed
        auto* customData = target->metadata()->customData();
        CustomData current;
        current.copyDataFrom(customData);
        current.removeStampItems();
        if (current != *source->metadata()->customData()) {
            customData->copyDataFrom(source->metadata()->customData());
        }
        if (const auto* bin = source->metadata()->recycleBin()) {
            target->metadata()->setRecycleBin(target->rootGroup()->findGroupByUuid(bin->uuid()));
        }
        // Last: deleting stale items above recorded deletions of their own
        target->setDeletedObjects(source->deletedObjects());
        for (auto* group : target->rootGroup()->groupsRecursive(true)) {
            group->setUpdateTimeinfo(true);
        }
    }

    Merger::Change::Type changeType(ov::ChangeType change)
    {
        switch (change) {
        case ov::ChangeType::Added:
            return Merger::Change::Type::Added;
        case ov::ChangeType::Modified:
            return Merger::Change::Type::Modified;
        case ov::ChangeType::Moved:
            return Merger::Change::Type::Moved;
        case ov::ChangeType::Deleted:
            return Merger::Change::Type::Deleted;
        case ov::ChangeType::Metadata:
            return Merger::Change::Type::Metadata;
        default:
            return Merger::Change::Type::Unspecified;
        }
    }
} // namespace

SyncEngine::SyncEngine(const QSharedPointer<Database>& db,
                       SyncServerClient* client,
                       const SyncStateStore& stateStore,
                       QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_client(client)
    , m_stateStore(stateStore)
{
    m_client->setParent(this);
    m_backoffTimer.setSingleShot(true);
    connect(&m_backoffTimer, &QTimer::timeout, this, &SyncEngine::sync);
    if (m_db) {
        connect(m_db.data(), &Database::databaseSaved, this, [this] { m_coreSavedSince = true; });
    }
}

SyncEngine::~SyncEngine()
{
    if (m_coreCancel) {
        m_coreCancel();
    }
    m_client->abort();
    cleanup();
}

void SyncEngine::setMergeHandler(MergeHandler handler)
{
    m_mergeHandler = std::move(handler);
}

void SyncEngine::setSaveHandler(SaveHandler handler)
{
    m_saveHandler = std::move(handler);
}

void SyncEngine::setMergeGate(MergeGate gate)
{
    m_mergeGate = std::move(gate);
}

void SyncEngine::setRetryOnNetworkError(bool retry)
{
    m_retryOnNetworkError = retry;
}

QString SyncEngine::pendingUnlockPath() const
{
    return m_state == State::WaitingForUnlock ? m_remotePath : QString();
}

SyncEngine::State SyncEngine::state() const
{
    return m_state;
}

bool SyncEngine::isBusy() const
{
    return m_state != State::Idle && m_state != State::Backoff && m_state != State::NeedsAttention;
}

QSharedPointer<Database> SyncEngine::database() const
{
    return m_db;
}

const RemoteParams& SyncEngine::params() const
{
    return m_client->params();
}

int SyncEngine::backoffMsec() const
{
    return m_backoffMsec;
}

void SyncEngine::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);
    }
}

SyncEngine::Attention SyncEngine::attention() const
{
    return m_attention;
}

QString SyncEngine::lastSyncedEtag() const
{
    return m_stateStore.load(m_db->filePath(), params()).etag;
}

void SyncEngine::sync()
{
    if (isBusy()) {
        return;
    }
    m_acceptServerState = false;
    start();
}

void SyncEngine::syncAcceptingServerState()
{
    if (isBusy()) {
        return;
    }
    m_acceptServerState = true;
    start();
}

void SyncEngine::start()
{
    m_attention = Attention::None;
    m_backoffTimer.stop();
    m_attempt = 0;
    m_unlockedViaDialog = false;
    m_syncState = m_stateStore.load(m_db->filePath(), params());

    m_coreSyncer = SyncServerClient::accountVaultSyncerFor(params());
    if (m_coreSyncer) {
        startCoreSync();
        return;
    }

    m_workDir.reset(new QTemporaryDir());
    if (!m_workDir->isValid()) {
        fail(tr("Failed to create a temporary folder for the sync: %1").arg(m_workDir->errorString()));
        return;
    }
    m_remotePath = m_workDir->filePath("remote.kdbx");

    startCheck();
}

void SyncEngine::cancel()
{
    const bool wasBusy = isBusy();
    if (m_coreCancel) {
        m_coreCancel();
    }
    m_client->abort();
    m_backoffTimer.stop();
    m_backoffMsec = 0;
    cleanup();
    setState(State::Idle);
    if (wasBusy) {
        emit finished(false, tr("The sync was cancelled."));
    }
}

bool SyncEngine::hasLocalChanges() const
{
    if (m_db->isModified() || !m_syncState.lastSync.isValid()) {
        return true;
    }
    // Saved since the last sync, by this app or by something else writing the file
    return QFileInfo(m_db->filePath()).lastModified() > m_syncState.lastSync;
}

void SyncEngine::startCheck()
{
    if (m_syncState.isEmpty() || hasLocalChanges()) {
        startDownload();
        return;
    }

    // Nothing changed here since the last sync: only download when the server has a new version
    setState(State::Checking);
    emit progress(10, tr("Checking…"));
    m_client->check(m_syncState.etag, [this](const SyncServerClient::Response& response) {
        noteServerTime(response);
        if (response.status == 304) {
            if (acceptRevision(response.revision)) {
                succeed(m_syncState.etag, response.revision);
            }
        } else if (response.status == 200 || response.status == 404) {
            startDownload();
        } else {
            fail(SyncServerClient::errorMessage(response), response.status == 0);
        }
    });
}

void SyncEngine::startDownload()
{
    setState(State::Downloading);
    emit progress(25, tr("Downloading..."));
    m_client->download([this](const SyncServerClient::Response& response) {
        noteServerTime(response);
        if (response.status == 404) {
            if (!m_acceptServerState && m_syncState.revision > 0) {
                needAttention(Attention::ServerLostDatabase,
                              tr("The sync server no longer has this database, although this device synced it "
                                 "before. It may have been deleted or the server reset."));
                return;
            }
            // Nothing on the server yet: publish the local database as the first version
            QString error;
            if (m_db->isModified() && (!m_saveHandler || !m_saveHandler(&error))) {
                fail(error.isEmpty() ? tr("The database must be saved before it can be uploaded.") : error);
                return;
            }
            m_remoteEtag.clear();
            m_remoteRevision = -1;
            uploadFile(m_db->filePath());
            return;
        }
        if (response.status != 200) {
            fail(SyncServerClient::errorMessage(response), response.status == 0);
            return;
        }
        if (!acceptRevision(response.revision)) {
            return;
        }

        QFile file(m_remotePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size()) {
            fail(tr("Failed to write downloaded database to %1: %2").arg(m_remotePath, file.errorString()));
            return;
        }
        file.close();
        m_remoteEtag = response.etag;
        m_remoteRevision = response.revision;
        mergeDownload();
    });
}

void SyncEngine::mergeDownload()
{
    if (m_mergeGate && !m_mergeGate()) {
        setState(State::WaitingToMerge);
        emit mergeDeferred();
        return;
    }

    setState(State::Merging);
    emit progress(50, tr("Syncing..."));
    if (!m_remoteDb) {
        openDownload();
        return;
    }

    auto conflicts = findConflicts(m_db.data(), m_remoteDb.data(), m_syncState.entries);

    // The copy both sides last synced from tells which fields each side changed, and which deletions to
    // apply. Opening it costs another key derivation, so only when merging needs it.
    const bool editedOnBoth = std::any_of(conflicts.begin(), conflicts.end(), [](const SyncConflict& conflict) {
        return conflict.kind == SyncConflict::Kind::EditedOnBoth;
    });
    if (!m_baseOpened && (editedOnBoth || needsCommonBase(m_db.data(), m_remoteDb.data(), m_syncState.entries))) {
        openBase();
        return;
    }
    if (m_baseDb) {
        QList<SyncConflict> remaining;
        for (auto conflict : conflicts) {
            const auto* baseEntry = m_baseDb->rootGroup()->findEntryByUuid(conflict.uuid);
            if (conflict.kind == SyncConflict::Kind::EditedOnBoth && baseEntry) {
                conflict.fields = conflictingFields(m_db->rootGroup()->findEntryByUuid(conflict.uuid),
                                                    m_remoteDb->rootGroup()->findEntryByUuid(conflict.uuid),
                                                    baseEntry);
                if (conflict.fields.isEmpty()) {
                    // Different fields changed: both changes are kept, nothing to report
                    continue;
                }
            }
            remaining << conflict;
        }
        conflicts = remaining;
    }

    QString error;
    if (!m_mergeHandler || !m_mergeHandler(m_remoteDb, m_baseDb.data(), &error)) {
        fail(error.isEmpty() ? tr("The databases could not be merged.") : error);
        return;
    }
    m_remoteDb.reset();
    m_baseDb.reset();
    if (!conflicts.isEmpty()) {
        emit conflictsDetected(conflicts);
    }
    uploadFile(m_remotePath);
}

void SyncEngine::openDownload()
{
    const auto run = m_openRun;
    openInBackground(m_remotePath, m_db->key(), this, [this, run](const QSharedPointer<Database>& db) {
        // Cancelled, or another sync started, while it was being read
        if (run != m_openRun || m_state != State::Merging) {
            return;
        }
        if (!db) {
            // Another device changed the password or key file: the owner asks the user to unlock it
            setState(State::WaitingForUnlock);
            emit unlockRequired(m_remotePath);
            return;
        }
        db->markAsTemporaryDatabase();
        m_remoteDb = db;

        // The revision comes from the server, the generation from inside the encrypted copy
        m_remoteGeneration = generationOf(m_remoteDb.data());
        if (!m_acceptServerState && m_remoteGeneration < m_syncState.generation) {
            needAttention(Attention::ServerRolledBack,
                          tr("The sync server returned a copy of the database that is older than one this device has "
                             "already synced (sync generation %1, expected at least %2).")
                              .arg(m_remoteGeneration)
                              .arg(m_syncState.generation));
            return;
        }
        // The user may have started editing while it was read
        mergeDownload();
    });
}

void SyncEngine::resumeMerge()
{
    if (m_state != State::WaitingToMerge || (m_mergeGate && !m_mergeGate())) {
        return;
    }
    if (m_coreResult) {
        applyCoreResult();
    } else if (m_coreSyncer) {
        startCoreSync();
    } else {
        mergeDownload();
    }
}

// ---- account vaults, synced in openvireo-core

void SyncEngine::startCoreSync()
{
    // The merged file comes back into the database, so it waits while the user edits, like a merge does
    if (m_mergeGate && !m_mergeGate()) {
        setState(State::WaitingToMerge);
        emit mergeDeferred();
        return;
    }
    setState(State::Downloading);
    emit progress(25, tr("Syncing..."));

    // A copy of what the window holds, unsaved changes included, under the vault key the database is under
    AccountVaultSync::Request request;
    if (!m_db->key() || !serialize(mergeCopyOf(m_db.data(), m_db->key()), &request.kdbx)) {
        fail(tr("The vault could not be prepared for the sync."));
        return;
    }
    request.vaultId = params().vaultId;
    request.localChanged = hasLocalChanges() || m_coreLocalAhead;
    request.acceptServerState = m_acceptServerState;
    const auto basePath = m_stateStore.basePath(m_db->filePath(), params());
    QFile baseFile(basePath);
    if (baseFile.open(QIODevice::ReadOnly)) {
        request.base = baseFile.readAll();
    }
    m_coreSent = request.kdbx;
    m_coreSentKey = m_db->key();
    m_coreSentLocalChanged = request.localChanged;
    m_coreSavedSince = false;
    m_coreResult.reset();

    const auto run = m_openRun;
    QPointer<SyncEngine> guard(this);
    m_coreCancel = m_coreSyncer(request, [guard, run](const AccountVaultSync::Result& result) {
        if (!guard || run != guard->m_openRun) {
            return;
        }
        guard->m_coreCancel = {};
        guard->finishCoreSync(result);
    });
}

void SyncEngine::finishCoreSync(const AccountVaultSync::Result& result)
{
    if (result.hasClockOffset) {
        m_clockOffsetMsec = result.clockOffsetMsec;
    }
    if (!result.ok()) {
        if (result.errorCode == "server_rolled_back") {
            needAttention(Attention::ServerRolledBack,
                          tr("The sync server returned an older version of the vault than this device has already "
                             "synced (%1).")
                              .arg(result.error));
        } else if (result.errorCode == "server_lost_database") {
            needAttention(Attention::ServerLostDatabase,
                          tr("The sync server no longer has this vault, although this device synced it before. It "
                             "may have been deleted or the server reset."));
        } else if (result.errorCode == "too_many_conflicts") {
            fail(tr("The database on the sync server was changed by another device during the sync. "
                    "Your local changes are saved, sync again to merge them."));
        } else {
            fail(result.error.isEmpty() ? tr("The vault could not be synced.") : result.error,
                 result.errorCode == "network_error");
        }
        return;
    }
    m_coreResult.reset(new AccountVaultSync::Result(result));
    applyCoreResult();
}

void SyncEngine::applyCoreResult()
{
    if (m_mergeGate && !m_mergeGate()) {
        setState(State::WaitingToMerge);
        emit mergeDeferred();
        return;
    }
    setState(State::Merging);
    emit progress(75, tr("Syncing..."));
    QScopedPointer<AccountVaultSync::Result> result(m_coreResult.take());
    // Edits made while the core synced aren't on the server yet
    const bool editedDuringSync = m_db->isModified() || m_coreSavedSince;
    bool localAhead = editedDuringSync;
    auto keyFile = result->keyFile;
    Botan::secure_scrub_memory(result->keyFile.data(), static_cast<size_t>(result->keyFile.size()));
    const auto key = AccountManager::vaultCompositeKey(keyFile);
    Botan::secure_scrub_memory(keyFile.data(), static_cast<size_t>(keyFile.size()));
    if (!key) {
        fail(tr("The vault came back without a usable key."));
        return;
    }

    if (result->outcome != AccountVaultSync::Outcome::Unchanged) {
        auto merged = deserialize(result->kdbx, key);
        if (!merged) {
            fail(tr("The merged vault could not be opened."));
            return;
        }
        // Merge the result into what the window holds now, with what was sent as the base, so edits made while the
        // core synced stay. Without such edits this makes the window hold the result. Edits the result lacks go
        // out with the next sync.
        auto sent = deserialize(m_coreSent, m_coreSentKey);
        bool ok = false;
        bool ahead = false;
        mergeAccountVault(m_db.data(), merged.data(), sent.data(), &ok, &ahead);
        if (!ok) {
            fail(tr("The databases could not be merged."));
            return;
        }
        // The core merged everything that was sent when it was told about local changes, so without edits made
        // during the sync the window can't hold anything the result lacks. Syncing again for such a difference
        // would repeat forever.
        localAhead = ahead && (editedDuringSync || !m_coreSentLocalChanged);
        if (ahead && !localAhead) {
            qWarning("SyncEngine: the synced vault differs from what was sent without edits; not syncing again");
        }
    }
    m_coreSent.clear();
    m_coreSentKey.reset();
    // After a key rotation the server copy is under the new vault key, and so is the local copy from now on
    if (m_db->key()->rawKey() != key->rawKey()) {
        m_db->setKey(key, true, true);
    }
    if (m_db->isModified()) {
        QString error;
        if (!m_saveHandler || !m_saveHandler(&error)) {
            fail(error.isEmpty() ? tr("The database must be saved before it can be uploaded.") : error);
            return;
        }
    }
    QList<SyncConflict> conflicts;
    for (const auto& conflict : result->conflicts) {
        SyncConflict c;
        c.uuid = QUuid::fromRfc4122(QByteArray::fromHex(conflict.uuid.toLatin1()));
        c.title = conflict.title;
        c.kind = conflict.kind == 1   ? SyncConflict::Kind::DeletedHereEditedThere
                 : conflict.kind == 2 ? SyncConflict::Kind::EditedHereDeletedThere
                                      : SyncConflict::Kind::EditedOnBoth;
        c.fields = conflict.fields;
        conflicts << c;
    }
    if (!conflicts.isEmpty()) {
        emit conflictsDetected(conflicts);
    }
    // What the server has now is the next merge's base
    m_coreLocalAhead = localAhead;
    succeed(result->etag,
            -1,
            result->outcome == AccountVaultSync::Outcome::Unchanged ? QByteArray() : result->kdbx);
    if (m_coreLocalAhead) {
        // Upload what was edited meanwhile
        QTimer::singleShot(0, this, &SyncEngine::sync);
    }
}

void SyncEngine::resumeAfterUnlock(bool merged, const QString& error)
{
    if (m_state != State::WaitingForUnlock) {
        return;
    }
    if (!merged) {
        fail(error.isEmpty() ? tr("Remote database unlock cancelled.") : error);
        return;
    }
    m_unlockedViaDialog = true;
    uploadFile(m_remotePath);
}

void SyncEngine::uploadFile(const QString& filePath)
{
    QByteArray data;
    QString error;
    if (!readFile(filePath, &data, &error)) {
        fail(error);
        return;
    }
    startUpload(data);
}

void SyncEngine::startUpload(const QByteArray& data)
{
    // The merge didn't change the server copy, so there is nothing to upload
    if (!m_remoteEtag.isEmpty() && SyncServerClient::etagFor(data) == m_remoteEtag) {
        succeed(m_remoteEtag, m_remoteRevision, data);
        return;
    }

    setState(State::Uploading);
    emit progress(75, tr("Uploading..."));
    m_uploadData = data;
    m_client->upload(data, m_remoteEtag, [this](const SyncServerClient::Response& response) {
        noteServerTime(response);
        if (response.status == 200 || response.status == 201) {
            succeed(response.etag, response.revision, m_uploadData);
            return;
        }
        if (response.status == 412) {
            // Another device uploaded since our download: merge its version too and try again
            if (!m_unlockedViaDialog && ++m_attempt < MaxAttempts) {
                startDownload();
                return;
            }
            fail(tr("The database on the sync server was changed by another device during the sync. "
                    "Your local changes are saved, sync again to merge them."));
            return;
        }
        fail(tr("Failed to upload merged database. %1").arg(SyncServerClient::errorMessage(response)),
             response.status == 0);
    });
}

void SyncEngine::succeed(const QString& etag, qint64 revision, const QByteArray& serverData)
{
    if (!serverData.isEmpty()) {
        // Kept like the database itself: encrypted, and readable only by the user
        const auto basePath = m_stateStore.basePath(m_db->filePath(), params());
        QDir().mkpath(QFileInfo(basePath).absolutePath());
        QSaveFile file(basePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            file.write(serverData);
            file.commit();
        }
    }

    SyncState state;
    state.etag = etag;
    // Accepting the server's state starts the comparison over from what it has now
    const auto& seen = m_acceptServerState ? SyncState() : m_syncState;
    state.revision = qMax(seen.revision, revision);
    state.generation = qMax(qMax(seen.generation, m_remoteGeneration), generationOf(m_db.data()));
    state.entries = snapshotOf(m_db.data());
    state.lastSync = QDateTime::currentDateTimeUtc();
    m_stateStore.save(m_db->filePath(), params(), state);
    m_syncState = state;

    cleanup();
    m_backoffMsec = 0;
    m_acceptServerState = false;
    setState(State::Idle);
    emit finished(true, {});
}

void SyncEngine::noteServerTime(const SyncServerClient::Response& response)
{
    if (response.serverTime.isValid()) {
        m_clockOffsetMsec = response.serverTime.msecsTo(Clock::currentDateTimeUtc());
    }
}

qint64 SyncEngine::clockOffsetMsec() const
{
    return m_clockOffsetMsec;
}

bool SyncEngine::isClockSkewed(qint64 offsetMsec)
{
    return qAbs(offsetMsec) > ClockSkewWarningMsec;
}

void SyncEngine::openBase()
{
    m_baseOpened = true;
    const auto basePath = m_stateStore.basePath(m_db->filePath(), params());
    if (!QFile::exists(basePath)) {
        mergeDownload();
        return;
    }
    const auto run = m_openRun;
    openInBackground(basePath, m_db->key(), this, [this, run](const QSharedPointer<Database>& base) {
        if (run != m_openRun || m_state != State::Merging) {
            return;
        }
        // A base under another password (e.g. from before a password change) is no use, merging goes on without it
        if (base) {
            base->markAsTemporaryDatabase();
        }
        m_baseDb = base;
        mergeDownload();
    });
}

bool SyncEngine::acceptRevision(qint64 revision)
{
    if (m_acceptServerState || revision < 0 || revision >= m_syncState.revision) {
        return true;
    }
    needAttention(Attention::ServerRolledBack,
                  tr("The sync server returned an older version of the database (revision %1) than this device has "
                     "already synced (revision %2).")
                      .arg(revision)
                      .arg(m_syncState.revision));
    return false;
}

void SyncEngine::needAttention(Attention attention, const QString& message)
{
    // Nothing is merged or uploaded, and nothing retries until the user decides
    cleanup();
    m_backoffTimer.stop();
    m_backoffMsec = 0;
    m_attention = attention;
    setState(State::NeedsAttention);
    emit finished(false, message);
}

qint64 SyncEngine::generationOf(const Database* db)
{
    return db->metadata()->customData()->value(CustomData::SyncGeneration).toLongLong();
}

QStringList SyncEngine::conflictingFields(const Entry* local, const Entry* remote, const Entry* base)
{
    if (!local || !remote || !base) {
        return {};
    }
    return resolveFields(local, remote, base, true).conflicts;
}

Merger::ChangeList SyncEngine::mergeDatabases(Database* local, Database* remote, const Database* base)
{
    if (AccountController::isAccountVaultPath(local->filePath())) {
        bool ok = false;
        auto changes = mergeAccountVault(local, remote, base, &ok);
        if (ok) {
            return changes;
        }
        qWarning("SyncEngine: openvireo-core couldn't merge the account vault, merging with KeePassXC's Merger");
    }
    return mergeHere(local, remote, base);
}

Merger::ChangeList
SyncEngine::mergeAccountVault(Database* local, Database* remote, const Database* base, bool* ok, bool* remoteChanged)
{
    if (ok) {
        *ok = false;
    }
    if (remoteChanged) {
        *remoteChanged = false;
    }
    // A throwaway key for the serialized copies
    auto key = QSharedPointer<CompositeKey>::create();
    auto passwordKey = QSharedPointer<PasswordKey>::create();
    passwordKey->setRawKey(randomGen()->randomArray(32));
    key->addKey(passwordKey);
    const auto components = passwordKey->rawKey();

    QByteArray localData, remoteData, baseData;
    if (!serialize(mergeCopyOf(local, key), &localData) || !serialize(mergeCopyOf(remote, key), &remoteData)
        || (base && !serialize(mergeCopyOf(base, key), &baseData))) {
        return {};
    }

    QSharedPointer<Database> mergedLocal;
    QSharedPointer<Database> mergedRemote;
    Merger::ChangeList changes;
    try {
        auto result = ov::merge_kdbx(slice(localData),
                                      slice(components),
                                      slice(remoteData),
                                      slice(components),
                                      slice(baseData),
                                      slice(components),
                                      Clock::currentSecondsSinceEpoch());
        if (result.local_changed) {
            mergedLocal = deserialize(toByteArray(result.local), key);
        }
        if (result.remote_changed) {
            mergedRemote = deserialize(toByteArray(result.remote), key);
        }
        if ((result.local_changed && !mergedLocal) || (result.remote_changed && !mergedRemote)) {
            return {};
        }
        if (mergedLocal) {
            applyMerged(local, mergedLocal.data());
        }
        if (mergedRemote) {
            applyMerged(remote, mergedRemote.data());
        }
        for (const auto& change : result.changes) {
            const auto type = changeType(change.change);
            const auto details = QString::fromUtf8(change.details.data(), static_cast<int>(change.details.size()));
            const auto uuid = change.uuid.size() == 16 ? QUuid::fromRfc4122(toByteArray(change.uuid))
                                                        : QUuid();
            if (change.subject == ov::ChangeSubject::Entry) {
                if (const auto* entry = local->rootGroup()->findEntryByUuid(uuid)) {
                    changes << Merger::Change(type, *entry, details);
                    continue;
                }
            } else if (change.subject == ov::ChangeSubject::Group) {
                if (const auto* group = local->rootGroup()->findGroupByUuid(uuid)) {
                    changes << Merger::Change(type, *group, details);
                    continue;
                }
            }
            changes << Merger::Change(type, details);
        }
    } catch (const rust::Error& e) {
        qWarning("SyncEngine: the core merge failed: %s", e.what());
        return {};
    }
    if (ok) {
        *ok = true;
    }
    if (remoteChanged) {
        *remoteChanged = !mergedRemote.isNull();
    }
    return changes;
}

Merger::ChangeList SyncEngine::mergeHere(Database* local, Database* remote, const Database* base)
{
    Merger::ChangeList changes;
    // Before combineEntries and Merger copy one entry's location over the other's
    const auto locations = locationsOf(local, remote);
    if (base) {
        for (auto* localEntry : local->rootGroup()->entriesRecursive()) {
            auto* remoteEntry = remote->rootGroup()->findEntryByUuid(localEntry->uuid());
            const auto* baseEntry = base->rootGroup()->findEntryByUuid(localEntry->uuid());
            if (!remoteEntry || !baseEntry) {
                continue;
            }
            if (sameFields(localEntry, remoteEntry)) {
                continue;
            }
            // Compared by content with the base, so neither clocks nor same-second edits can mislead
            const bool localChanged = !sameFields(localEntry, baseEntry);
            const bool remoteChanged = !sameFields(remoteEntry, baseEntry);
            const auto localTime = seconds(localEntry);
            const auto remoteTime = seconds(remoteEntry);
            // Merger keeps the newer entry: right when only one side changed and it is strictly newer
            const bool needed = (localChanged && remoteChanged) || (localChanged && localTime <= remoteTime)
                                || (remoteChanged && remoteTime <= localTime);
            if (needed) {
                combineEntries(localEntry, remoteEntry, baseEntry);
                changes << Merger::Change(
                    Merger::Change::Type::Modified, *localEntry, tr("Combined the changes made on both devices"));
            }
        }
    }

    // Found before Merger brings the deleted items back from the other side
    QHash<QUuid, DeletedObject> pending;
    if (base) {
        pending = pendingDeletions(local, remote);
        pending.insert(pendingDeletions(remote, local));
    }

    Merger firstMerge(local, remote);
    Merger secondMerge(remote, local);
    changes += firstMerge.merge() + secondMerge.merge();
    changes += applyLocations(local, remote, locations);
    if (!pending.isEmpty()) {
        changes += applyDeletions(local, remote, base, pending);
    }
    changes += mergeRecycleBins(local, remote);
    if (!changes.isEmpty()) {
        const auto generation = QString::number(qMax(generationOf(local), generationOf(remote)) + 1);
        local->metadata()->customData()->set(CustomData::SyncGeneration, generation);
        remote->metadata()->customData()->set(CustomData::SyncGeneration, generation);
    }
    return changes;
}

QHash<QUuid, QDateTime> SyncEngine::snapshotOf(const Database* db)
{
    QHash<QUuid, QDateTime> snapshot;
    for (const auto* entry : db->rootGroup()->entriesRecursive()) {
        // With milliseconds where this device has them, to see edits made in the same second as a sync
        snapshot.insert(entry->uuid(), entry->timeInfo().lastModificationTime().toUTC());
    }
    return snapshot;
}

QList<SyncConflict>
SyncEngine::findConflicts(const Database* local, const Database* remote, const QHash<QUuid, QDateTime>& snapshot)
{
    QList<SyncConflict> conflicts;
    const auto changedSince = [](const Entry* entry, const QDateTime& base) {
        return entry->timeInfo().lastModificationTime().toSecsSinceEpoch() != base.toSecsSinceEpoch();
    };
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        const auto* localEntry = local->rootGroup()->findEntryByUuid(it.key());
        const auto* remoteEntry = remote->rootGroup()->findEntryByUuid(it.key());
        SyncConflict conflict;
        conflict.uuid = it.key();
        if (localEntry && remoteEntry) {
            const auto changes = changesSince(localEntry, remoteEntry, it.value());
            if (!changes.local || !changes.remote || sameFields(localEntry, remoteEntry)) {
                continue;
            }
            conflict.kind = SyncConflict::Kind::EditedOnBoth;
            conflict.title = localEntry->title();
        } else if (remoteEntry && local->containsDeletedObject(it.key()) && changedSince(remoteEntry, it.value())) {
            conflict.kind = SyncConflict::Kind::DeletedHereEditedThere;
            conflict.title = remoteEntry->title();
        } else if (localEntry && remote->containsDeletedObject(it.key()) && changedSince(localEntry, it.value())) {
            conflict.kind = SyncConflict::Kind::EditedHereDeletedThere;
            conflict.title = localEntry->title();
        } else {
            continue;
        }
        conflicts << conflict;
    }
    return conflicts;
}

void SyncEngine::fail(const QString& error, bool networkError)
{
    cleanup();
    if (networkError && m_retryOnNetworkError) {
        m_backoffMsec = m_backoffMsec == 0 ? FirstBackoffMsec : qMin(m_backoffMsec * 2, MaxBackoffMsec);
        m_backoffTimer.start(m_backoffMsec);
        setState(State::Backoff);
    } else {
        m_backoffMsec = 0;
        setState(State::Idle);
    }
    emit finished(false, error);
}

void SyncEngine::cleanup()
{
    // Databases still being read belong to the sync that is ending
    ++m_openRun;
    m_remoteDb.reset();
    m_baseDb.reset();
    m_baseOpened = false;
    m_workDir.reset();
    m_remotePath.clear();
    m_remoteEtag.clear();
    m_remoteRevision = -1;
    m_remoteGeneration = 0;
    m_uploadData.clear();
    m_coreCancel = {};
    m_coreSent.clear();
    m_coreResult.reset();
    m_coreSentKey.reset();
}
