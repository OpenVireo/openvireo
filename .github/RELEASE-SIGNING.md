# Signed packages and automatic updates

The nightly and release workflows sign every package they build and publish update feeds that the
app installs updates from. Each part turns on once its secrets or variables are set in the repository
settings (Settings → Secrets and variables → Actions); until then the workflows build unsigned packages
and print a warning for what they skipped.

| What | macOS | Windows | Linux |
| --- | --- | --- | --- |
| Code signing | Developer ID + notarization | Azure Artifact Signing (exe, dll, msi) | GPG signature embedded in the AppImage |
| Installs updates | Sparkle 2 | WinSparkle | AppImageUpdate (zsync); the app only reports new versions |
| Update signature | Ed25519 (`sparkle:edSignature`) | Ed25519 (`sparkle:edSignature`) | GPG (AppImage) |

Every release and nightly also carries `SHA256SUMS.txt` and its GPG signature `SHA256SUMS.txt.asc`.

## Update channels

Release builds follow the **stable** channel and nightlies the **nightly** channel; users can switch in
Settings → General. Each channel has one Sparkle appcast per platform, attached to a GitHub release:

- stable: `https://github.com/openvireo/openvireo/releases/latest/download/appcast-<platform>-<arch>.xml`
- nightly: `https://github.com/openvireo/openvireo/releases/download/nightly/appcast-<platform>-<arch>.xml`

`releases/latest` skips drafts and pre-releases, so a release's feeds go live when its draft is published.
The updaters compare `KEEPASSXC_UPDATE_VERSION`: a release is its own version (`0.9.0`), a nightly appends
a build number from the UTC date and hour (`0.9.0.26102503`), so nightlies sort above the release they
were built from and below the next one. Local and other snapshot builds follow no channel and never update.

## Versions and releases

OpenVireo has its own version number, separate from KeePassXC's. `KEEPASSXC_UPSTREAM_VERSION` in
`CMakeLists.txt` records the KeePassXC release it is based on; the About dialog and the debug info show
it, and the browser extension is sent it, since the extension enables features by KeePassXC version.

Versions are plain `X.Y.Z`: the release tooling, the MSI and the update feeds accept nothing else. Releases
before 1.0 are betas: the app shows a Beta pill and notice and the GitHub release is titled "beta", but
it is a full release rather than a pre-release, so the stable feeds serve it. `develop` keeps the version
of the last release until the next one is prepared.

To cut a release, set the version in `CMakeLists.txt` and `vcpkg.json`, add its `CHANGELOG.md` section
(the release notes) and its `<release>` in `share/linux/com.openvireo.OpenVireo.appdata.xml`, then:

```sh
git switch -c release/0.9.x            # or switch to the existing release branch
python3 release-tool.py tag 0.9.0 --skip-translations --no-latest -k FE2778BF074705E0994BBAF9FEBA35562DE104CB
git push origin release/0.9.x && git push origin tag 0.9.0
```

The tag starts the Release workflow, which builds and signs the packages and creates a draft release
with its update feeds. Check the draft, then publish it.

## One-time setup

### Update signing key (Sparkle and WinSparkle)

Both updaters verify an Ed25519 signature of each package against the public key built into the app.
Generate the key pair once, offline, and keep a backup of the private key: losing it means existing
installs can no longer be updated, and anyone who has it can push updates to every user.

```sh
openssl genpkey -algorithm ed25519 -out openvireo-update-key.pem
openssl pkey -in openvireo-update-key.pem -pubout -outform DER | tail -c 32 | base64
```

- Secret `UPDATE_PRIVATE_KEY`: the contents of `openvireo-update-key.pem`
- Variable `UPDATE_PUBLIC_KEY`: the base64 line printed by the second command

The publish jobs refuse to write feeds if the private key does not match the public key.
Without the public key, macOS and Windows builds leave out Sparkle and WinSparkle and only report new versions.

### GPG key (checksums and AppImages)

Use a signing subkey without a passphrase, and publish the primary public key (for example on the website
and keys.openpgp.org) so that users can check `SHA256SUMS.txt.asc`.

```sh
gpg --quick-add-key <primary fingerprint> ed25519 sign 2y
gpg --armor --export-secret-subkeys <subkey fingerprint>! > ci-signing-subkey.asc
# remove the passphrase from the exported copy only, e.g. in a temporary GNUPGHOME
```

- Secret `GPG_PRIVATE_KEY`: the contents of `ci-signing-subkey.asc`

The current key is `FE27 78BF 0747 05E0 994B BAF9 FEBA 3556 2DE1 04CB`, published at
`https://openvireo.com/openvireo-release.asc` (`openvireo-site/static/`), on keys.openpgp.org and on the
website's "Verifying Signatures" page. Update both places when the key or its expiry changes.

### Apple Developer ID (macOS)

- Secret `MACOS_CERTIFICATE_P12`: base64 of the exported "Developer ID Application" certificate and key
- Secret `MACOS_CERTIFICATE_PASSWORD`: the .p12 password
- Secrets `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` (an app-specific password) for notarization

`MACOS_TEAM_IDENTIFIER` in `CMakeLists.txt` must be the certificate's team.

### Azure Artifact Signing (Windows)

1. Create an Artifact Signing account and a public-trust certificate profile, and complete identity validation.
2. Create an app registration, give it the "Artifact Signing Certificate Profile Signer" role on the
   certificate profile, and add a federated credential for GitHub Actions: organization `OpenVireo`,
   repository `openvireo`, entity `Environment`, name `signing`. The repository sends immutable OIDC
   subjects, so the credential's subject must be `repo:OpenVireo@347287727/openvireo@1432261311:environment:signing`
   (see `gh api repos/OpenVireo/openvireo/actions/oidc/customization/sub`). The build jobs run in that environment,
   so one credential covers nightlies and release tags; no client secret is needed. In the repository's
   `signing` environment, deployment branch and tag rules can limit signing to `develop` and version tags.
3. Set the secrets `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`.
4. Set the variables `ARTIFACT_SIGNING_ENDPOINT` (the account's regional endpoint, e.g.
   `https://weu.codesigning.azure.net`), `ARTIFACT_SIGNING_ACCOUNT` and `ARTIFACT_SIGNING_PROFILE`.

The build signs through SignTool with the `Azure.CodeSigning.Dlib.dll` from the
`Microsoft.ArtifactSigning.Client` NuGet package, using the Azure CLI login of `azure/login`.
Locally the same works with `release-tool.py build … --sign --sign-dlib <dlib> --sign-metadata <json>`.

## Pinned downloads

`build.yml` pins Sparkle, WinSparkle and the Artifact Signing client by version and SHA-256 in its `env`
block. Update the version and checksum together.
