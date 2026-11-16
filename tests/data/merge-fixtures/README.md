# Merge fixtures

Three-way merge cases for `SyncEngine::mergeDatabases()` (`src/gui/remote/SyncEngine.cpp`, which runs
`resolveFields`/`combineEntries` and then `Merger` both ways). They let the TS SDK port of the merge
check that it resolves conflicts the same way as Qt. `tests/TestMergeFixtures.cpp` checks every file
here with the C++ merge (`ctest -R testmergefixtures`).

Each `*.json` file is one case. Everything is deterministic: UUIDs, times and the clock are fixed.

```jsonc
{
  "description": "what the case shows",
  "now": "2026-03-01T12:00:00Z",          // the clock during the merge
  "base":   Database | null,              // the copy both sides last synced from; null = no base
  "local":  Database,
  "remote": Database,
  "expected": {
    "conflicts": { "<entry uuid>": ["Password"] },   // fields both sides changed differently (before merging)
    "result": Projection                             // the merged database; local and remote end up equal
  }
}
```

**Database** (input):

```jsonc
{
  "groups": [{                                    // optional: groups besides the root, in any order
    "uuid": "c3000000-0000-4000-8000-000000000003",
    "name": "Work",
    "parent": "…",                                // optional, default the root group's UUID
    "modified": "2026-03-01T10:05:00Z",           // optional, default 2026-01-01T00:00:00Z
    "location_changed": "2026-03-01T10:00:00Z"    // optional, default 2026-01-01T00:00:00Z
  }],
  "entries": [{
    "uuid": "a1000000-0000-4000-8000-000000000001",
    "modified": "2026-03-01T11:00:00Z",          // lastModificationTime, whole seconds, UTC
    "attributes": { "Title": "Bank", "Password": "…", "API key": "…" },  // standard and custom fields
    "protected": ["API key"],                     // optional: protected custom attributes
    "tags": "finance;shared",                     // optional
    "attachments": { "note.txt": "<base64>" },    // optional
    "group": "c3000000-0000-4000-8000-000000000003",  // optional, default the root group's UUID
    "location_changed": "2026-03-01T10:00:00Z"    // optional, default 2026-01-01T00:00:00Z
  }],
  "deleted": [{ "uuid": "…", "time": "2026-03-01T11:00:00Z" }]   // deleted objects
}
```

The root group's UUID (`6e3a9f40-1c2b-4d5e-8f70-9a1b2c3d4e5f`) and times (`2026-01-01T00:00:00Z`)
are the same in every copy. Entry creation times are `2026-01-01T00:00:00Z`; the last access time
equals `modified`. A group's other times are `2026-01-01T00:00:00Z`, and adding the entries doesn't
change them. Fields not listed are empty (no expiry, icon 0, no colors).

**Projection** (expected result): `entries` sorted by UUID, each with `uuid`, `modified`,
`attributes` (empty standard fields left out), `protected`, `tags` and `attachments` when not empty,
`group` and `location_changed` when not the default (the root, `2026-01-01T00:00:00Z`), and
`history_count`; `deleted` is the sorted list of deleted-object UUIDs (times left out). Only when the
database has groups besides the root, `groups` lists them sorted by UUID, each with `uuid`, `name`,
`parent` (the root's UUID for a top-level group), and `modified` and `location_changed` when not
`2026-01-01T00:00:00Z`. Cases 01-10 have no groups, so their projections have neither.

Notes for a port:

- A combined entry (changed on both sides, or on one side with a clock that makes it look older) gets
  `max(now, local.modified, remote.modified)` as its modification time, and both copies keep their
  previous state in their history. Conflicting fields take the side with the newer `modified`
  (local wins a tie). Conflict names are the display names `SyncEngine` uses: `Title`, `Username`,
  `Password`, `URL`, `Notes`, a custom attribute's key, `attachment <name>`, `Tags`, `Expiry`, `Icon`,
  `Colors`.
- An entry both sides have ends up, on both, in the group of the copy with the newer
  `location_changed` (whole seconds; local wins a tie) and with that copy's `location_changed`,
  whichever copy wins on the fields and modification time. Don't copy the location along with the
  fields when one copy replaces the other, or a move made on one device and an edit made on the other
  leave the copies in different groups (cases 11-13). The root groups count as the same group even if
  their UUIDs differ. The Recycle Bin is a group like any other here (recycling is a move); deleting
  for good still follows the deletion rules below.
- Groups don't need that rule in Qt: `Merger` moves a group by its own `location_changed` and updates
  its name separately, without replacing one copy with the other. A port must keep those apart too
  (case 14).
- `history_count` comes from KeePassXC's `Merger` and is informative; a port may compare it loosely.
- With a base, deletions are decided against it after `Merger` has run both ways (`Merger` itself only
  applies deletions in the `Synchronize` merge mode, which sync doesn't set): an entry deleted on one
  side goes from both only when both copies still match the base (same fields, same group). Otherwise
  the deletion record is dropped and the entry stays. Deletion and modification times play no part, so
  a wrong clock can't decide (cases 05, 09, 10). Without a base nothing is deleted.

Regenerate the expected results after an intended change to the merge with
`KPXC_UPDATE_MERGE_FIXTURES=1 ./tests/testmergefixtures`, then review the diff.
