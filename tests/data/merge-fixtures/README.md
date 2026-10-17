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
  "entries": [{
    "uuid": "a1000000-0000-4000-8000-000000000001",
    "modified": "2026-03-01T11:00:00Z",          // lastModificationTime, whole seconds, UTC
    "attributes": { "Title": "Bank", "Password": "…", "API key": "…" },  // standard and custom fields
    "protected": ["API key"],                     // optional: protected custom attributes
    "tags": "finance;shared",                     // optional
    "attachments": { "note.txt": "<base64>" }     // optional
  }],
  "deleted": [{ "uuid": "…", "time": "2026-03-01T11:00:00Z" }]   // deleted objects
}
```

All entries sit directly in the root group, whose UUID (`6e3a9f40-1c2b-4d5e-8f70-9a1b2c3d4e5f`) and
times (`2026-01-01T00:00:00Z`) are the same in every copy. Entry creation and location-changed times
are `2026-01-01T00:00:00Z`; the last access time equals `modified`. Fields not listed are empty
(no expiry, icon 0, no colors).

**Projection** (expected result): `entries` sorted by UUID, each with `uuid`, `modified`,
`attributes` (empty standard fields left out), `protected`, `tags` and `attachments` when not empty,
and `history_count`; `deleted` is the sorted list of deleted-object UUIDs (times left out).

Notes for a port:

- A combined entry (changed on both sides, or on one side with a clock that makes it look older) gets
  `max(now, local.modified, remote.modified)` as its modification time, and both copies keep their
  previous state in their history. Conflicting fields take the side with the newer `modified`
  (local wins a tie). Conflict names are the display names `SyncEngine` uses: `Title`, `Username`,
  `Password`, `URL`, `Notes`, a custom attribute's key, `attachment <name>`, `Tags`, `Expiry`, `Icon`,
  `Colors`.
- `history_count` comes from KeePassXC's `Merger` and is informative; a port may compare it loosely.
- Case 05 records current behaviour, not necessarily the intended one: `Merger` only applies
  deletions in the `Synchronize` merge mode, which sync doesn't set, so an entry deleted on one side
  and unchanged on the other comes back (and the deletion record stays).

Regenerate the expected results after an intended change to the merge with
`KPXC_UPDATE_MERGE_FIXTURES=1 ./tests/testmergefixtures`, then review the diff.
