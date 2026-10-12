#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build one pinned graphics component privately; never install or execute target code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from fetch_graphics import tree_hash

ROOT = Path(__file__).resolve().parents[1]


def validate_llvm_metadata(metadata):
    required = {'LLVM_NATIVE_ARCH': 'AArch64'}
    for macro, initializer in (
        ('ASMPARSER', 'AsmParser'), ('ASMPRINTER', 'AsmPrinter'),
        ('DISASSEMBLER', 'Disassembler'), ('TARGET', 'Target'),
        ('TARGETINFO', 'TargetInfo'), ('TARGETMC', 'TargetMC'),
    ):
        required['LLVM_NATIVE_' + macro] = 'LLVMInitializeAArch64' + initializer
    for macro, value in required.items():
        definitions = re.findall(r'^[ \t]*#[ \t]*define[ \t]+' + macro + r'(?:[ \t]+(.*))?$', metadata, re.M)
        values = [re.sub(r'/\*.*?\*/|//.*', '', definition).strip() for definition in definitions]
        if values != [value]:
            raise SystemExit('Wrong or ambiguous LLVM native metadata: ' + macro)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('component', choices=('mesa', 'xwayland', 'wayland', 'egl-wayland'))
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--sysroot', type=Path, required=True)
    ap.add_argument('--build-dir', type=Path, required=True)
    ap.add_argument('--host-tools', type=Path, required=True)
    ap.add_argument('--profile', choices=('llvmpipe', 'zink'), default='llvmpipe')
    ap.add_argument('--llvm-config', type=Path, help='native executable with target AArch64 library metadata')
    ap.add_argument('--jobs', type=int, default=2)
    args = ap.parse_args()
    source, sysroot, build = args.source.resolve(strict=True), args.sysroot.resolve(strict=True), args.build_dir.resolve()
    if build.exists() or build.is_symlink() or args.jobs < 1:
        raise SystemExit('Fresh build directory and positive jobs required')
    receipt = json.loads((source / '.source-receipt.json').read_text())
    lock = json.loads((ROOT / 'graphics-stack/sources.lock.json').read_text())
    if (receipt['component'] != args.component or receipt['source_pin'] != lock['components'][args.component]
            or receipt['tree_sha256'] != tree_hash(source)):
        raise SystemExit('Source receipt/pin/content differs; no build accepted')
    if not (sysroot / 'usr/include').is_dir() or not (sysroot / 'usr/lib').is_dir():
        raise SystemExit('Incomplete target sysroot')
    native = args.host_tools.resolve(strict=True)
    if not (native / 'bin/wayland-scanner').is_file() or not (native / 'lib/pkgconfig/wayland-scanner.pc').is_file():
        raise SystemExit('Use build_host_tools.py for native Wayland 1.26.0 tools')
    flags = ['-march=armv8-a', '-mtune=cortex-a57', '-mno-outline-atomics', '--sysroot=' + str(sysroot)]
    pkgdirs = [str(sysroot / p) for p in ('usr/lib/pkgconfig', 'usr/lib/aarch64-linux-gnu/pkgconfig', 'usr/share/pkgconfig') if (sysroot / p).is_dir()]
    cross = "[binaries]\nc='aarch64-linux-gnu-gcc'\ncpp='aarch64-linux-gnu-g++'\nar='aarch64-linux-gnu-ar'\nstrip='aarch64-linux-gnu-strip'\npkg-config='pkg-config'\n"
    if args.component == 'mesa' and args.profile == 'llvmpipe':
        if not args.llvm_config:
            raise SystemExit('llvmpipe needs --llvm-config with AArch64 native init metadata and target libraries')
        llvm = args.llvm_config.resolve(strict=True)
        include = Path(subprocess.check_output([str(llvm), '--includedir'], text=True).strip())
        metadata = (include / 'llvm/Config/llvm-config.h').read_text()
        validate_llvm_metadata(metadata)
        cross += 'llvm-config=' + repr(str(llvm)) + '\n'
    cross += "[host_machine]\nsystem='linux'\ncpu_family='aarch64'\ncpu='cortex-a57'\nendian='little'\n[properties]\nneeds_exe_wrapper=true\n"
    cross += 'sys_root=' + repr(str(sysroot)) + '\npkg_config_libdir=' + repr(os.pathsep.join(pkgdirs)) + '\n'
    cross += '[built-in options]\nc_args=' + repr(flags) + '\ncpp_args=' + repr(flags) + '\nc_link_args=' + repr(flags) + '\ncpp_link_args=' + repr(flags) + '\n'
    build.parent.mkdir(parents=True, exist_ok=True)
    machine = build.parent / (build.name + '.cross.ini')
    if machine.exists():
        raise SystemExit('Cross-file collision')
    machine.write_text(cross)
    options = []
    if args.component == 'mesa':
        options = ['-Dgallium-drivers=' + args.profile, '-Dllvm=' + ('enabled' if args.profile == 'llvmpipe' else 'disabled'),
                   '-Dglx=dri', '-Dglvnd=enabled', '-Dplatforms=x11', '-Dopengl=true',
                   '-Dgles1=disabled', '-Dgles2=disabled', '-Degl=disabled', '-Dgbm=disabled',
                   '-Dvulkan-drivers=', '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dlmsensors=disabled',
                   '-Dbuild-tests=false', '-Dtools=', '-Dvideo-codecs=', '-Dzstd=enabled', '-Dexpat=enabled',
                   '-Dxlib-lease=disabled', '-Dgallium-va=disabled', '-Dgallium-rusticl=false']
    elif args.component == 'xwayland':
        options = ['-Dxwayland_eglstream=true', '-Dxvfb=false', '-Ddocs=false', '-Ddevel-docs=false', '-Dxwayland_ei=false']
    elif args.component == 'wayland':
        options = ['-Dlibraries=true', '-Dscanner=false', '-Dtests=false', '-Ddocumentation=false']
    env = os.environ.copy()
    env['PATH'] = str(native / 'bin') + os.pathsep + env.get('PATH', '')
    env['PKG_CONFIG_PATH_FOR_BUILD'] = str(native / 'lib/pkgconfig')
    env['PKG_CONFIG_LIBDIR_FOR_BUILD'] = os.pathsep.join([str(native / 'lib/pkgconfig'), '/usr/lib/x86_64-linux-gnu/pkgconfig', '/usr/share/pkgconfig'])
    env['PKG_CONFIG_PATH'] = ''
    env['PKG_CONFIG_LIBDIR'] = os.pathsep.join(pkgdirs)
    setup = ['meson', 'setup', str(build), str(source), '--cross-file', str(machine), '--buildtype=release', '--wrap-mode=nodownload', '--prefix=/usr', '--libdir=lib'] + options
    for cmd in (setup, ['meson', 'compile', '-C', str(build), '-j', str(args.jobs)]):
        print('+', ' '.join(cmd), flush=True)
        subprocess.run(cmd, check=True, env=env, timeout=3600)
    (build / 'BUILD-RESULT.json').write_text(json.dumps({'result': 'SOURCE_COMPILE_PASS', 'hardware_qualified': False,
          'component': args.component, 'profile': args.profile if args.component == 'mesa' else None,
          'source_tree_sha256': receipt['tree_sha256'], 'source_pin': receipt['source_pin'], 'setup': setup}, indent=2) + '\n')
    print('GRAPHICS_COMPONENT_BUILT_NO_INSTALL', args.component)


if __name__ == '__main__':
    main()
