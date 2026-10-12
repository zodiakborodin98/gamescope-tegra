# Graphics stack source recipes

The compositor, client GL, X server and Wayland/EGLStream libraries are separate
components. This directory records their source pins and actual project patches.
It does not flatten upstream repositories or redistribute closed NVIDIA software.

| Component | Source | Status |
|---|---|---|
| Gamescope Tegra | in-tree reconstruction, see root source lock | fresh source compile/link PASS; new ELF hardware acceptance pending |
| Mesa client | signed release 26.0.3, locked archive SHA | historical private llvmpipe8+depthfix2 functional/human UI PASS; slow CPU fallback |
| Zink | Mesa 26.0.3 upstream build profile | experimental; no working Steam GPU/Zink compatibility claim |
| Xwayland | xserver23.2.7 commit9b10be99 + size override patch | historical EGLStream and240p UI/720p-game path accepted |
| Wayland |1.26.0 commit87cc8a87 | native scanner source reproduced; target library depends on user sysroot |
| egl-wayland | NVIDIA1.1.23 commit2a953fc8 | preferred protocol XML reproduced; runtime requires compatible NVIDIA EGL vendor |
| LLVM | compatible target shared LLVM19.1 | native AArch64 init metadata required; not rebuilt from scratch by default |

Fetch a component into a **new local source directory**:
```sh
python3 scripts/fetch_graphics.py mesa
python3 scripts/fetch_graphics.py xwayland
python3 scripts/fetch_graphics.py wayland
python3 scripts/fetch_graphics.py egl-wayland
```
The fetcher verifies the tar hash or Git commit, checks patch hashes, applies
patches and records a whole source-tree receipt. The builder refuses altered
source/pin receipts. All fetched sources and build output stay in ignored `_work/`.

Build using an already compatible AArch64 development sysroot and native scanner:
```sh
python3 scripts/build_graphics.py xwayland \
  --source _work/graphics-sources/xwayland --sysroot /path/to/sysroot \
  --host-tools _work/host-tools --build-dir _work/build-xwayland
python3 scripts/build_graphics.py mesa --profile llvmpipe \
  --source _work/graphics-sources/mesa --sysroot /path/to/sysroot \
  --llvm-config /path/to/qualified-cross-aware-llvm-config \
  --host-tools _work/host-tools --build-dir _work/build-mesa-llvmpipe
```
`wayland` and `egl-wayland` use the same generic arguments. Meson dependencies,
including Xtrans and wayland-eglstream-protocols metadata for Xwayland, must exist
in the target sysroot; missing prerequisites refuse. Mesa needs Meson>=1.4,
Mako/PyYAML/packaging, flex/bison/m4 and target development libraries.
The commands never install libraries, launch Steam, change global providers or
run target executables. This publication adds portable recipes; those new recipes
have not all been compiled as a new whole-stack distribution. Historical builds
do not transfer their bit identity or device acceptance automatically.

## LLVM native init correction

The historical llvmpipe failure came from **host X86 generated LLVM metadata**,
not a missing AArch64 target implementation. The isolated metadata patch changes
only `LLVM_NATIVE_ARCH` and six native initializer macros to AArch64. Apply it to
an owned copied include tree, never to system headers. A task-local llvm-config
wrapper may redirect only include-related output to that tree; library/link
metadata must continue to identify the compatible target AArch64 LLVM closure.
The builder requires that qualified wrapper and rejects X86 native metadata.
Do not link host x86 libraries or rebuild LLVM wholesale to hide this mismatch.

## Presentation and provider scope

The Mesa depth patch uses the **actual server-reported drawable depth** for drisw
XImage creation. It does not spoof visuals, capabilities or GLX directness.
The Xwayland patch allows `XWL_SCREEN_SIZE_OVERRIDE=426x240`, preserving a larger
compositor game output; unset preserves upstream dimensions.
The private Xwayland launcher is in `integration/xwayland-eglstream-wrapper.sh`:
it selects the NVIDIA EGL vendor in that child and its private compatible GBM path.
Mesa/LLVM software-client variables remain scoped to the Steam child.

Closed NVIDIA ICD/EGL/core/producer libraries are external, licensed inputs.
Driver filename `libGLX_nvidia.so.0` can represent a Vulkan ICD; that does not imply
a working client GLX frontend. Client Zink under the accepted Xwayland still has
unresolved init/Steam semantics. Do not equate its source profile with the FEX
GPU/Wayland synthetic successes described in the research repository.

Original packaging is GPL-2.0-or-later. Mesa, X.Org, Wayland, egl-wayland and LLVM
retain their actual upstream/per-file licenses. The LLVM metadata patch is an
adaptation of generated upstream metadata, not a relicensing of LLVM.
