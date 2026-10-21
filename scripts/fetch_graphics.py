#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch pinned open graphics sources and apply the recorded private patches; no install/run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time
from urllib.request import Request, build_opener

from fetch_external import OfficialRedirect, copy_verified, validate_url

ROOT = Path(__file__).resolve().parents[1]


def tree_hash(source):
    source = Path(source).resolve()
    result = hashlib.sha256()

    def link_bytes(path):
        target = os.readlink(path)
        if Path(target).is_absolute() or not path.resolve().is_relative_to(source):
            raise ValueError('Source symlink escapes source tree: ' + path.relative_to(source).as_posix())
        return target.encode()

    for base, dirs, files in os.walk(source, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d != '.git')
        links = []
        for name in dirs:
            path = Path(base) / name
            if path.is_symlink():
                data = link_bytes(path)
                result.update(b'symlink\0' + path.relative_to(source).as_posix().encode()
                              + b'\0' + str(len(data)).encode() + b'\0' + data + b'\0')
                links.append(name)
                continue
            result.update(b'directory\0' + path.relative_to(source).as_posix().encode() + b'\0')
        dirs[:] = [name for name in dirs if name not in links]
        for name in sorted(files):
            path = Path(base) / name
            rel = path.relative_to(source).as_posix()
            if rel == '.source-receipt.json':
                continue
            kind = b'symlink' if path.is_symlink() else b'file'
            data = link_bytes(path) if path.is_symlink() else path.read_bytes()
            executable = b'1' if not path.is_symlink() and path.stat().st_mode & 0o111 else b'0'
            result.update(kind + b'\0' + rel.encode() + b'\0' + executable + b'\0')
            result.update(str(len(data)).encode() + b'\0' + data + b'\0')
    return result.hexdigest()


def apply_locked_patches(dest, item):
    dest = Path(dest).resolve()
    patch_env = os.environ.copy()
    # A tar source below this checkout must not inherit its Git attributes or
    # repository context. An actual upstream .git in dest still takes precedence.
    patch_env['GIT_CEILING_DIRECTORIES'] = str(dest.parent)
    for name in item['patches']:
        patch = ROOT / 'graphics-stack/patches' / name
        if hashlib.sha256(patch.read_bytes()).hexdigest() != item['patch_sha256'][name]:
            raise SystemExit('Patch hash differs: ' + name)
        apply = ['git', '-c', 'core.autocrlf=false', '-c', 'safe.directory=' + str(dest), '-C', str(dest), 'apply']
        subprocess.run(apply + ['--check', str(patch)], check=True, timeout=30, env=patch_env)
        subprocess.run(apply + [str(patch)], check=True, timeout=30, env=patch_env)
    for rel, want in item.get('patched_sources_sha256', {}).items():
        path = dest / rel
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != want:
            raise SystemExit('Patched preferred source differs: ' + rel)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    lock = json.loads((ROOT / 'graphics-stack/sources.lock.json').read_text())
    ap.add_argument('component', choices=lock['components'])
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    dest = (args.output or ROOT / '_work/graphics-sources' / args.component).resolve()
    if dest.exists() or dest.is_symlink():
        raise SystemExit('Use a new source directory; no overwrite or implicit update')
    item = lock['components'][args.component]
    dest.parent.mkdir(parents=True, exist_ok=True)
    if item['kind'] == 'tar':
        hosts = {'archive.mesa3d.org'}
        validate_url(item['url'], hosts)
        with tempfile.TemporaryDirectory(prefix='graphics-download-', dir=dest.parent) as temporary:
            archive = Path(temporary) / 'source.tar.xz'
            opener = build_opener(OfficialRedirect(hosts))
            with opener.open(Request(item['url']), timeout=30) as response:
                validate_url(response.geturl(), hosts)
                copy_verified(response, archive, item['sha256'], expected_size=item['size'],
                              max_bytes=item['size'], deadline=time.monotonic() + 600)
            expanded = Path(temporary) / 'expanded'
            expanded.mkdir()
            with tarfile.open(archive) as source:
                source.extractall(expanded, filter='data')
            children = list(expanded.iterdir())
            if len(children) != 1 or not children[0].is_dir() or children[0].is_symlink():
                raise SystemExit('Unexpected archive source root')
            if dest.exists():
                raise SystemExit('Source destination appeared; refusing')
            children[0].rename(dest)
    else:
        subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', '--', item['url'], str(dest)], check=True, timeout=600)
        git = ['git', '-c', 'safe.directory=' + str(dest), '-C', str(dest)]
        subprocess.run(git + ['fetch', '--depth', '1', 'origin', item['commit']], check=True, timeout=600)
        subprocess.run(git + ['checkout', '--detach', item['commit']], check=True, timeout=600)
        actual = subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip()
        if actual != item['commit']:
            raise SystemExit('Source commit differs')
    apply_locked_patches(dest, item)
    receipt = {'component': args.component, 'source_pin': item, 'tree_sha256': tree_hash(dest),
               'hardware_qualified': False}
    (dest / '.source-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('GRAPHICS_SOURCE_READY', args.component, receipt['tree_sha256'])


if __name__ == '__main__':
    main()
