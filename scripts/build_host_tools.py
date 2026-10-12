#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build Wayland 1.26.0 scanner into a task-local prefix, never a system prefix."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PIN = '87cc8a8728a923fc57938faa81ba0e74f34ecdc7'
URL = 'https://gitlab.freedesktop.org/wayland/wayland.git'


def run(cmd):
    print('+', ' '.join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, help='existing exact Wayland source; read-only input')
    ap.add_argument('--prefix', type=Path, default=ROOT / '_work/host-tools')
    ap.add_argument('--build-dir', type=Path, default=ROOT / '_work/build-host-scanner')
    ap.add_argument('--expat-includedir', type=Path,
                    help='optional Expat header directory for a minimal native compiler build; links only host libexpat.so.1')
    args = ap.parse_args()
    prefix, build = args.prefix.resolve(), args.build_dir.resolve()
    if prefix.exists() or build.exists():
        raise SystemExit('Use new prefix and build paths; existing paths are not overwritten/deleted')
    if any(prefix.is_relative_to(Path(p)) for p in ('/usr', '/etc', '/bin', '/sbin', '/lib')):
        raise SystemExit('System prefixes are forbidden')
    if args.source:
        source = args.source.resolve(strict=True)
    else:
        source = ROOT / '_work/wayland-1.26.0'
        if source.exists():
            raise SystemExit('Source collision; supply an existing exact source with --source')
        source.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '--depth', '1', '--branch', '1.26.0', '--', URL, source])
    got = subprocess.check_output(['git', '-c', 'safe.directory=' + str(source), '-C', str(source),
                                   'rev-parse', 'HEAD'], text=True).strip()
    if got != PIN:
        raise SystemExit('Wayland source pin differs')
    run(['git', '-c', 'safe.directory=' + str(source), '-C', source, 'diff', 'HEAD', '--exit-code'])
    if args.expat_includedir:
        import shutil
        headers = args.expat_includedir.resolve(strict=True)
        build.mkdir(parents=True)
        for name in ('expat.h', 'expat_external.h'):
            shutil.copyfile(headers / name, build / name)
        template = (source / 'src/wayland-version.h.in').read_text()
        for key, value in {'WAYLAND_VERSION_MAJOR': '1', 'WAYLAND_VERSION_MINOR': '26',
                           'WAYLAND_VERSION_MICRO': '0', 'WAYLAND_VERSION': '1.26.0'}.items():
            template = template.replace('@' + key + '@', value)
        (build / 'wayland-version.h').write_text(template)
        run(['cc', '-O2', '-I' + str(build), '-I' + str(source / 'src'),
             source / 'src/scanner.c', source / 'src/wayland-util.c',
             '-o', build / 'wayland-scanner', '-l:libexpat.so.1', '-lm'])
        (prefix / 'bin').mkdir(parents=True)
        (prefix / 'lib/pkgconfig').mkdir(parents=True)
        shutil.copyfile(build / 'wayland-scanner', prefix / 'bin/wayland-scanner')
        (prefix / 'bin/wayland-scanner').chmod(0o755)
        (prefix / 'lib/pkgconfig/wayland-scanner.pc').write_text(
            'prefix=' + str(prefix) + '\nexec_prefix=${prefix}\nbindir=${prefix}/bin\n'
            'Name: wayland-scanner\nDescription: native Wayland scanner\nVersion: 1.26.0\n'
            'wayland_scanner=${bindir}/wayland-scanner\n')
    else:
        run(['meson', 'setup', build, source, '--prefix=' + str(prefix), '--buildtype=release',
             '--libdir=lib', '-Ddocumentation=false', '-Dtests=false', '-Dlibraries=false', '-Dscanner=true'])
        run(['meson', 'compile', '-C', build, '-j', '2'])
        run(['meson', 'install', '-C', build])
    run([prefix / 'bin/wayland-scanner', '--version'])
    print('HOST_SCANNER_READY', prefix)


if __name__ == '__main__':
    main()
