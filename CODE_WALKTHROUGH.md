# KeePassXC Code Walkthrough

A guided tour of the KeePassXC source tree. It covers how the app starts, how a `.kdbx` file is decrypted into objects in memory, how the GUI sits on top of that, and where the integrations (browser, auto-type, SSH agent, Secret Service, CLI) connect.

> Paths are relative to the repository root. Line numbers are approximate and drift as the code changes.

---

## 1. The big picture

KeePassXC is a C++17 / Qt 6 application. Cryptography comes from **Botan** (plus **libsodium** for browser messaging). The build splits it into a few targets ([src/CMakeLists.txt](src/CMakeLists.txt)):

| Target | What it contains |
|---|---|
| `keepassxc_core` (static lib) | Data model, file formats, crypto, keys, streams, CLI helpers. No widgets. |
| `keepassxc_gui` (static lib) | Everything under `src/gui`, plus the GUI-facing parts of the integrations |
| `keepassxc` (executable) | [src/main.cpp](src/main.cpp), the desktop app |
| `keepassxc-cli` | [src/cli/keepassxc-cli.cpp](src/cli/keepassxc-cli.cpp), the command-line tool |
| `keepassxc-proxy` | [src/proxy/](src/proxy/), the native-messaging bridge between browser extensions and the app |

Optional features are switched on and off by CMake options in [CMakeLists.txt](CMakeLists.txt) (`KPXC_FEATURE_BROWSER`, `KPXC_FEATURE_SSHAGENT`, `KPXC_FEATURE_FDOSECRETS`, `KPXC_FEATURE_NETWORK`, `KPXC_MINIMAL`, …). In the code, these show up as `#ifdef WITH_XC_*` guards.

### Directory map

```
src/
├── main.cpp          GUI entry point
├── core/             Data model: Database, Group, Entry, Metadata, Config, Merger, generators, TOTP
├── format/           Readers/writers: KDBX 3/4, KeePass 1, CSV, HTML, importers (Bitwarden, 1Password, Proton Pass)
├── crypto/           Botan wrappers: Crypto init/self-test, hashes, ciphers, random, KDFs (AES-KDF, Argon2)
├── keys/             Key components: PasswordKey, FileKey, ChallengeResponseKey (YubiKey), CompositeKey
├── streams/          Layered QIODevices: cipher, HMAC block, hashed block, gzip
├── gui/              Qt Widgets UI (MainWindow, DatabaseWidget, editors, dialogs, reports, settings)
├── autotype/         Auto-Type engine + per-platform plugins (xcb, wayland, windows, mac)
├── browser/          Browser integration and passkeys (WebAuthn)
├── proxy/            keepassxc-proxy native messaging host
├── sshagent/         SSH agent integration (OpenSSH key parsing, agent protocol)
├── fdosecrets/       freedesktop.org Secret Service over D-Bus (Linux)
├── keeshare/         Sharing groups between databases via exported container files
├── quickunlock/      TouchID, Windows Hello and Polkit quick unlock
├── networking/       Network access manager (icon downloads, HIBP)
├── cli/              keepassxc-cli subcommands
└── thirdparty/       Vendored code
tests/                QtTest unit tests (tests/gui for GUI tests), test data in tests/data
docs/                 AsciiDoc user guide and man pages
share/                Icons, translations, wordlists, desktop files
```

---

## 2. Startup: `main()`

[src/main.cpp](src/main.cpp) runs these steps in order:

1. **Hardening and Qt setup.** On Windows it points the `OPENSSL_*` environment variables at an invalid path, so a planted `openssl.cnf` can't inject a DLL. It also sets the high-DPI attributes.
2. **`Application app(argc, argv)`** ([src/gui/Application.cpp](src/gui/Application.cpp)) is a `QApplication` subclass. It handles single-instance detection over a local socket, OS signals and theming.
3. **Command-line parsing** for `--config`, `--localconfig`, `--keyfile`, `--pw-stdin`, `--lock`, `--minimized`, `--allow-screencapture` and `--debug-info`.
4. **Single-instance handoff.** If another instance is running, the new process forwards the file names (or a lock request) to it and exits.
5. **`Crypto::init()`** ([src/crypto/Crypto.cpp](src/crypto/Crypto.cpp)) checks the Botan version and runs crypto self-tests. If they fail, the app shows a fatal error.
6. **`Application::bootstrap()`** ([src/core/Bootstrap.cpp](src/core/Bootstrap.cpp)) loads translations and applies early process hardening.
7. **`MainWindow mainWindow;`** creates the main window. Screen capture is blocked unless `--allow-screencapture` is set.
8. Each file name from the command line goes to `mainWindow.openDatabase(...)`. The app then enters `Application::exec()`. The exit code `RESTART_EXITCODE` makes the app relaunch itself.

---

## 3. The data model (`src/core`)

Everything the user edits is a tree of `QObject`s rooted in a `Database`:

```
Database
├── Metadata            name, description, recycle bin, custom icons, history limits, custom data
├── rootGroup : Group
│   ├── Group ...       (m_children)
│   └── Entry ...       (m_entries)
│       ├── EntryAttributes      Title/UserName/Password/URL/Notes + custom attributes (with "protected" flag)
│       ├── EntryAttachments     binary attachments
│       ├── AutoTypeAssociations window-title → sequence mappings
│       ├── CustomData           plugin data (browser, KeeShare, SSH agent settings…)
│       ├── TimeInfo             created/modified/accessed/expires
│       └── history : QList<Entry*>   older snapshots, oldest first
└── deletedObjects      UUID tombstones, used by merge/sync
```

Key files:

- [src/core/Database.h](src/core/Database.h) / [.cpp](src/core/Database.cpp): owns the root group and metadata, plus the crypto settings (cipher, KDF, compression). It also holds the **CompositeKey** and the **transformed key**. It handles `open()`, `save()`/`saveAs()`, the recycle bin, and file watching for external changes.
- [src/core/Group.h](src/core/Group.h) and [src/core/Entry.h](src/core/Entry.h) are tree nodes. An `Entry` can resolve field references such as `{REF:P@I:<uuid>}` (`Entry::resolveReference`), and [src/core/EntryPlaceholders.cpp](src/core/EntryPlaceholders.cpp) expands placeholders such as `{USERNAME}`.
- [src/core/ModifiableObject.h](src/core/ModifiableObject.h) is the base class that emits `modified()`. Changes propagate up the tree to `Database::markAsModified()`, which drives the "unsaved" asterisk and auto-save. Bulk operations call `setEmitModified(false)` so they don't fire a flood of signals. File loading does this, for example.
- [src/core/Merger.cpp](src/core/Merger.cpp) does a three-way-style merge of two databases using UUIDs, modification times and deleted-object tombstones. "Merge database", KeeShare and external-change sync all use it.
- [src/core/Config.h](src/core/Config.h) holds app settings as a `ConfigKey` enum. Values are read with `config()->get(Config::Key)`. There are separate roaming and local INI files.
- Utilities: `EntrySearcher` (search syntax), `PasswordGenerator` / `PassphraseGenerator`, `PasswordHealth` (zxcvbn scoring), `Totp`, `HibpOffline`, `FileWatcher` and `InactivityTimer`.

---

## 4. Opening a database: from bytes to objects

This is the most important path in the codebase. Here it is end to end.

### 4.1 The user enters credentials

[src/gui/DatabaseOpenWidget.cpp](src/gui/DatabaseOpenWidget.cpp), `openDatabase()`:

```cpp
const auto databaseKey = buildDatabaseKey();   // password + keyfile + YubiKey → CompositeKey
m_db.reset(new Database());
bool ok = m_db->open(m_filename, databaseKey, &error);
```

If quick unlock (TouchID / Windows Hello / Polkit) is available, the serialized key is stored after a successful open, so the next unlock can skip the password.

### 4.2 Building the key (`src/keys`)

- `PasswordKey`: SHA-256 of the password.
- `FileKey`: parses XML v1/v2 key files, 32/64-byte raw or hex files, or hashes any other file.
- `ChallengeResponseKey`: a hardware token (YubiKey over USB or PC/SC, see [src/keys/drivers/](src/keys/drivers/)).
- `CompositeKey` ([src/keys/CompositeKey.cpp](src/keys/CompositeKey.cpp)) combines them:

```cpp
// rawKey = SHA256( key1.rawKey || key2.rawKey || ... || challengeResponse(seed) )
// transform() then runs the KDF (Argon2d/id or AES-KDF) over rawKey
```

KDBX4 sends the KDF seed to the YubiKey as the challenge. Legacy KDBX3 AES-KDF adds the challenge-response afterwards, for backward compatibility.

### 4.3 `Database::open()`

[src/core/Database.cpp](src/core/Database.cpp#L131):

1. Opens the file read-only and hashes the first block. That hash is used later to spot external modification.
2. Calls `KeePass2Reader reader; reader.readDatabase(&dbFile, key, this)`.
3. On success it sets the file path, marks the database clean, emits `databaseOpened()` and starts the `FileWatcher`.

### 4.4 Format dispatch

[src/format/KeePass2Reader.cpp](src/format/KeePass2Reader.cpp) reads the magic numbers and version, then:

- KeePass 1 signature → error asking the user to *import* it (via `KeePass1Reader`).
- Version < 4 → `Kdbx3Reader`.
- Version ≥ 4 → `Kdbx4Reader`.

The shared header parsing lives in [src/format/KdbxReader.cpp](src/format/KdbxReader.cpp).

### 4.5 KDBX4 decryption pipeline

[src/format/Kdbx4Reader.cpp](src/format/Kdbx4Reader.cpp), `readDatabaseImpl()`, shows the security design clearly:

```
file ──► HmacBlockStream ──► SymmetricCipherStream ──► QtIOCompressor (gzip) ──► inner header ──► KdbxXmlReader
         (integrity per     (AES-256 / ChaCha20 /      (optional)                (protected     (builds Groups/
          block)             Twofish, CBC/stream)                                 stream key,    Entries)
                                                                                  binaries)
```

The steps:

1. **Derive the key** (the slow part, Argon2):
   `AsyncTask::runAndWaitForFuture([&]{ return db->setKey(key, false, false); })` runs the KDF on a worker thread so the UI stays responsive.
2. `finalKey = SHA256(masterSeed || transformedKey)`.
3. **Verify the header**: a SHA-256 check for corruption, then an HMAC check against the HMAC key. If the HMAC check fails, the credentials were wrong ("HMAC mismatch").
4. Wrap the device in streams from [src/streams/](src/streams/). Each stream is a `QIODevice` that decorates the one below it (`LayeredStream`).
5. Read the **inner header**, which holds the protected-stream key and the attachment binaries.
6. `KeePass2RandomStream` (ChaCha20/Salsa20) decrypts fields marked as *protected* inside the XML, such as passwords.
7. [src/format/KdbxXmlReader.cpp](src/format/KdbxXmlReader.cpp) parses the XML into `Group` and `Entry` objects.

Saving runs the same pipeline in reverse: `Database::save` → `performSave` → `writeDatabase` → `KeePass2Writer` → `Kdbx4Writer` / `KdbxXmlWriter`. `performSave` supports three save modes:

- **Atomic**: `QSaveFile`.
- **TempFile**: write a temporary file, then rename it into place.
- **DirectWrite**.

It can also write a backup first. Every save calls `setKey(..., updateTransformSalt=true)`, so the KDF seed is regenerated. `writeDatabase` asserts that the transformed key actually changed.

---

## 5. The GUI (`src/gui`)

The main widget hierarchy:

```
MainWindow                               src/gui/MainWindow.cpp   (menus, toolbar, tray, global actions)
└── DatabaseTabWidget                    src/gui/DatabaseTabWidget.cpp  (one tab per database, lock-all)
    └── DatabaseWidget (QStackedWidget)  src/gui/DatabaseWidget.cpp
        ├── DatabaseOpenWidget           unlock screen
        ├── main view: GroupView | EntryView | EntryPreviewWidget
        ├── EditEntryWidget              src/gui/entry/
        ├── EditGroupWidget              src/gui/group/
        ├── DatabaseSettingsDialog       src/gui/dbsettings/
        └── ReportsDialog                src/gui/reports/ (health check, HIBP, statistics, passkeys)
```

- `DatabaseWidget` switches between modes with `switchToMainView()`, `switchToEntryEdit()`, `switchToOpenDatabase()` and similar methods. `lock()` saves or discards changes, then releases the decrypted data.
- The views follow Qt model/view. `GroupModel` and `EntryModel` adapt the `core` tree for `GroupView` and `EntryView`, and `SortFilterHideProxyModel` handles column hiding and sorting.
- `DatabaseWidgetStateSync` keeps splitter sizes and column layouts consistent across tabs.
- Platform-specific code lives in [src/gui/osutils/](src/gui/osutils/) (`macutils`, `winutils`, `nixutils`). It covers screen-lock detection, launch-at-startup and device listeners, all behind `osUtils->...`.
- `Clipboard` copies secrets to the clipboard, clears them after a timeout, and uses platform hints to keep them out of clipboard history.
- New-database setup is under [src/gui/wizard/](src/gui/wizard/). The key editors for passwords, key files and YubiKeys are under [src/gui/databasekey/](src/gui/databasekey/).

---

## 6. Integrations

### 6.1 Browser integration and passkeys (`src/browser`, `src/proxy`)

```
Browser extension ──stdin/stdout (native messaging)──► keepassxc-proxy ──QLocalSocket──► BrowserHost ──► BrowserAction ──► BrowserService ──► Database
```

- [src/proxy/NativeMessagingProxy.cpp](src/proxy/NativeMessagingProxy.cpp) is a small process launched by the browser. It relays length-prefixed JSON between stdin/stdout and the app's local socket.
- [src/browser/BrowserHost.cpp](src/browser/BrowserHost.cpp) runs a `QLocalServer` that only the current user can access.
- [src/browser/BrowserAction.cpp](src/browser/BrowserAction.cpp) dispatches `action` values such as `associate`, `get-logins`, `set-login`, `generate-password`, `get-totp`, `passkeys-get` and `passkeys-register`. Messages are encrypted end to end with **libsodium `crypto_box`** ([BrowserMessageBuilder.cpp](src/browser/BrowserMessageBuilder.cpp)). The key exchange happens on `change-public-keys`, and the association key is stored in the database's custom data.
- [src/browser/BrowserService.cpp](src/browser/BrowserService.cpp) holds the logic: URL matching, access-confirmation dialogs and entry creation.
- Passkeys: `BrowserPasskeys`, `BrowserPasskeysClient`, `BrowserCbor` and `PasskeyUtils` implement the WebAuthn authenticator side.
- `NativeMessageInstaller` writes the native-messaging manifest for each supported browser.

### 6.2 Auto-Type (`src/autotype`)

- [src/autotype/AutoType.cpp](src/autotype/AutoType.cpp) parses a sequence such as `{USERNAME}{TAB}{PASSWORD}{ENTER}` into `AutoTypeAction`s (key presses, delays, field values), then runs them.
- Platform backends implement `AutoTypePlatformInterface` ([AutoTypePlatform.h](src/autotype/AutoTypePlatform.h)) and are loaded as **Qt plugins** from `xcb/`, `wayland/`, `windows/` and `mac/`.
- *Global* Auto-Type matches the active window title against each entry's associations and shows `AutoTypeSelectDialog` when more than one entry matches.

### 6.3 SSH Agent (`src/sshagent`)

`OpenSSHKey` and `ASN1Key` parse private keys stored as attachments or attributes. `SSHAgent` speaks the agent protocol: a Unix socket, Pageant, or the Windows OpenSSH pipe. It adds keys when a database unlocks and removes them when it locks. `KeeAgentSettings` stores each entry's settings, compatible with KeeAgent.

### 6.4 Secret Service (`src/fdosecrets`, Linux)

This implements the `org.freedesktop.secrets` D-Bus API so that apps using libsecret can store secrets in KeePassXC. The D-Bus objects (`Service`, `Collection`, `Item`, `Session`, `Prompt`) live in [src/fdosecrets/objects/](src/fdosecrets/objects/), and each collection maps to a database. See [src/fdosecrets/README.md](src/fdosecrets/README.md).

### 6.5 KeeShare (`src/keeshare`)

KeeShare syncs a group with an external container file. `ShareExport` writes the file and `ShareImport` reads it back through the `Merger`. `ShareObserver` watches for changes on both sides.

### 6.6 Quick Unlock (`src/quickunlock`)

`QuickUnlockInterface` has three backends: `TouchID.mm` (macOS Keychain + LocalAuthentication), `WindowsHello.cpp` and `Polkit.cpp` (Linux). Each one encrypts the serialized `CompositeKey` with a key protected by the OS, so a database can be unlocked again without retyping the password.

---

## 7. The CLI (`src/cli`)

[src/cli/keepassxc-cli.cpp](src/cli/keepassxc-cli.cpp) looks up a subcommand with `Commands::getCommand(name)` and runs it. Running `open` starts an interactive shell built on `LineReader`.

- [Command.h](src/cli/Command.h) is the base class. It declares the options and positional arguments, and its `execute(QStringList)` is pure virtual.
- [DatabaseCommand.h](src/cli/DatabaseCommand.h) opens and unlocks a database first, then calls `executeWithDatabase(db, parser)`. Most subcommands (`Show`, `Add`, `Edit`, `Clip`, `Search`, `Export`, `Merge`, …) derive from it.
- [Utils.cpp](src/cli/Utils.cpp) handles password prompts (no echo), key-file and YubiKey loading, and the shared `unlockDatabase()`.

The CLI links `keepassxc_core`, so it reads and writes databases with the same code as the GUI.

---

## 8. Cryptography (`src/crypto`)

- `Crypto`: initialisation, a version check and known-answer self-tests run at startup.
- `CryptoHash`: SHA-256/512 and HMAC.
- `SymmetricCipher`: AES-256 (CBC), Twofish (CBC) and ChaCha20/Salsa20, mapped from KDBX cipher UUIDs (`cipherUuidToMode`).
- `Random`: a CSPRNG built on Botan's `System_RNG` where the platform has one.
- `kdf/`: the `Kdf` base class has `transform()`. It has two implementations: `AesKdf`, the legacy one, and `Argon2Kdf`, which supports Argon2d and Argon2id and is the default.

Memory hygiene: [src/core/Alloc.cpp](src/core/Alloc.cpp) overrides global `operator delete` to zero freed memory. Protected attributes stay flagged so that the UI hides them and the file format encrypts them.

---

## 9. Tests (`tests`)

- QtTest-based. Each `TestXxx.cpp` is registered in [tests/CMakeLists.txt](tests/CMakeLists.txt) with `add_unit_test(NAME ... SOURCES ...)`.
- Notable suites: `TestKdbx3`/`TestKdbx4`/`TestKeePass2Format` (file format round-trips), `TestKeys` (key files and composite keys), `TestMerge`, `TestCli`, `TestBrowser`, `TestPasskeys`, `TestAutoType` (with a mock platform in `src/autotype/test`) and `TestFdoSecrets`.
- GUI tests live in [tests/gui/](tests/gui/) and only build with `-DWITH_GUI_TESTS=ON`.
- Fixture databases and key files are in `tests/data/`.

Build and run:

```bash
mkdir build && cd build
cmake -DWITH_TESTS=ON -DWITH_GUI_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure            # or run a single binary, e.g. ./tests/testkdbx4
```

See [INSTALL.md](INSTALL.md) for platform dependencies.

---

## 10. Suggested reading order for newcomers

1. [src/main.cpp](src/main.cpp): startup.
2. [src/core/Database.h](src/core/Database.h), [Group.h](src/core/Group.h), [Entry.h](src/core/Entry.h): the data model.
3. [src/keys/CompositeKey.cpp](src/keys/CompositeKey.cpp) → [src/format/Kdbx4Reader.cpp](src/format/Kdbx4Reader.cpp) → [src/format/KdbxXmlReader.cpp](src/format/KdbxXmlReader.cpp): how a file gets decrypted.
4. [src/gui/DatabaseOpenWidget.cpp](src/gui/DatabaseOpenWidget.cpp) → [src/gui/DatabaseWidget.cpp](src/gui/DatabaseWidget.cpp): how the UI drives the core.
5. Whichever integration you're working on (§6), together with its `tests/TestXxx.cpp`.

### Conventions worth knowing

- Qt idioms are used throughout: signals and slots, `QSharedPointer<Database>`, and `tr()` for every user-visible string (translations live in `share/translations`).
- Formatting is enforced with `clang-format` (see `.clang-format`).
- Feature code is wrapped in `#ifdef WITH_XC_<FEATURE>`. Check `config-keepassx.h.in` for the full list.
- Slow work (KDF, HIBP checks, icon downloads) goes through `AsyncTask` so the UI never blocks.
