#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Recompile the exact accepted GLSL and compare all embedded SPIR-V words."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import hashlib

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'gamescope/src/Backends/tegra_l4t'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler', default='glslangValidator')
    args = ap.parse_args()
    header = (SRC / 'gob_swizzle_spv.h').read_text()
    words = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{8})U', header)]
    reference = struct.pack('<%dI' % len(words), *words)
    with tempfile.TemporaryDirectory(prefix='gamescope-tegra-shader-') as tmp:
        out = Path(tmp) / 'gob.spv'
        subprocess.run([args.compiler, '-V', '--target-env', 'vulkan1.1', '-o', str(out),
                        str(SRC / 'gob_swizzle_sx1.comp')], check=True)
        compiled = out.read_bytes()
    if compiled != reference:
        raise SystemExit('Shader differs from the accepted embedded words; no source/header replacement was made. '
                         'Use the documented glslang 15.1.0 toolchain.')
    print('SHADER_REPRODUCED', len(compiled), hashlib.sha256(compiled).hexdigest())


if __name__ == '__main__':
    main()
