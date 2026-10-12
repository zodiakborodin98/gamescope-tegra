#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch exact open-source dependency commits. No install or target access."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kw):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kw)


def revision(path):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + str(path), '-C', str(path),
                                    'rev-parse', 'HEAD'], text=True).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mirror-root', type=Path, help='optional existing Gamescope source with subproject clones')
    ap.add_argument('--offline', action='store_true', help='refuse network; require matching local mirrors')
    ap.add_argument('--include-openvr', action='store_true')
    ap.add_argument('--include-optional', action='store_true', help='also fetch libdisplay-info and OpenVR fallbacks')
    args = ap.parse_args()
    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    for item in lock['repositories']:
        if item.get('optional') and not args.include_optional and not (args.include_openvr and item['path'] == 'subprojects/openvr'):
            continue
        dest = ROOT / 'gamescope' / item['path']
        if dest.exists() and (dest / '.git').exists():
            if revision(dest) != item['commit']:
                raise SystemExit('Existing dependency has a different commit: ' + str(dest))
            diff = subprocess.check_output(['git', '-c', 'safe.directory=' + str(dest), '-C', str(dest),
                                             'status', '--porcelain', '--untracked-files=no'], text=True)
            if diff.strip():
                raise SystemExit('Tracked dependency modifications: ' + str(dest))
        else:
            if dest.exists() and any(dest.iterdir()):
                raise SystemExit('Refusing nonempty dependency destination: ' + str(dest))
            origin = item['url']
            if args.mirror_root:
                mirror = (args.mirror_root / item['path']).resolve()
                if mirror.is_dir() and (mirror / '.git').exists():
                    if revision(mirror) != item['commit']:
                        raise SystemExit('Local mirror revision differs: ' + str(mirror))
                    origin = str(mirror)
                elif args.offline:
                    raise SystemExit('Missing offline mirror: ' + str(mirror))
            elif args.offline:
                raise SystemExit('Offline fetching requires --mirror-root')
            dest.parent.mkdir(parents=True, exist_ok=True)
            run(['git', 'clone', '--no-hardlinks', '--no-checkout', '--', origin, dest])
            run(['git', '-C', dest, 'checkout', '--detach', item['commit']])
            if revision(dest) != item['commit']:
                raise SystemExit('Fetched revision differs: ' + str(dest))
        package = item.get('meson_packagefiles')
        if package:
            for src in (ROOT / 'gamescope/subprojects/packagefiles' / package).iterdir():
                dst = dest / src.name
                if dst.exists() and dst.read_bytes() != src.read_bytes():
                    raise SystemExit('Refusing different packagefile: ' + str(dst))
                if not dst.exists():
                    shutil.copyfile(src, dst)
        print('PIN OK', item['path'], item['commit'])
    print('DEPENDENCIES_READY')


if __name__ == '__main__':
    main()
