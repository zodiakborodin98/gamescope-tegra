#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build from a fresh directory and supplied target sysroot; never install or run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def version(program):
    return subprocess.check_output([program, '--version'], text=True).splitlines()[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sysroot', type=Path, help='required for a cross build; target libraries and headers')
    ap.add_argument('--build-dir', type=Path, default=ROOT / '_work/build')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--host-tools', type=Path, help='optional prefix containing native bin/wayland-scanner')
    ap.add_argument('--prefix', default='/usr/local')
    args = ap.parse_args()
    if args.jobs < 1:
        raise SystemExit('--jobs must be positive')
    build = args.build_dir.resolve()
    if build.exists():
        raise SystemExit('Use a fresh build directory; existing output is never reused or deleted')
    source = ROOT / 'gamescope'
    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    for dep in lock['repositories']:
        path = source / dep['path']
        if not (path / '.git').exists():
            if dep.get('optional'):
                continue
            raise SystemExit('Run scripts/fetch_deps.py first: missing ' + dep['path'])
        git = ['git', '-c', 'safe.directory=' + str(path), '-C', str(path)]
        got = subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(git + ['status', '--porcelain', '--untracked-files=no'], text=True).strip()
        if got != dep['commit'] or dirty:
            raise SystemExit('Dependency pin or tracked content differs: ' + dep['path'])
    if args.sysroot and not args.host_tools:
        raise SystemExit('Cross builds require --host-tools with a native Wayland 1.26.0 scanner and its pkg-config metadata')
    env = os.environ.copy()
    if args.host_tools:
        native_bin = args.host_tools.resolve() / 'bin'
        if not (native_bin / 'wayland-scanner').is_file():
            raise SystemExit('Missing native wayland-scanner under --host-tools')
        env['PATH'] = str(native_bin) + os.pathsep + env.get('PATH', '')
        native_pc = args.host_tools.resolve() / 'lib/pkgconfig'
        if not (native_pc / 'wayland-scanner.pc').is_file():
            raise SystemExit('--host-tools needs its native lib/pkgconfig/wayland-scanner.pc')
        env['PKG_CONFIG_PATH_FOR_BUILD'] = str(native_pc)
        env['PKG_CONFIG_LIBDIR_FOR_BUILD'] = os.pathsep.join([
            str(native_pc), '/usr/lib/x86_64-linux-gnu/pkgconfig', '/usr/share/pkgconfig'])
    for tool in ('meson', 'ninja', 'pkg-config', 'glslangValidator', 'wayland-scanner'):
        if not shutil.which(tool, path=env['PATH']):
            raise SystemExit('Missing build tool: ' + tool)
    subprocess.run([sys.executable, str(ROOT / 'scripts/verify_shader.py')], check=True, env=env)
    subprocess.run([sys.executable, str(ROOT / 'scripts/verify_protocols.py')], check=True, env=env)
    build.parent.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(cmd):
        commands.append(list(map(str, cmd)))
        print('+', ' '.join(map(str, cmd)), flush=True)
        subprocess.run(list(map(str, cmd)), check=True, env=env)

    configure = ['meson', 'setup', str(build), str(source), '--buildtype=debug',
                 '--wrap-mode=nodownload', '--prefix=' + args.prefix,
                 '-Denable_openvr_support=false', '-Dsdl2_backend=disabled',
                 '-Denable_tests=false', '-Dbenchmark=disabled', '-Dpipewire=disabled',
                 '-Dforce_fallback_for=libliftoff,vkroots']
    if args.sysroot:
        sysroot = args.sysroot.resolve(strict=True)
        if not (sysroot / 'usr/include').is_dir() or not (sysroot / 'usr/lib').is_dir():
            raise SystemExit('Incomplete target sysroot')
        for tool in ('aarch64-linux-gnu-gcc', 'aarch64-linux-gnu-g++', 'aarch64-linux-gnu-ar'):
            if not shutil.which(tool, path=env['PATH']):
                raise SystemExit('Missing cross compiler: ' + tool)
        probe = subprocess.run(['aarch64-linux-gnu-g++', '-E', '-x', 'c++', '-v', '/dev/null'],
                               capture_output=True, text=True, check=True)
        wrappers = [Path(x.strip()) / 'math.h' for x in probe.stderr.splitlines()
                    if '/include/c++/' in x and (Path(x.strip()) / 'math.h').is_file()]
        if not wrappers:
            raise SystemExit('Cannot find cross libstdc++ math.h wrapper')
        cflags = ['-march=armv8-a', '-mtune=cortex-a57', '--sysroot=' + str(sysroot)]
        cppflags = cflags + ['-include', 'utility', '-include', str(wrappers[0])]
        pkgdirs = [str(sysroot / p) for p in ('usr/lib/pkgconfig', 'usr/lib/aarch64-linux-gnu/pkgconfig',
                                              'usr/share/pkgconfig') if (sysroot / p).is_dir()]
        cross = build.parent / (build.name + '.cross.ini')
        if cross.exists():
            raise SystemExit('Machine-file collision: ' + str(cross))
        cross.write_text("[binaries]\nc='aarch64-linux-gnu-gcc'\ncpp='aarch64-linux-gnu-g++'\n"
                         "ar='aarch64-linux-gnu-ar'\nstrip='aarch64-linux-gnu-strip'\npkg-config='pkg-config'\n"
                         "[host_machine]\nsystem='linux'\ncpu_family='aarch64'\ncpu='cortex-a57'\nendian='little'\n"
                         "[properties]\nneeds_exe_wrapper=true\nsys_root=" + repr(str(sysroot)) + '\n'
                         + 'pkg_config_libdir=' + repr(os.pathsep.join(pkgdirs)) + '\n'
                         + '[built-in options]\nc_args=' + repr(cflags) + '\ncpp_args=' + repr(cppflags)
                         + '\nc_link_args=' + repr(cflags) + '\ncpp_link_args=' + repr(cflags) + '\n')
        configure += ['--cross-file', str(cross)]
        env['PKG_CONFIG_PATH'] = ''
        env['PKG_CONFIG_LIBDIR'] = os.pathsep.join(pkgdirs)
    else:
        if platform.machine().lower() not in ('aarch64', 'arm64'):
            raise SystemExit('On x86 hosts supply --sysroot; native Tegra build requires AArch64')
        configure += ["-Dc_args=-march=armv8-a -mtune=cortex-a57",
                      "-Dcpp_args=-march=armv8-a -mtune=cortex-a57 -include utility -include math.h"]
    run(configure)
    run(['meson', 'compile', '-C', str(build), '-j', str(args.jobs)])
    binary = build / 'src/gamescope'
    if not binary.is_file():
        raise SystemExit('Build did not produce Gamescope')
    header = (source / 'src/Backends/tegra_l4t/gob_swizzle_spv.h').read_text()
    import re
    import struct
    values = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{8})U', header)]
    shader = struct.pack('<%dI' % len(values), *values)
    blob = binary.read_bytes()
    if blob.count(shader) != 2:
        raise SystemExit('Expected the accepted shader in both presenter and first-frame TUs')
    manifest = {'result': 'PC_COMPILE_LINK_PASS', 'hardware_validated': False,
                'binary_sha256': hashlib.sha256(blob).hexdigest(), 'binary_size': len(blob),
                'reference_elf_sha256': lock['accepted_reference_elf_sha256'],
                'bit_identical_to_historical_reference': hashlib.sha256(blob).hexdigest() == lock['accepted_reference_elf_sha256'],
                'buildtype': 'debug', 'optimization': 0, 'shader_occurrences': 2,
                'source_input_hashes': {p: hashlib.sha256((source / p).read_bytes()).hexdigest()
                                        for p in lock['source_inputs']},
                'dependency_commits': {d['path']: d['commit'] for d in lock['repositories']
                                       if (source / d['path'] / '.git').exists()},
                'commands': commands, 'tools': {x: version(x) for x in ('meson', 'ninja', 'aarch64-linux-gnu-g++')}
                if args.sysroot else {x: version(x) for x in ('meson', 'ninja')}}
    (build / 'BUILD-RESULT.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
