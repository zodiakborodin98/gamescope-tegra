#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify and archive only publication sources; never include local builds/downloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'SOURCE-MANIFEST.sha256'
SKIP_DIRS = {'.git', '__pycache__', '_work', 'downloads', 'sysroots', 'build', 'stage', '.scratch'}
PAYLOAD_SUFFIXES = {'.exe', '.dll', '.so', '.o', '.a', '.obj', '.pdb', '.pyc', '.log', '.tgz', '.tar', '.gz', '.xz', '.tbz2', '.zip'}
SECRETS = [
    rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----',
    rb'\bgh[pousr]_[A-Za-z0-9]{36,}\b', rb'\bgithub_pat_[A-Za-z0-9_]{60,}\b',
    rb'\bAKIA[0-9A-Z]{16}\b', rb'\bxox[baprs]-[A-Za-z0-9-]{20,}\b',
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def files(root=ROOT):
    root = Path(root)
    lock = json.loads((root / 'sources.lock.json').read_text())
    deps = {'gamescope/' + item['path'] for item in lock['repositories']}
    result = []
    for base, dirs, names in os.walk(root, followlinks=False):
        relbase = Path(base).relative_to(root)
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS
                         and (relbase / d).as_posix() not in deps)
        for d in dirs:
            if (Path(base) / d).is_symlink():
                raise ValueError('Publication directory symlink: ' + str(relbase / d))
        for name in sorted(names):
            rel = (relbase / name).as_posix()
            if name.endswith('.pyc') or name == '.DS_Store':
                continue
            result.append(rel)
    return sorted(result)


def verify(root=ROOT, *, require_manifest=True):
    root = Path(root)
    inventory = files(root)
    required = ['LICENSE', 'COPYING', 'NOTICE.md', 'README.md', 'docs/BUILD.md',
                'docs/LICENSING.md', 'docs/PROVENANCE.md', 'external-artifacts.lock.json',
                'LICENSES/MIT.txt', 'LICENSES/GPL-2.0.txt', 'gamescope/LICENSE']
    for rel in required:
        if rel not in inventory:
            raise ValueError('Missing required publication file: ' + rel)
    lock = json.loads((root / 'sources.lock.json').read_text())
    for rel, item in lock['source_inputs'].items():
        if sha((root / 'gamescope' / rel).read_bytes()) != item['sha256']:
            raise ValueError('Frozen source hash differs: ' + rel)
    if (root / 'COPYING').read_bytes() != (root / 'LICENSES/GPL-2.0.txt').read_bytes():
        raise ValueError('GPL license copies differ')
    if (root / 'gamescope/LICENSE').read_bytes() != (root / 'LICENSES/Gamescope.upstream.txt').read_bytes():
        raise ValueError('Upstream license copies differ')
    rows = []
    for rel in inventory:
        path = root / rel
        if path.is_symlink():
            raise ValueError('Publication file symlink: ' + rel)
        data = path.read_bytes()
        if data[:4] == b'\x7fELF' or data[:2] == b'MZ':
            raise ValueError('Compiled executable payload: ' + rel)
        if path.suffix.lower() in PAYLOAD_SUFFIXES or '.so.' in path.name:
            raise ValueError('Build/runtime/archive payload: ' + rel)
        for pattern in SECRETS:
            if re.search(pattern, data):
                raise ValueError('Credential pattern: ' + rel)
        if (b'C:' + b'/Users/') in data or (b'C:' + b'\\Users\\') in data:
            raise ValueError('Private Windows workspace path: ' + rel)
        # The single upstream EDID asset is public source input, not a driver.
        if path.suffix == '.bin' and rel != 'gamescope/src/gamescope_base_edid.bin':
            raise ValueError('Unexpected binary asset: ' + rel)
        if rel != MANIFEST:
            rows.append(sha(data) + '  ' + rel)
    expected = '\n'.join(rows) + '\n'
    if require_manifest and (root / MANIFEST).read_text() != expected:
        raise ValueError('Source manifest mismatch; regenerate only after reviewing changes')
    return inventory, expected


def archive(destination, root=ROOT):
    root = Path(root).resolve()
    inventory, _ = verify(root)
    destination = Path(destination).resolve()
    if destination == root or destination.is_relative_to(root):
        raise ValueError('Archive destination must be outside the source folder')
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation prevents replacement of an earlier reviewed package.
    try:
        stream = destination.open('xb')
    except FileExistsError:
        raise ValueError('Archive already exists') from None
    try:
        with stream, zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as out:
            for rel in inventory:
                data = (root / rel).read_bytes()
                info = zipfile.ZipInfo('gamescope-tegra/' + rel, date_time=(2026, 10, 12, 0, 0, 0))
                executable = rel.endswith('.sh') or (rel.endswith('.py') and data.startswith(b'#!'))
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                out.writestr(info, data)
        with zipfile.ZipFile(destination) as check:
            if check.testzip() or len(check.infolist()) != len(inventory):
                raise ValueError('Archive validation failed')
            for rel in inventory:
                if check.read('gamescope-tegra/' + rel) != (root / rel).read_bytes():
                    raise ValueError('Archived bytes differ: ' + rel)
    except BaseException:
        destination.unlink(missing_ok=True)
        raise
    digest = sha(destination.read_bytes())
    print('SOURCE_ARCHIVE_VERIFIED', destination.name, len(inventory), destination.stat().st_size, digest)
    return digest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--write-manifest', action='store_true')
    ap.add_argument('--archive', type=Path)
    args = ap.parse_args()
    if args.write_manifest:
        inventory, content = verify(require_manifest=False)
        (ROOT / MANIFEST).write_text(content, encoding='utf-8', newline='\n')
    inventory, _ = verify()
    print('PUBLICATION_SOURCE_VERIFIED', len(inventory))
    if args.archive:
        archive(args.archive)


if __name__ == '__main__':
    main()
