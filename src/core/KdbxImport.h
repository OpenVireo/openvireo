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

#ifndef KEEPASSXC_KDBXIMPORT_H
#define KEEPASSXC_KDBXIMPORT_H

#include <QSharedPointer>
#include <QString>

class Database;
class Group;

/**
 * Importing a .kdbx into an open database, as the web app does (@openvireo/sdk/merge): a UUID merge, or
 * copies with fresh UUIDs under a group. `source` is a database read only for the import: its recycle
 * bin is removed first, so neither way brings it along. Database custom data (sync remotes, browser
 * keys…) stays behind, and the target's root group (an account vault's name) isn't changed.
 */
namespace KdbxImport
{
    struct Summary
    {
        // Entries new to the target
        int added = 0;
        // Entries the target had that the import changed (newer version, history or location)
        int updated = 0;
        // Entries the target already had as they are
        int unchanged = 0;
        int groupsAdded = 0;
    };

    // Reads a .kdbx into memory; the database gets no file path, so nothing can ever write to the file
    QSharedPointer<Database> read(const QString& filePath, const QString& password, const QString& keyFile, QString* error);

    // Keep-newer merge by UUID that deletes nothing, so importing the same file twice changes nothing
    Summary mergeInto(Database* source, Database* target);

    // The source's tree as one group under `parent`, named after its root group, with fresh UUIDs for every
    // entry and group; field references between the copied entries follow them
    Summary addAsCopies(Database* source, Group* parent);

    // "Imported into "Work": 12 new entries, 3 updated, 40 unchanged."
    QString summaryText(const QString& into, bool copies, const Summary& summary);
} // namespace KdbxImport

#endif // KEEPASSXC_KDBXIMPORT_H
