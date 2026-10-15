# Automatic Sync with the Sync Server — Plan

**Status:** Phases 1–5 implemented on `feature/auto-sync` (see 5.6); phase 6 (three-way merge) remains; builds on the manual Remote Sync and `sync_server` from `feature/sync-server`
**Branch target:** `develop`
**Last updated:** 2026-10-14

## 1. Goal

Sync automatically with the KeePassXC Sync Server, without the user picking **Database → Remote Sync**, while keeping
the existing guarantees: the server only ever stores encrypted `.kdbx` files, merges happen on the client, and no device
silently overwrites another's changes.

The design stays whole-file: the server stores complete encrypted KDBX files, and the client syncs them over REST. Per-entry
sync is out of scope.

## 2. Already in place

| Requirement | Where |
|---|---|
| Server never decrypts the database | `sync_server` stores bytes and checks only the KDBX signature (`415` otherwise) |
| Conditional uploads (`If-Match` / `If-None-Match`, `412`, `428`) | `src/gui/remote/RemoteHandler.cpp:316-327`, `sync_server/src/SyncApi.cpp` |
| Atomic check-and-commit on the server | `sync_server/src/DatabaseStore.cpp:119-145` (a lock per database, temp file + rename) |
| Encrypted version history | `data/databases/<user>/.history/<id>/<rev>.kdbx`, bounded by `SYNC_HISTORY_SIZE` |
| Client-side merge | `DatabaseWidget::syncWithDatabase` (`src/gui/DatabaseWidget.cpp:1899`), two-way `Merger` |
| No upload when nothing changed | `RemoteHandler.cpp:306` compares the SHA-256 ETag. Works because the downloaded copy is only re-saved when the merge changed something. |
| Simultaneous uploads | `ConcurrentConditionalWritesExactlyOneWins` in `sync_server/test/ApiTest.cpp` |

## 3. Gaps

### 3.1 Nothing triggers a sync automatically

`DatabaseWidget::syncWithRemote` (`DatabaseWidget.cpp:1564`) only runs from the menu.

- Trigger it after `Database::databaseSaved` (connected at `DatabaseWidget.cpp:1768`) with a short debounce, after the
  database is unlocked, and on a periodic timer.
- For the timer, poll with `HEAD` + `If-None-Match`. The server already returns `304`, so a poll is nearly free.
- Save the last-synced ETag per database in **local config** (like `DatabaseWidgetStateSync`), keyed by database UUID.
  **Do not store it in the KDBX custom data.** Every sync would then change the file and its hash, the other devices would
  see a change and sync again, and the cycle would never end.
- Don't start a sync when the save came from a merge done by sync itself.

### 3.2 Sync blocks the window

`syncWithRemote` calls `setDisabled(true)`, and `RemoteHandler` waits for each request with
`AsyncTask::runAndWaitForFuture`. That's acceptable for a menu action but not for a sync after every save.

- Turn the sync into a step-by-step process that doesn't block the window.
- Hold off while an entry editor is open or the database is locked.
- Never show a dialog in the background. Today, if the server copy uses a different key, `syncDatabaseWithLockedDatabase`
  shows an unlock dialog. In the background, this should become a "sync needs attention" indicator the user can act on.

### 3.3 A `412` doesn't retry

`uploadToSyncServer` gives up with "sync again to merge them". In `syncWithRemote`, loop
download → merge → upload a limited number of times (3–5) when the upload gets a `412`, then fall back to retrying later.

### 3.4 No protection against a server serving an old copy

Encryption alone doesn't stop a server from returning an older, still valid database.

- **Local check:** the server already sends `X-Database-Revision`. Remember the highest revision seen and refuse to merge
  a download with a lower one.
- **Check inside the database:** to also catch a server that lies about the revision number, keep a counter in the KDBX
  custom data that only goes up when there are real changes. It's then covered by the KDBX integrity check, and a merge keeps
  the higher value. Because it only changes when there are real changes, it doesn't cause the cycle described in 3.1.

### 3.5 Access tokens are per user and synced

`serverToken` is part of `RemoteSettings`, which is stored in the database's custom data (`RemoteSettings.cpp:80,94`).
Every device therefore shares one token, the token travels inside the synced file, and one device can't be revoked on its
own.

- Server: change `UserStore` from users → tokens to users → devices → tokens. Add device register, list and revoke commands
  (and later endpoints).
- Client: keep the token in the OS keychain or local config, not in the KDBX.

### 3.6 Merges are decided per entry, by timestamp

`Merger` keeps whichever version of an entry was modified last and moves the other into the entry's history. Nothing is
lost, but the user isn't told about the conflict, and the result depends on device clocks being right.

- Keep a local copy of the last-synced KDBX to use as the common base.
- First, detect when both sides changed the same entry since the base, and point the user to that entry's history.
- Later, use the base for a real field-by-field (three-way) merge.

### 3.7 Push notifications and history endpoints

- **Events:** add `/api/v1/events` as a Drogon WebSocket that announces `{id, etag}` after each commit on the server. Not
  urgent, because polling with `304` is cheap.
- **History:** add `GET /api/v1/databases/{id}/history` and a way to download one revision. On the client, restore a version
  by reusing the import wizard's "Temporary Database" path (open it, then merge or copy entries from it).

## 4. Edge cases to test

- An entry deleted on one device while it's edited on another.
- Attachments changed on two devices at once.
- Wrong clocks on a device.
- Two uploads arriving at the same moment (already covered on the server).
- A device coming back after months offline.
- The server returning an older valid database (3.4).
- A crash or lost connection partway through an upload.
- KeePassXC's own atomic save replacing the file (so the file watcher doesn't start a sync on the app's own writes).

## 5. Implementation

### 5.1 Client architecture

```
DatabaseWidget ──owns──▶ SyncEngine ──uses──▶ SyncServerClient ──▶ sync_server (REST)
      ▲                    │  │
      │ merge + save       │  └──▶ SyncStateStore   (local, per database UUID)
      └────────────────────┘
VaultSyncChip ◀── stateChanged ── SyncEngine
```

New code goes in `src/gui/remote/` next to `RemoteHandler`.

| Component | Responsibility |
|---|---|
| `SyncServerClient` | The sync server's HTTP calls, moved out of `RemoteHandler.cpp` and made **asynchronous** (`QNetworkReply` + signals, no `runAndWaitForFuture`). Methods: `check(etag)` (HEAD + `If-None-Match`), `download()`, `upload(data, etag)`, later `history()` / `downloadRevision()` / `events()`. Returns status, ETag, `X-Database-Revision`. `RemoteHandler` keeps the command-based remotes and calls this client for the manual sync, so there's one HTTP code path. |
| `SyncEngine` | One per `DatabaseWidget` that has a sync server remote with auto-sync turned on. Holds the state machine, the triggers, retries and backoff. Opening the downloaded KDBX, the merge and the save run on the GUI thread, like unlocking does. `Database` isn't thread-safe (its constructor registers in a static map, and `open()` starts a file watcher), so moving the KDF step off the GUI thread is a separate change. |
| `SyncStateStore` | Local, per-device sync state keyed by database UUID, in `AppDataLocation/sync/<uuid>.json`: last-synced ETag, highest server revision, highest sync counter (generation), last sync time, and later the base snapshot. **Never stored in the KDBX** (see 3.1). |

#### State machine

```
Idle ─trigger─▶ Checking ─304─▶ Idle
                   │ changed / local dirty
                   ▼
               Downloading ─▶ Merging (open + merge + save) ─▶ Uploading ─200/201─▶ Idle
                   ▲                                                          │ 412
                   └──────────────── retry ≤ 3 ───────────────────────────────┘
Any step ─network error─▶ Backoff (30 s → 15 min) ─▶ Checking
Merging ─wrong key─▶ WaitingForUnlock (manual sync: the existing unlock dialog; background sync, phase 2: NeedsAttention)
Downloading ─revision/generation lower than seen─▶ NeedsAttention (possible rollback)
```

- "Local dirty" means the local file was saved since the last successful sync (the database's last-modified time is later
  than the stored last sync time).
- If the user is editing an entry when a merge is due, the engine keeps the downloaded file and waits until the editor
  closes.
- With unsaved changes in memory, the engine waits for the next save instead of merging into an unsaved database.

#### Triggers

| Trigger | Hook |
|---|---|
| After a save | `Database::databaseSaved` (`DatabaseWidget.cpp:1768`), debounced about 3 s. Ignored while the engine is saving its own merge. |
| After unlock | `DatabaseWidget::databaseUnlocked` |
| Periodic poll | `QTimer`, 60 s while unlocked (only a HEAD; the server returns `304` when nothing changed) |
| Back online / app comes to the front | `QNetworkInformation::reachabilityChanged`, `QGuiApplication::applicationStateChanged` |
| Push from the server (phase 5) | The events stream replaces most polling; keep a 5-minute poll as a fallback |

#### Settings and UI

- `RemoteParams` gets `bool autoSync`, saved by `RemoteSettings`, with a checkbox in `DatabaseSettingsWidgetRemote`. Only
  sync server remotes can use it, and only one per database.
- `VaultSyncChip` already shows Idle/Syncing/Synced/Failed. Add `NeedsAttention` and `Offline`, and have it follow
  `SyncEngine::stateChanged` instead of the manual-sync signals.
- A background sync must not emit `databaseSyncInProgress`, which disables the menu and toolbar (`MainWindow.cpp:694`).
  Background syncs only drive the chip; a manual sync keeps today's behaviour.

### 5.2 Rollback protection (3.4)

1. **Server revision:** store the highest `X-Database-Revision` seen in `SyncStateStore`. A download with a lower revision
   goes to `NeedsAttention` and isn't merged.
2. **Sync counter (generation) inside the KDBX:** a custom data key, for example `KPXC_SYNC_GENERATION`, covered by the
   KDBX integrity check. On a sync whose merge changed something, set it to `max(local, remote) + 1` before saving and
   uploading. A downloaded database whose generation is lower than the highest one seen is refused. `Merger` merges custom
   data as one block, keeping the most recently modified side (`Merger.cpp:720-731`), so the engine must set the max
   explicitly after merging and not rely on the merge.

### 5.3 Conflict detection (3.6)

- **First step (metadata only):** after each successful sync, save `{entry UUID → last-modified time}` in `SyncStateStore`
  (no titles or secrets). On the next merge, an entry changed both locally and remotely since that snapshot is a conflict.
  After the merge, show a message that lists those entries and links to each one's history.
- **Later (three-way merge):** keep the last-synced KDBX as `sync/<uuid>.base.kdbx`. It's encrypted with the same key, so
  opening it costs one more KDF. Use it as the common ancestor so changes to different fields both survive, and clock skew
  no longer matters for those entries.

### 5.4 Per-device tokens (3.5)

Server (`UserStore`):

- Schema: `users.<name>.devices.<deviceId> = { name, tokenHash, createdAt, lastSeenAt }`. The existing `tokenHash` is
  migrated into a device called `legacy`.
- CLI: `devices add <user> <device-name>`, `devices list <user>`, `devices revoke <user> <device-id>`.
- Later, pairing: an authenticated device asks `POST /api/v1/devices/pairing` for a short-lived code, and the new device
  swaps it for its own token. This makes adding a device a one-step process.

Client:

- Move `serverToken` out of `RemoteSettings`, which is stored in the KDBX, into device-local secure storage keyed by
  database UUID and remote name. On macOS, use the Keychain (`Security.framework` is already linked for Touch ID). On other
  platforms, start with local config and add Windows Credential Manager or libsecret later.
- On first load, move an existing token out of the KDBX into local storage and remove it from the KDBX.

### 5.5 Server additions (3.7)

| Endpoint | Notes |
|---|---|
| `GET /api/v1/databases/{id}/history` | `[{ revision, etag, size, updatedAt }]` from `.history/<id>/`. Keep a small metadata file per revision so the ETag isn't recomputed each time. |
| `GET /api/v1/databases/{id}/history/{revision}` | Raw KDBX of that revision |
| `GET /api/v1/events` | Server-sent events (one long HTTP response that streams messages): `{ id, etag, revision }` after each commit for that user. SSE instead of WebSocket because it needs no new Qt module (`QNetworkReply::readyRead`) and works through ordinary reverse proxies. In Drogon: a streaming response plus a per-user broadcaster that `DatabaseStore::put` notifies after the rename. |

Client history UI: a list under **Remote Sync**. Restoring a revision downloads it and opens it as a temporary database,
reusing the import wizard's path, so the user can merge it or copy entries from it.

### 5.6 Phases

Each phase is one PR, and the feature stays off unless `autoSync` is turned on for a remote.

| # | Scope | Server changes | Main files |
|---|---|---|---|
| 1 ✅ | `SyncServerClient` (async), `SyncStateStore`, `SyncEngine` state machine with retry on `412` and backoff; the manual sync goes through it | none | `RemoteHandler.*`, new `SyncServerClient.*`, `SyncEngine.*`, `SyncStateStore.*`, `DatabaseWidget.cpp` |
| 2 ✅ | Triggers, the `autoSync` setting, chip states, no menu disabling for background syncs | none | `RemoteSettings.*`, `DatabaseSettingsWidgetRemote.*`, `VaultPanes.*`, `MainWindow.cpp` |
| 3 ✅ | Rollback checks (revision + generation), conflict detection from the metadata snapshot | none | `SyncEngine.*`, `SyncStateStore.*` |
| 4 ✅ | Per-device tokens, migration, token moved out of the KDBX | `UserStore`, CLI | `UserStore.*`, `main.cpp`, `RemoteSettings.*`, new token store |
| 5 ✅ | History endpoints and UI, SSE events | `SyncApi`, `DatabaseStore` | `SyncApi.cpp`, `DatabaseStore.*`, `SyncServerClient.*`, `SyncEngine.*` |
| 6 | Three-way merge from the base snapshot | none | `Merger.*`, `SyncEngine.*` |

#### Phase 1 notes

- Manual syncs with a sync server remote go through `SyncEngine` (`DatabaseWidget::syncWithServer`). Command-based remotes
  keep the old `RemoteHandler` path. `RemoteHandler`'s sync server functions (import wizard, Test Connection) now wrap
  `SyncServerClient`, so there is one HTTP code path.
- Sync state lives in `sync-state.json` next to the local config (`AppLocalDataLocation`, or the portable config folder),
  keyed by a SHA-256 of the local file path, server URL and database ID.
- Retrying after network errors with a growing delay is built and tested but turned off (`setRetryOnNetworkError`) until
  phase 2 adds automatic syncs.
- A sync still disables the database widget and the menus while it runs, as before; phase 2 changes that for background
  syncs.
- Tests: `tests/TestSyncEngine.cpp` (fake server), and `testSyncEngineTwoDevices` in `tests/TestRemoteSyncServer.cpp`
  (live server).

#### Phase 2 notes

- `SyncScheduler` (`src/gui/remote/`) emits `syncRequested()` 3 s after the last save, every 60 s, when the network comes
  back (`QNetworkInformation`) and when the app comes to the front (at most once per 10 s for those two). Enabling it,
  which happens on unlock or when auto-sync is turned on, syncs at once.
- `DatabaseWidget` only starts a background sync when it is showing the database (no editor or settings open), has no
  unsaved changes, and isn't saving or reloading; otherwise it remembers the sync and runs it once the user is done.
  If the user opens an editor while a sync downloads, the engine's merge gate holds the downloaded copy (`WaitingToMerge`)
  until the editor closes.
- `Database` emits `databaseSaved` while it still holds its save lock (`isSaving()` is true), so the post-save check runs
  from the event loop.
- Background syncs never show dialogs, messages, progress or disable the window. A server copy with a different key
  leaves the engine in `WaitingForUnlock` and the chip at "unlock needed"; clicking it opens the unlock dialog. Network
  errors show "offline" and retry with backoff.
- The chip reads `DatabaseWidget::syncStatus()` instead of keeping its own state. Polls that find nothing new (HEAD → 304)
  don't show "Syncing…".
- `autoSync` is part of the remote settings, which live in the KDBX and sync to every device. Turning it on therefore
  turns it on everywhere; a per-device override could come with the per-device tokens of phase 4.
- `testgui` and `testguivault` crash during setup on this branch, before any sync work, so phase 2 has its own GUI test,
  `tests/gui/TestGuiAutoSync.cpp` (needs `WITH_GUI_TESTS=ON` and a live server, like `testremotesyncserver`).

#### Phase 3 notes

- The engine stops in `NeedsAttention`, before merging or uploading, when:
  - the server's `X-Database-Revision` is lower than the highest this device has seen;
  - the downloaded copy's sync generation (`KPXC_SYNC_GENERATION` in the database's custom data) is lower than the
    highest seen, even if the revision went up;
  - the server returns 404 for a database this device has synced before (deleted, or a server reset). Not in the
    original plan, but quietly re-uploading would hide a server that drops data.
- Background syncs skip a database in `NeedsAttention`; the chip shows "check server". Clicking it asks the user, and
  agreeing runs `syncAcceptingServerState()`, which merges as usual and takes the server's state as the new baseline.
- `SyncEngine::mergeDatabases()` is now the one two-way merge, used by the engine and by `DatabaseWidget` for command
  remotes and the unlock-dialog path. When the merge changes anything, both copies get `max(generations) + 1`; when it
  changes nothing, neither is touched, so devices don't keep uploading to each other.
- The generation key is "auto-generated" for `Merger` (like `KPXC_RANDOM_SLUG`) and protected from removal. Otherwise
  `Merger` copies it with the rest of the custom data, and two devices keep changing it for each other forever;
  `testTwoDevicesSettle` and `testMergerIgnoresTheGeneration` fail without this.
- Conflicts compare each entry's modification time, to the second (KDBX stores whole seconds), with the snapshot taken
  after the previous sync. Edited on both sides, or deleted on one and edited on the other, is reported in a message
  bar that stays until closed. The first sync of a database has no snapshot, so it reports nothing.
- Not covered: when the server copy needs the unlock dialog (different key), its generation isn't checked, because the
  engine never opens it.

#### Phase 4 notes

- Server: `users.json` keeps devices per user, each with its own token hash, creation time and last-seen time (written
  at most once an hour per device). A user's `tokenHash` from before devices is read as a device called `legacy`, so
  existing tokens keep working until revoked. CLI: `users add <name> [device]`, `devices add|list|revoke`. Adding an
  existing user is an error now instead of rotating its token. Pairing by code is left for later.
- Client: `SyncTokenStore` keeps tokens in the login Keychain on macOS (generic passwords, service "KeePassXC Sync",
  never synced through iCloud) and, elsewhere for now, in `sync-tokens.json` (mode 0600) next to the local config.
  That fallback is plain text on disk, where the token used to be inside the encrypted database; Windows Credential
  Manager and libsecret are the follow-ups. Tests use an in-memory store; `KPXC_TEST_KEYCHAIN=1` runs one test against
  the real store.
- Tokens are keyed by the database's root group UUID, server URL and database ID. `RemoteSettings` no longer writes
  `serverToken` into the database. A token found there is copied to the store on load and dropped from the database
  the next time the remote settings are saved; a token the device already has wins.
- A sync server remote without a token on this device shows "token needed"; background syncs skip it without sending
  anything, and a manual sync opens the Remote Sync settings.

#### Phase 5 notes

- History: `GET …/{id}/history` and `GET …/{id}/history/{revision}`, routed inside the existing `databases/(.+)`
  handler rather than as separate routes, so route order doesn't matter. Each kept version now has a `<revision>.json`
  with its metadata; versions kept before that get theirs from the file.
- Events: server-sent events, as planned, through Drogon's `newAsyncStreamResponse`. An `EventHub` keeps each user's
  open streams, publishes after every upload and delete, and pings every 25 s, dropping streams that fail to send.
- Client: `SyncEventStream` runs while automatic sync is on. It reconnects with a growing delay (5 s up to 5 min), and
  after 60 s without even a ping. An event for our database whose ETag isn't the one we last synced requests a sync at
  once; our own uploads come back as events too and are skipped that way. While connected the poll slows to 5 minutes.
- History UI: **Database → Remote Sync → Versions on <name>…** opens `SyncHistoryDialog`, a plain `QDialog` for now
  rather than a Fluent modal. Opening a version downloads it to its own folder under the system temp directory (left
  for the OS to clean up, because the tab keeps using the file) and opens it in a new tab, locked, through
  `requestOpenDatabase`.
- `SyncEventStream`'s destructor blocks its signals: it is usually deleted as `DatabaseWidget`'s child, and a final
  `connectedChanged(false)` called back into the half-destroyed widget (found by `testguiautosync`).

### 5.7 Testing

- **Unit:** `SyncEngine` against a fake `SyncServerClient` that scripts responses: `304`, `412` then success, network
  error then backoff, lower revision → `NeedsAttention`, wrong key → `NeedsAttention`, a save during a sync.
- **Integration:** extend `tests/TestRemoteSyncServer.cpp`, which runs against a real server when
  `KPXC_SYNC_SERVER_URL` / `KPXC_SYNC_SERVER_TOKEN` are set. Cover two `Database` instances syncing the same ID with
  interleaved edits, a delete on one side with an edit on the other, and attachments.
- **Server:** extend `sync_server/test/ApiTest.cpp` for history, device revocation and events.
- **Manual:** two machines, one going offline for a while; kill the app during an upload; restore an old file on the server
  by hand (must be detected as a rollback).
