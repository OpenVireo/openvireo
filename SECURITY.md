# Security Policy

OpenVireo is a password manager, so security reports are our top priority. Thank you for reporting responsibly.

## Reporting a vulnerability

Please **do not** open a public issue, discussion or pull request for a security problem.

- **GitHub (preferred):** use [Report a vulnerability](https://github.com/openvireo/openvireo/security/advisories/new) on the affected repository's **Security** tab. Private reporting is turned on for every public OpenVireo repository.
- **Email:** send your report to [admin@openvireo.com](mailto:admin@openvireo.com) with "Security" in the subject.

Please include:

- the product and version (desktop, browser extension, iOS, web vault, SDK/CLI, sync server, or the openvireo.com account service)
- steps to reproduce, or a proof of concept
- the impact you think it has

## What to expect

- We acknowledge your report within **3 business days**.
- We keep you updated while we investigate and fix it, and agree a disclosure date with you.
- We credit you in the advisory and release notes, unless you prefer to stay anonymous.

If we decide the report is not a vulnerability, we tell you why. We may turn it into a normal issue.

## Scope

In scope:

- the OpenVireo apps: desktop, browser extension, iOS, and the web vault at app.openvireo.com
- the OpenVireo SDK, `ov` CLI, connect server and sync server
- the openvireo.com services: accounts, sync, app and share links

Examples of real vulnerabilities:

- unauthorized access to vault data, master passwords, Secret Keys or account keys
- bypassing authentication, two-step login, account recovery, or organization access controls
- weak or broken cryptography, or plaintext secrets leaking to the server, logs or disk
- remote code execution or privilege escalation

Out of scope:

- issues that require an already compromised device, or a locally modified app (for example injected DLLs)
- crashes or misbehavior from normal use (please open a normal issue)
- missing best-practice headers, or rate limits, without a demonstrated impact
- denial of service, social engineering, or physical attacks
- vulnerabilities in third-party dependencies that OpenVireo cannot exploit (please report these upstream)

Problems in code inherited from KeePassXC or KeePassXC-Browser that also affect upstream should also be reported to the [KeePassXC team](https://github.com/keepassxreboot/keepassxc/security).

## Safe harbor

We will not take legal action against research done in good faith under this policy. That means you avoid privacy violations, data destruction and service disruption, only test against accounts you own, and give us reasonable time to fix the problem before you disclose it.

## CVEs

Please do not request a CVE before we have confirmed the issue. If we do not reply within 30 days, this no longer applies.

## Verifying releases

Releases are signed with the OpenVireo release key, fingerprint `FE27 78BF 0747 05E0 994B BAF9 FEBA 3556 2DE1 04CB`. See [Verifying signatures](https://openvireo.com/verifying-signatures/).
