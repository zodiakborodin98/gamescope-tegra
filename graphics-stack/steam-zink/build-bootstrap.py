#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the native early-ICD shim privately; no installation or execution."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
P=Path(__file__).resolve().parent

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sysroot',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    root=args.sysroot.resolve(strict=True);out=args.output.resolve()
    if out.exists() or out.is_symlink():raise SystemExit('Refusing output collision')
    if not (root/'usr/include/vulkan/vulkan_core.h').is_file():raise SystemExit('Vulkan headers required in target sysroot')
    out.parent.mkdir(parents=True,exist_ok=True);temp=out.with_name(out.name+'.part')
    if temp.exists():raise SystemExit('Refusing temporary collision')
    command=['aarch64-linux-gnu-gcc','--sysroot='+str(root),'-march=armv8-a','-mtune=cortex-a57','-mno-outline-atomics','-O2','-fPIC','-shared',str(P/'early-icd-forkexec.c'),'-ldl','-pthread','-o',str(temp)]
    try:
        subprocess.run(command,check=True,timeout=180)
        data=temp.read_bytes()
        if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:raise ValueError('Wrong ELF architecture')
        if b'steam-zink-forkexec-v3' not in data:raise ValueError('Missing bootstrap marker')
        os.replace(temp,out)
    finally:temp.unlink(missing_ok=True)
    print(json.dumps({'result':'SOURCE_COMPILE_PASS','sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'hardware_qualified':False}))

if __name__=='__main__':main()
