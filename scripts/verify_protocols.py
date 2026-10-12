#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce all four EGLStream generated sources without modifying frozen files."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'gamescope/src/Backends/tegra_l4t/eglstream'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--scanner', default='wayland-scanner')
    args = ap.parse_args()
    print(subprocess.check_output([args.scanner, '--version'], stderr=subprocess.STDOUT, text=True).strip())
    with tempfile.TemporaryDirectory(prefix='gamescope-tegra-protocols-') as tmp:
        for name in ('wayland-eglstream', 'wayland-eglstream-controller'):
            for mode, suffix in (('private-code', '-protocol.c'), ('server-header', '-server-protocol.h')):
                output = Path(tmp) / (name + suffix)
                subprocess.run([args.scanner, mode, str(SOURCE / 'source' / (name + '.xml')), str(output)], check=True)
                generated = output.read_bytes()
                if generated != (SOURCE / output.name).read_bytes():
                    raise SystemExit('Generated protocol differs: ' + output.name + '; use wayland-scanner 1.26.0')
                print('PROTOCOL_REPRODUCED', output.name, hashlib.sha256(generated).hexdigest())


if __name__ == '__main__':
    main()
