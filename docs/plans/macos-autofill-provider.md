# macOS AutoFill Credential Provider — Plan

**Status:** Implemented behind `KPXC_FEATURE_AUTOFILL` (off by default); waiting on Apple Developer account setup for signed testing
**Branch target:** `develop`
**Last updated:** 2026-10-14

## 1. Goal

Make KeePassXC a system AutoFill credential provider on macOS (AuthenticationServices framework). After enabling it in
**System Settings → General → AutoFill & Passwords**, users get KeePassXC logins, passkeys and one-time codes in the
QuickType bar for Safari and native apps, the same way 1Password and Bitwarden integrate.

### Non-goals

- Writing into Apple's Passwords app or iCloud Keychain. No public API exists for that.
- iOS support.
- Replacing the browser extension integration. Both coexist and share logic.
- Mac App Store distribution.

## 2. Background

| Capability | Minimum macOS | API |
|---|---|---|
| Passwords | 11 | `ASPasswordCredentialIdentity`, `ASPasswordCredential` |
| Passkeys | 14 | `ASPasskeyCredentialIdentity`, `ASPasskeyRegistrationCredential`, `ASPasskeyAssertionCredential` |
| One-time codes | 15 | `ASOneTimeCodeCredentialIdentity`, `ASOneTimeCodeCredential` |

The provider is an **app extension** (`.appex`) that runs **sandboxed, in its own process**. It can't see KeePassXC's
in-memory unlocked database, which drives most of this design.

### Existing pieces to reuse

| Area | Location |
|---|---|
| Bundle, entitlements, provisioning profile (team `DP5U2UP42V`, bundle ID `com.bonganimbigi.keepassxc`) | `share/macosx/`, `src/CMakeLists.txt` (~L446) |
| Objective-C++ integration | `src/gui/osutils/macutils/AppKitImpl.mm`, `src/quickunlock/TouchID.mm` |
| URL matching, access control, "hide from browser" rules | `src/browser/BrowserService.cpp` |
| Passkeys (CBOR, attestation, assertion) | `src/browser/BrowserPasskeys.cpp`, `BrowserCbor.cpp`, `PasskeyUtils.cpp` |
| Local socket server pattern | `src/browser/BrowserHost.cpp`, `BrowserShared::localServerPath()` |

## 3. Architecture

```
┌──────────────────────────── KeePassXC.app ─────────────────────────────┐
│                                                                         │
│  Main app (Qt, not sandboxed)            Contents/PlugIns/              │
│  ┌──────────────────────────────┐        ┌───────────────────────────┐  │
│  │ AutofillProviderService      │        │ KeePassXCAutofill.appex   │  │
│  │  ├─ IdentityStoreSync ───────┼──────► │ (sandboxed, Swift/ObjC)   │  │
│  │  │   ASCredentialIdentityStore│ system │ ASCredentialProvider-     │  │
│  │  ├─ AutofillHost (QLocalServer)◄──────┤   ViewController          │  │
│  │  │   socket in App Group      │  IPC   │                           │  │
│  │  └─ BrowserService/Passkeys  │        └───────────────────────────┘  │
│  └──────────────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────────────┘
```

### Decision: the extension talks to the running app (recommended)

The extension asks the running main app for secrets over a Unix domain socket in the shared App Group container
(`~/Library/Group Containers/DP5U2UP42V.com.bonganimbigi.keepassxc/`). The extension's sandbox allows that path, and the main app
isn't sandboxed, so it can listen there.

**Alternative considered:** the extension opens the `.kdbx` file itself. Rejected because it needs core, format and
crypto linked into a sandboxed bundle, security-scoped bookmarks, its own unlock UI and its own key handling, all
duplicating the main app.

**Consequence:** autofill only works while KeePassXC is running. A locked database gives an "Unlock in KeePassXC" flow.

## 4. Work breakdown

Code for phases 0 to 3 is in place and builds with `-DKPXC_FEATURE_AUTOFILL=ON`. What's left is the developer account
setup and end-to-end testing on a signed build, which can't happen until the profiles exist.

### Phase 0: Prove the signing chain

Exit criterion: a stub extension built by CI appears in System Settings → AutoFill & Passwords.

Progress as of 2026-10-14: the main app now signs, notarizes and launches under the fork's own identity.

- [x] Move the fork to its own identifiers: app ID and bundle ID `com.bonganimbigi.keepassxc`, team `DP5U2UP42V`
      (`73819c48`). The extension's identifiers below follow from this.
- [x] Bundle a Developer ID provisioning profile for team `DP5U2UP42V` in `share/macosx/embedded.provisionprofile`,
      re-issued for the current Developer ID certificate so the signed app launches (`10c11813`).
- [x] Fix `release-tool.py`'s notarization credential check: it now asks `xcrun notarytool history` whether the
      keychain profile works, instead of looking up a keychain item name newer Xcode versions no longer use
      (`10c11813`).
- [ ] **Developer account:** enable the AutoFill Credential Provider capability on the app ID
      `com.bonganimbigi.keepassxc`, register the App ID `com.bonganimbigi.keepassxc.autofill` with the same capability,
      and register the App Group `DP5U2UP42V.com.bonganimbigi.keepassxc` for both.
- [ ] **Developer account:** issue Developer ID profiles for both IDs and save them as
      `share/macosx/autofill/OpenVireo.provisionprofile` and `share/macosx/autofill/OpenVireoAutofill.provisionprofile`.
      CMake warns while either is missing. The app's profile replaces `share/macosx/embedded.provisionprofile` in
      AutoFill builds.
- [x] `share/macosx/autofill/`: `Info.plist.in` with `ProvidesPasswords`, `ProvidesPasskeys`, `ProvidesOneTimeCodes`
      and `ShowsConfigurationUI`, plus entitlements templates for the extension (sandbox, App Group, AutoFill) and the
      app (adds the App Group and AutoFill to the existing keys). Both are generated from `MACOS_BUNDLE_IDENTIFIER`
      and `MACOS_TEAM_IDENTIFIER` in the root `CMakeLists.txt`.
- [x] CMake builds the `.appex` as a `MACOSX_BUNDLE` executable with `BUNDLE_EXTENSION appex`, linked with
      `-e _NSExtensionMain`, and embeds it in `Contents/PlugIns/` both in the build tree and on install. No Xcode
      sub-project needed (Q5).
- [x] `cmake/MacOSCodesign.cmake.in` signs inside-out: the whole bundle, then the extension with its entitlements, then
      the app. An ad-hoc signed copy passes `codesign --verify --deep --strict`.
- [x] `release-tool.py build --autofill` turns on `KPXC_FEATURE_AUTOFILL` for macOS builds and warns without `--sign`.

### Phase 1: Passwords

**Main app: `src/autofill/`, behind `KPXC_FEATURE_AUTOFILL`** (the plan's `WITH_XC_AUTOFILL_MACOS`, renamed to match the
other `KPXC_FEATURE_*` options)

- [x] `AutofillCredentials`: entry → identity mapping and request matching, free of AppKit and unit tested.
  - One password identity per host per entry, from its URL, additional URLs and passkey relying party, with
    `recordIdentifier = "<root group uuid>:<entry uuid>"`. The root group's uuid is used because it's stored in the
    file, unlike `Database::uuid()`.
  - Same exclusions as `BrowserService`: recycle bin, expired entries, "hide from browser" on the entry or its groups.
    Hosts on an entry's KeePassXC-Browser deny list are left out too.
  - A request for `login.example.com` matches an entry for `example.com`, not the reverse.
- [x] `AutofillService` keeps the identity store in sync. Instead of the planned incremental updates it rebuilds and
      replaces the whole set 500 ms after the last change to any unlocked database, which is simpler and fast enough.
      On lock it drops that database's identities unless "keep suggesting usernames while locked" is on (Q1).
- [x] `AutofillHost`: a `QLocalServer` on `~/Library/Group Containers/DP5U2UP42V.com.bonganimbigi.keepassxc/autofill.sock`.
  - Peer authentication: each connection's audit token (`LOCAL_PEERTOKEN`) is checked with
    `SecCodeCopyGuestWithAttributes` + `SecCodeCheckValidity` against
    `identifier "com.bonganimbigi.keepassxc.autofill" and anchor apple generic and certificate leaf[subject.OU] = "DP5U2UP42V"`.
    The host refuses to listen without a validator. A test confirms an unsigned client is refused (-67050).
- [x] The service only starts when the running binary is entitled to the App Group. Without that entitlement, even
      looking up the group container makes macOS ask the user for access to other apps' data, which blocks startup.
- [x] Settings → AutoFill: enable toggle, keep-while-locked toggle, which unlocked databases to use (stored by id in
      the local config, so databases aren't modified), System Settings status, and a button that opens the AutoFill
      pane (`ASSettingsHelper` on macOS 15+, the Extensions pane before that).

**Extension (Objective-C, not Swift, so that CMake builds it without an Xcode project)**

- [x] Without user interaction: `getCredential` with a 3 s timeout; locked, not running or slow returns
      `userInteractionRequired`, unknown records return `credentialIdentityNotFound`.
- [x] With an interface: if locked, an "Unlock in KeePassXC" button sends `requestUnlock`, which raises KeePassXC's
      unlock dialog and answers once a database unlocks. KeePassXC then hands focus back to the app that asked.
- [x] Credential list: a searchable native list fed by `listCredentials`. It falls back to every entry when none
      matches the site.
- [x] Configuration screen.
- [x] Not running: "Open KeePassXC" launches the containing app with `NSWorkspace`, waits for it to listen, then
      starts the unlock (Q4).

### Phase 2: Passkeys

- [x] Passkey identities with relying party, user name, credential id and user handle.
- [x] Assertion: the extension sends `rpId`, the record or credential id and `clientDataHash`; the app refuses to sign
      for any relying party other than the passkey's own.
- [x] Registration: the extension asks the user to confirm, then the app creates the entry in the active database
      (or the first one AutoFill may use) through `BrowserService::addPasskeyToGroup` and returns the attestation
      object.
- [x] `BrowserPasskeys::buildAssertionWithClientDataHash` and `buildRegistrationWithClientDataHash` sign over a given
      hash. The browser path now goes through the same signing function, and `testpasskeys` still passes.
- [x] Confirmation UX: in the extension sheet, not a Qt dialog in the main app.
- [x] `excludedCredentials` (macOS 15): the extension passes the site's existing credential ids, and when one of
      them is already in an AutoFill database the app refuses with `excluded`. The extension then cancels with
      `ASExtensionErrorCodeMatchedExcludedCredential`, so the system tells the user the account already has a passkey.
      The extension checks with `passkeyCheckExcluded` before asking the user to confirm, and the app checks again
      when saving, which covers a database that was locked during the first check.
- [ ] Not handled yet: passkey extensions such as PRF and large blobs.

### Phase 3: One-time codes (macOS 15+)

- [x] `ProvidesOneTimeCodes`, one identity per host for entries with valid TOTP settings, and codes generated by the
      app with the existing TOTP code.

### Remaining

- [ ] Developer account and profiles (phase 0 above).
- [ ] Run the QA checklist in §7 on a signed, notarized build. Results so far, on a Developer ID signed debug build
      (no hardened runtime, Homebrew Qt) on 2026-10-14:
  - [x] The extension registers with `pluginkit` and shows in System Settings.
  - [x] Safari fills a password with the database unlocked.
  - [x] Locked database: unlock in KeePassXC, then fill.
  - [x] KeePassXC not running: "Open KeePassXC". The sandboxed extension can launch the app (Q4).
  - [ ] Native app login.
  - [x] Passkeys: registration and sign-in, with the app signing over the system's client data hash.
  - [ ] One-time code (macOS 15+).
  - [ ] Settings → AutoFill page and the extension's configuration screen.
- [ ] User docs in `docs/topics/`.
- [ ] Turn `KPXC_FEATURE_AUTOFILL` on by default once signed builds ship with the new profiles.

## 5. IPC protocol

Implemented in `src/autofill/AutofillProtocol.h` and, for the extension, `src/autofill/extension/KPXAutofillClient.m`.
Each frame is a 4-byte big-endian length followed by a UTF-8 JSON object, at most 1 MiB. The extension opens one
connection per request; every connection passes the code signature check first. Requests name their kind in `type`.
Responses carry `ok`, and `error` when it's false: `locked`, `notFound`, `denied`, `canceled`, `disabled`, `badRequest`,
`failed` or `excluded`. Binary fields are standard base64.

| Request | Fields | Response |
|---|---|---|
| `status` | — | `{ unlocked, locked }` |
| `listCredentials` | `kind` (`password`, `passkey`, `oneTimeCode`), `serviceIdentifiers: [{type, value}]`, `rpId` for passkeys | `{ items: [{recordId, title, username, url, hasPassword, hasPasskey, hasOneTimeCode}] }`, no secrets |
| `getCredential` | `recordId` | `{ username, password }` |
| `getOneTimeCode` | `recordId` | `{ code }` |
| `passkeyAssert` | `rpId, clientDataHash` and `recordId` or `credentialId` | `{ credentialId, userHandle, authenticatorData, signature }` |
| `passkeyRegister` | `rpId, userName, userHandle, clientDataHash, algorithms, excludedCredentials` | `{ credentialId, attestationObject }`, or `excluded` when the account already has one of the excluded passkeys |
| `passkeyCheckExcluded` | `rpId, excludedCredentials` | `{ ok }`, or `excluded`. Saves nothing |
| `requestUnlock` | — | Raises KeePassXC's unlock dialog and answers `{ ok }` once a database unlocks, or `canceled` |

The planned `databaseUnlocked` / `databaseLocked` push events turned out unnecessary: `requestUnlock` simply answers late.

## 6. Security considerations

- **Peer authentication on the socket is mandatory.** Socket file permissions alone would let any process running as
  the user read passwords.
- No secrets go into `ASCredentialIdentityStore`. It holds identifiers and usernames only.
- Per-entry deny lists from `BrowserEntryConfig` apply; the browser's allow prompts don't, because the user picks the
  entry in the system's own UI (Q3).
- Memory: secrets cross the socket once per fill. The extension wipes the raw response bytes, but the `NSString` copies
  it hands to the system can't be wiped.
- The socket only exists while AutoFill is on, and only in builds entitled to the App Group.
- Threat model note: Apple shows the user which provider is filling, and the user picks the entry.

## 7. Testing

- Unit tests (`tests/TestAutofill.cpp`, `testautofill`): frame encoding and decoding, entry → identity mapping and
  exclusions, host matching, record lookup, the socket host with an injected validator and with the real code
  signature check, and hash-only passkey signing and registration verified with Botan.
- Integration: needs a **properly signed** build, because ad-hoc signed extensions with restricted entitlements don't
  register. Add a manual QA checklist covering Safari login, a native app login, locked-database flow, app not running,
  passkey register and assert on webauthn.io, and TOTP fill.
- CI: build the appex on macOS runners. Do signed smoke tests only on release builds.

## 8. Open questions

| # | Question | Leaning |
|---|---|---|
| Q1 | Clear identities on lock, or keep them so usernames still show while locked? | **Decided:** configurable, default clear |
| Q2 | Does the app's minimum macOS version allow phase 1 without `@available` sprawl? | **Decided:** the app keeps its 10.15 target and weak-links AuthenticationServices; the platform layer gates per API. The extension sets `LSMinimumSystemVersion` 11.0 |
| Q3 | How do the browser access-control rules apply to native-app requests (bundle IDs)? | **Decided:** apps arrive as their associated domains, so they're matched as sites, and deny lists apply |
| Q4 | Should KeePassXC auto-launch when the extension needs it? | **Decided:** on the user's click, via `NSWorkspace`; confirmed working from the sandbox on a signed build |
| Q5 | Build the appex with CMake or an Xcode sub-project? | **Decided:** CMake |
| Q6 | Is there upstream appetite? This may need to stay a fork feature | The build already ships under fork identifiers and team `DP5U2UP42V`; upstreaming would mean re-signing under `G2S7P7J672` and `org.keepassxc`. Ask maintainers before phase 1 |

## 9. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Autofill entitlement not granted or delayed | Blocks everything | Request first, in phase 0 |
| Building and signing the appex in CMake + Qt | Schedule slip | Fall back to an `xcodebuild` sub-project |
| Mistakes in IPC security | Credential exposure | Code-signature peer check, security review before merge |
| Passkey hash-only signing breaks `BrowserPasskeys` assumptions | Phase 2 rework | Refactor behind tests first |
| Developers can't test locally without signing | Slow iteration | Document a dev profile setup and keep logic testable outside the appex |

## 10. Building and trying it

```sh
cmake -B build-autofill -DKPXC_FEATURE_AUTOFILL=ON -DWITH_XC_CODESIGN_IDENTITY="Developer ID Application: …" .
cmake --build build-autofill
ctest --test-dir build-autofill -R testautofill
```

The extension only registers once the app is signed with profiles that grant the AutoFill entitlement and the App
Group. After installing, turn KeePassXC on in **System Settings → General → AutoFill & Passwords** and in KeePassXC's
**Settings → AutoFill**. `pluginkit -m -p com.apple.authentication-services-credential-provider-ui` lists the extension
once macOS has registered it.

## 11. Milestones

1. **M0:** the stub extension registers from a CI-signed build.
2. **M1:** passwords autofill in Safari and native apps with the database unlocked.
3. **M1.1:** locked and not-running flows, settings UI, docs (`docs/topics/`).
4. **M2:** passkeys.
5. **M3:** one-time codes.
