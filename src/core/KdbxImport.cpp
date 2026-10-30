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

#include "KdbxImport.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Merger.h"
#include "core/Metadata.h"
#include "format/KeePass2Reader.h"
#include "keys/FileKey.h"
#include "keys/PasswordKey.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace
{
    QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
    {
        return QCoreApplication::translate("KdbxImport", text, disambiguation, n);
    }

    void removeRecycleBin(Database* db)
    {
        if (auto* bin = db->metadata()->recycleBin()) {
            delete bin;
        }
    }

    QString hex(const QUuid& uuid)
    {
        return QString::fromLatin1(uuid.toRfc4122().toHex()).toUpper();
    }
} // namespace

namespace KdbxImport
{
    QSharedPointer<Database>
    read(const QString& filePath, const QString& password, const QString& keyFile, QString* error)
    {
        auto key = QSharedPointer<CompositeKey>::create();
        if (!password.isEmpty()) {
            key->addKey(QSharedPointer<PasswordKey>::create(password));
        }
        if (!keyFile.isEmpty()) {
            auto fileKey = QSharedPointer<FileKey>::create();
            QString keyError;
            if (!fileKey->load(keyFile, &keyError)) {
                *error = tr("Could not load the key file: %1").arg(keyError);
                return {};
            }
            key->addKey(fileKey);
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = tr("Could not open %1: %2").arg(filePath, file.errorString());
            return {};
        }
        auto db = QSharedPointer<Database>::create();
        KeePass2Reader reader;
        if (!reader.readDatabase(&file, key, db.data())) {
            *error = tr("Wrong password or key file, or not a KeePass 2 database (%1). If the file is protected with "
                        "a hardware key, open it with File > Open and use Database > Merge From Database… instead.")
                         .arg(reader.errorString());
            return {};
        }
        return db;
    }

    Summary mergeInto(Database* source, Database* target)
    {
        removeRecycleBin(source);
        Merger merger(source, target);
        merger.setSkipDatabaseCustomData(true);
        // Never apply deletions, even when the target syncs with Synchronize
        merger.setForcedMergeMode(Group::KeepNewer);
        const auto changes = merger.merge();

        Summary summary;
        QSet<QUuid> added;
        QSet<QUuid> updated;
        for (const auto& change : changes) {
            const bool isEntry = target->rootGroup()->findEntryByUuid(change.uuid());
            if (change.type() == Merger::Change::Type::Added) {
                if (isEntry) {
                    added.insert(change.uuid());
                } else if (!change.uuid().isNull()) {
                    ++summary.groupsAdded;
                }
            } else if ((change.type() == Merger::Change::Type::Modified || change.type() == Merger::Change::Type::Moved)
                       && isEntry) {
                updated.insert(change.uuid());
            }
        }
        updated.subtract(added);
        summary.added = added.size();
        summary.updated = updated.size();
        summary.unchanged = qMax(0, source->rootGroup()->entriesRecursive().size() - summary.added - summary.updated);
        return summary;
    }

    Summary addAsCopies(Database* source, Group* parent)
    {
        removeRecycleBin(source);
        auto* sourceRoot = source->rootGroup();
        auto* copy = sourceRoot->clone(Entry::CloneNewUuid | Entry::CloneIncludeHistory,
                                       Group::CloneNewUuid | Group::CloneIncludeEntries);

        // clone() keeps the order, so the copies line up with their originals
        const auto originals = sourceRoot->entriesRecursive();
        const auto copies = copy->entriesRecursive();
        QHash<QString, QString> ids;
        for (int i = 0; i < originals.size() && i < copies.size(); ++i) {
            ids.insert(hex(originals[i]->uuid()), hex(copies[i]->uuid()));
        }

        QSet<QUuid> icons;
        for (const auto* group : copy->groupsRecursive(true)) {
            if (!group->iconUuid().isNull()) {
                icons.insert(group->iconUuid());
            }
        }
        static const QRegularExpression ref(QStringLiteral("(\\{REF:[TUPAN]@I:)([0-9A-Fa-f]{32})(\\})"));
        for (auto* entry : copies) {
            if (!entry->iconUuid().isNull()) {
                icons.insert(entry->iconUuid());
            }
            QList<Entry*> versions{entry};
            versions << entry->historyItems();
            for (auto* version : versions) {
                auto* attributes = version->attributes();
                for (const auto& key : attributes->keys()) {
                    const auto value = attributes->value(key);
                    if (!value.contains(QStringLiteral("{REF:"))) {
                        continue;
                    }
                    QString next;
                    int last = 0;
                    auto it = ref.globalMatch(value);
                    while (it.hasNext()) {
                        const auto m = it.next();
                        const auto to = ids.value(m.captured(2).toUpper());
                        next += value.mid(last, m.capturedStart() - last);
                        next += to.isEmpty() ? m.captured(0) : m.captured(1) + to + m.captured(3);
                        last = m.capturedEnd();
                    }
                    next += value.mid(last);
                    if (next != value) {
                        version->setUpdateTimeinfo(false);
                        attributes->set(key, next, attributes->isProtected(key));
                        version->setUpdateTimeinfo(true);
                    }
                }
            }
        }

        parent->database()->metadata()->copyCustomIcons(icons, source->metadata());
        copy->setParent(parent);

        Summary summary;
        summary.added = copies.size();
        summary.groupsAdded = copy->groupsRecursive(true).size();
        return summary;
    }

    QString summaryText(const QString& into, bool copies, const Summary& s)
    {
        if (copies) {
            return tr("Imported %n entries into \"%1\" as copies.", nullptr, s.added).arg(into);
        }
        if (s.added == 0 && s.updated == 0) {
            return tr("Nothing to import: \"%1\" already has everything in this file.").arg(into);
        }
        auto text = tr("%n new entries", nullptr, s.added) + ", " + tr("%n updated", nullptr, s.updated);
        if (s.unchanged > 0) {
            text += ", " + tr("%n unchanged", nullptr, s.unchanged);
        }
        return tr("Imported into \"%1\": %2.").arg(into, text);
    }
} // namespace KdbxImport
