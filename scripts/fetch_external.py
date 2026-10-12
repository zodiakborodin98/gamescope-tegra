#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in official-host downloads with required SHA verification; no extraction/install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import time
from urllib.parse import urlparse
from urllib.request import Request, HTTPRedirectHandler, build_opener

ROOT = Path(__file__).resolve().parents[1]


class OfficialRedirect(HTTPRedirectHandler):
    def __init__(self, hosts):
        super().__init__()
        self.hosts = set(hosts)

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        validate_url(newurl, self.hosts)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def validate_url(url, hosts):
    parsed = urlparse(url)
    if (parsed.scheme != 'https' or parsed.hostname not in hosts or parsed.username or parsed.password
            or parsed.port not in (None, 443)):
        raise ValueError('Not an approved official HTTPS URL')


def copy_verified(stream, target, expected, *, expected_size=None, max_bytes=2*1024**3, deadline=None):
    """Hash incoming bytes before replacing anything; never overwrite an existing target."""
    target = Path(target)
    if target.exists() or target.is_symlink():
        raise ValueError('Destination already exists')
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected or ''):
        raise ValueError('A trusted SHA256 pin is required')
    target.parent.mkdir(parents=True, exist_ok=True)
    name = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.fetch-', delete=False) as out:
            name = Path(out.name)
            digest, count = hashlib.sha256(), 0
            while True:
                if deadline is not None and time.monotonic() > deadline:
                    raise TimeoutError('Download deadline exceeded')
                chunk = stream.read(1024*1024)
                if not chunk:
                    break
                count += len(chunk)
                if count > max_bytes:
                    raise ValueError('Artifact size exceeds bound')
                digest.update(chunk)
                out.write(chunk)
        if digest.hexdigest() != expected.lower() or (expected_size is not None and count != expected_size):
            raise ValueError('Artifact hash/size mismatch; nothing accepted')
        # Same-directory hardlink installs complete verified bytes atomically, without
        # overwrite. Filesystems without hardlink support refuse; no copy fallback.
        os.link(name, target)
        return count
    finally:
        if name is not None:
            name.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('artifact', nargs='?')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--sha256', help='required trusted pin if the manifest has no qualified hash')
    ap.add_argument('--accept-license', action='store_true', help='acknowledge that you have read the listed component terms')
    ap.add_argument('--local-file', type=Path, help='verify an already lawfully obtained file instead of downloading')
    ap.add_argument('--output-dir', type=Path, default=ROOT / 'downloads')
    args = ap.parse_args()
    lock = json.loads((ROOT / 'external-artifacts.lock.json').read_text())
    if args.list:
        for name, item in lock['artifacts'].items():
            print(name, 'PINNED' if item['sha256'] else 'TRUSTED_SHA_REQUIRED', item['url'])
        print('Steam ARM64 client: USER_SUPPLIED, not a build dependency')
        return
    if args.artifact not in lock['artifacts']:
        raise SystemExit('Select a known artifact; use --list')
    item = lock['artifacts'][args.artifact]
    expected = item['sha256'] or args.sha256
    if item['sha256'] and args.sha256 and item['sha256'] != args.sha256.lower():
        raise SystemExit('Cannot override the qualified manifest pin')
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected or ''):
        raise SystemExit('No independently qualified hash. Supply a trusted --sha256; this script never trusts its own first download.')
    if not args.accept_license:
        raise SystemExit('Read ' + item['license_url'] + ' and use --accept-license to acknowledge the terms')
    dest = args.output_dir.resolve() / item['filename']
    deadline = time.monotonic() + 1800
    if args.local_file:
        with args.local_file.open('rb') as src:
            count = copy_verified(src, dest, expected, expected_size=item.get('size'), deadline=deadline)
    else:
        validate_url(item['url'], item['allowed_hosts'])
        opener = build_opener(OfficialRedirect(item['allowed_hosts']))
        req = Request(item['url'], headers={'User-Agent': 'gamescope-tegra-source-preparation/1'})
        with opener.open(req, timeout=30) as response:
            validate_url(response.geturl(), item['allowed_hosts'])
            count = copy_verified(response, dest, expected, expected_size=item.get('size'), deadline=deadline)
    print('EXTERNAL_FILE_VERIFIED', dest, count, expected.lower())
    print('No files were extracted, installed or added to Git.')


if __name__ == '__main__':
    main()
