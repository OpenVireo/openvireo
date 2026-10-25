#!/usr/bin/env python3
#
# Writes the Sparkle appcasts that Sparkle (macOS), WinSparkle (Windows) and the update checker (Linux)
# read: one feed per platform and architecture, named as UpdateFeed::feedFileName() expects, with an
# Ed25519 signature of each package. The release workflows attach them to the GitHub release next to
# the packages they describe.
#
# The signing key is an Ed25519 private key in PEM form (openssl genpkey -algorithm ed25519), read from
# the environment variable named by --key-env. Signing uses the openssl command line tool.

import argparse
import base64
import email.utils
import html
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

SPARKLE_NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'

# Package name pattern -> (feed, label, extra enclosure attributes). Unsigned DMGs are left out:
# Sparkle would refuse to replace a signed app with them, and no one should be updated to one.
PACKAGES = [
    (re.compile(r'^OpenVireo-.+-arm64\.dmg$'), 'macos-arm64', 'macOS (Apple silicon)', {}),
    (re.compile(r'^OpenVireo-.+-x86_64\.dmg$'), 'macos-x86_64', 'macOS (Intel)', {}),
    (re.compile(r'^OpenVireo-.+-(x64|AMD64|amd64)\.msi$'), 'windows-x86_64', 'Windows (x64)',
     {'sparkle:installerArguments': '/passive'}),
    (re.compile(r'^OpenVireo-.+-(ARM64|arm64)\.msi$'), 'windows-arm64', 'Windows (ARM64)',
     {'sparkle:installerArguments': '/passive'}),
    (re.compile(r'^OpenVireo-.+-x86_64\.AppImage$'), 'linux-x86_64', 'Linux (x86_64)', {}),
    (re.compile(r'^OpenVireo-.+-aarch64\.AppImage$'), 'linux-arm64', 'Linux (aarch64)', {}),
]

MACOS_MINIMUM_VERSION = '12.0'


def sign(path, key_file):
    """Return the base64 Ed25519 signature of a file, checked against the key's public half."""
    with tempfile.TemporaryDirectory() as tmp:
        sig_file = Path(tmp) / 'sig'
        pub_file = Path(tmp) / 'pub.pem'
        subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', key_file,
                        '-in', path, '-out', sig_file], check=True)
        subprocess.run(['openssl', 'pkey', '-in', key_file, '-pubout', '-out', pub_file], check=True)
        subprocess.run(['openssl', 'pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', pub_file,
                        '-in', path, '-sigfile', sig_file], check=True, stdout=subprocess.DEVNULL)
        return base64.b64encode(sig_file.read_bytes()).decode()


def public_key(key_file):
    """Return the base64 raw Ed25519 public key, the form KPXC_UPDATE_PUBLIC_KEY takes."""
    der = subprocess.run(['openssl', 'pkey', '-in', key_file, '-pubout', '-outform', 'DER'],
                         check=True, capture_output=True).stdout
    return base64.b64encode(der[-32:]).decode()


def appcast(title, link, item):
    attributes = ''.join(f' {k}={quoteattr(v)}' for k, v in item['enclosure'].items())
    extra = ''
    if item.get('minimum_system_version'):
        extra = f'      <sparkle:minimumSystemVersion>{item["minimum_system_version"]}</sparkle:minimumSystemVersion>\n'
    description = ''
    if item.get('notes'):
        description = f'      <description><![CDATA[<pre>{html.escape(item["notes"])}</pre>]]></description>\n'
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<rss version="2.0" xmlns:sparkle="{SPARKLE_NS}">\n'
        '  <channel>\n'
        f'    <title>{escape(title)}</title>\n'
        f'    <link>{escape(link)}</link>\n'
        '    <item>\n'
        f'      <title>{escape(item["title"])}</title>\n'
        f'      <link>{escape(link)}</link>\n'
        f'      <pubDate>{item["date"]}</pubDate>\n'
        f'      <sparkle:version>{escape(item["version"])}</sparkle:version>\n'
        f'      <sparkle:shortVersionString>{escape(item["display_version"])}</sparkle:shortVersionString>\n'
        f'{extra}{description}'
        f'      <enclosure{attributes}/>\n'
        '    </item>\n'
        '  </channel>\n'
        '</rss>\n'
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', type=Path, help='Directory with the packages; the feeds are written here too.')
    parser.add_argument('--channel', required=True, choices=['stable', 'nightly'])
    parser.add_argument('--version', required=True, help='Version shown to users, e.g. 2.8.0-nightly-20261025.')
    parser.add_argument('--update-version', required=True,
                        help='Version the updaters compare, e.g. 2.8.0.26102503 (KEEPASSXC_UPDATE_VERSION).')
    parser.add_argument('--download-url', required=True, help='URL the packages are downloaded from, without a file name.')
    parser.add_argument('--release-url', required=True, help='Web page of the release.')
    parser.add_argument('--notes-file', type=Path, help='Plain-text release notes to show in the update dialog.')
    parser.add_argument('--key-env', default='UPDATE_PRIVATE_KEY',
                        help='Environment variable with the PEM Ed25519 private key (default: %(default)s).')
    parser.add_argument('--expect-public-key', help='Fail unless the private key belongs to this public key.')
    args = parser.parse_args()

    key = os.environ.get(args.key_env, '').strip()
    if not key:
        sys.exit(f'{args.key_env} is not set')
    if not re.fullmatch(r'\d+(\.\d+)*', args.update_version):
        sys.exit(f'Update version must be dotted numbers, got {args.update_version}')

    notes = args.notes_file.read_text().strip() if args.notes_file else ''
    date = email.utils.formatdate(usegmt=True)
    written = []

    with tempfile.TemporaryDirectory() as tmp:
        key_file = Path(tmp) / 'key.pem'
        key_file.touch(mode=0o600)
        key_file.write_text(key + '\n')

        if args.expect_public_key and public_key(key_file) != args.expect_public_key.strip():
            sys.exit('The private key does not belong to the public key built into the app')

        for package in sorted(args.packages.iterdir()):
            match = next((p for p in PACKAGES if p[0].match(package.name)), None)
            if not match:
                continue
            _, feed, label, extra = match
            feed_file = args.packages / f'appcast-{feed}.xml'
            if feed_file in written:
                sys.exit(f'Two packages for the {feed} feed, the second is {package.name}')

            item = {
                'title': f'OpenVireo {args.version}',
                'date': date,
                'version': args.update_version,
                'display_version': args.version,
                'notes': notes,
                'enclosure': {
                    'url': f'{args.download_url.rstrip("/")}/{package.name}',
                    'length': str(package.stat().st_size),
                    'type': 'application/octet-stream',
                    'sparkle:edSignature': sign(package, key_file),
                    **extra,
                },
            }
            if feed.startswith('macos-'):
                item['minimum_system_version'] = MACOS_MINIMUM_VERSION
            title = f'OpenVireo {args.channel} updates for {label}'
            feed_file.write_text(appcast(title, args.release_url, item))
            written.append(feed_file)
            print(f'{feed_file.name}: {package.name}')

    if not written:
        sys.exit(f'No packages for any update feed in {args.packages}')


if __name__ == '__main__':
    main()
