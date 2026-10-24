# <img src="share/icons/application/scalable/apps/keepassxc.svg" width="40" height="40"/> OpenVireo

[OpenVireo](https://openvireo.com) is a modern, secure, and open-source password manager that stores and manages your most sensitive information. You can run OpenVireo on Windows, macOS, and Linux systems. It saves many different types of information, such as usernames, passwords, URLs, attachments, and notes in an offline, encrypted KDBX file that can be stored in any location, including private and public cloud solutions, and that stays compatible with other KeePass clients such as KeePassXC, KeePassDX and Strongbox.

## A fork of KeePassXC

OpenVireo is a fork of [KeePassXC](https://keepassxc.org), which is made by the KeePassXC Team and its contributors. Nearly all of the application is their work, and it remains under their copyright and the GPL. OpenVireo adds accounts, encrypted sync and sharing, a redesigned interface and its own browser extension, OpenVireo Browser. OpenVireo is not affiliated with or endorsed by the KeePassXC Team; please report OpenVireo problems to OpenVireo, not to KeePassXC.

## Quick Start

Download OpenVireo from the [releases page](https://github.com/openvireo/openvireo/releases). The [Getting Started guide](./docs/GettingStarted.adoc) and the [User Guide](./docs/UserGuide.adoc) are in this repository.

## Features List

OpenVireo has numerous features for novice and power users alike. Our goal is to create an application that can be used by anyone while still offering advanced features to those that need them.

### Core Features

* Create, open, and save databases in the KDBX format (KeePass-compatible with KDBX4 and KDBX3)
* All information is encrypted at rest and never exposed outside the program
* Store sensitive information in entries that are organized by groups
* Password generator
* Search for entries
* TOTP storage and generation
* YubiKey/OnlyKey challenge-response support
* Auto-Type passwords into applications
* Browser integration with Google Chrome, Mozilla Firefox, Microsoft Edge, Chromium, Vivaldi, Brave, and Tor-Browser
* Support for passkeys using the browser integration
* Entry icon download
* Import databases from CSV, 1Password, Bitwarden, Proton Pass, and KeePass1 formats
* Optional OpenVireo accounts: end-to-end encrypted vault sync and sharing through a self-hostable sync server

### Advanced
* Database reports (password health, HIBP, and statistics)
* Database export to CSV, XML, and HTML formats
* TOTP storage and generation
* Field references between entries
* File attachments and custom attributes
* Entry history and data restoration
* Command line interface (openvireo-cli)
* SSH Agent integration
* FreeDesktop.org Secret Service (replace Gnome keyring, etc.)
* Additional encryption choices: Twofish and ChaCha20

For a full list of changes, read the [CHANGELOG](CHANGELOG.md) document. \
For a full list of keyboard shortcuts, see [KeyboardShortcuts.adoc](./docs/topics/KeyboardShortcuts.adoc)

## Building OpenVireo

Detailed instructions are available in the [Build and Install](./INSTALL.md) page.

## Contributing

We are always looking for suggestions on how to improve OpenVireo. If you find any bugs or have an idea for a new feature, please let us know by opening a report in the [issue tracker](https://github.com/openvireo/openvireo/issues) on GitHub.

You may directly contribute your own code by submitting a pull request. Please read the [CONTRIBUTING](.github/CONTRIBUTING.md) document for further information.

Contributors are required to adhere to the project's [Code of Conduct](CODE-OF-CONDUCT.md).

## Generative AI

Generative AI is fast becoming a first-party feature in most development environments, including GitHub itself. If the majority of a code submission is made using Generative AI (e.g., agent-based or vibe coding) then **we will document that in the pull request.** All code submissions go through a rigorous review process regardless of the development workflow or submitter.

## License

OpenVireo, like KeePassXC, is licensed under GPL-2 or GPL-3. Additional licensing for third-party files is detailed in [COPYING](./COPYING).
