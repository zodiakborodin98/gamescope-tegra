<!-- publication-2026-10-21 -->
**Current 2026-10-21 status:** see [the update](UPDATE-2026-10-21.md). The dated evidence and build results below are historical, not fresh acceptance of the new source snapshot.

# Publication build validation — 2026-10-12

The exported source tree compiled and linked from a **fresh build directory**,
without retained Gamescope object files or the private accepted ELF. Result:
`PC_COMPILE_LINK_PASS`, Ninja **666/666**. No resulting target executable was
run, uploaded or installed on the Switch.

| Item | Measured value |
|---|---|
| Target | AArch64, ARMv8-A, Cortex-A57 |
| Host | x86-64 Linux / WSL Ubuntu 24.04 |
| Compiler | aarch64-linux-gnu GCC 13.3.0 |
| Meson / Ninja | 1.3.2 / 1.11.1 |
| Profile | debug, optimization 0, no LTO |
| Native scanner | Wayland 1.26.0 |
| GLSL generator | glslang 15.1.0 |
| Output size | 40,602,424 bytes |
| Output SHA256 | `b2e027024c70c2a7e19c41fb86f1b855789bcc0b1134884c2ec3bc035046fe1c` |

The two presenter/first-frame shader embeddings match the accepted 6,668-byte
SPIR-V, SHA256 `a3792dd6056456d75f9616d4a137b5d53dbdc40ce0be339f98f84ea4bd2dad04`.
The included GLSL reproduces these bytes; all four generated EGLStream protocol
files reproduce byte-for-byte from the supplied official NVIDIA XML.

The native scanner helper was separately compiled and run in a new local prefix
using the pinned official Wayland source, native GCC and host Expat runtime.
That freshly built scanner reproduced all four EGLStream generated files.
The normal Meson scanner route requires host development packages; in this
environment it refused cleanly because host `expat.pc` was absent. The
documented minimal `--expat-includedir` route was the one qualified here.

Representative target sysroot dependencies: Wayland 1.26.0, protocols 1.49,
Vulkan 1.4.309 headers, X11 1.8.9, libdrm 2.4.134, libeis 1.4.1,
xkbcommon 1.13.2, pixman 0.46.4, libinput 1.26.1, libseat 0.8.0,
libudev 257, lcms2 2.16, libavif 1.0.4 and libcap 2.69. These are measured
reference inputs, not an automatically downloaded universal sysroot. The
runtime NVIDIA driver's Vulkan API remains 1.2.131; newer headers do not
upgrade that runtime capability.

One initial fresh compilation selected an older host scanner through polluted
native pkg-config lookup. The builder now separates native/target metadata,
requires the native prefix for cross builds and verifies protocol generation.
The subsequent fresh build completed successfully. The original development
build and target baseline were not modified.

Downloader and packaging refusal/roundtrip tests: **21/21 passed**, including
hash/size/timeout/host/redirect/collision/atomic-install failures, exclusion of
local builds/dependencies, frozen-input mismatch, credentials and archive
overwrite. The experimental patch passes `git apply --check` on the exported
baseline. A separate read-only license/download review found no remaining
blocking issue after the partial-output downloader fix.

This is compile/link and source-package validation. It does not establish
bit identity with `b9f5847f…`, repair the open EGLStream hardware failures,
qualify a native AArch64 build, or establish new game/visual performance.

## Additional graphics source recipes

The recorded Mesa depth patch passes `git apply --check` on the preserved
unmodified Mesa 26.0.3 source. The Xwayland size-override patch passes reverse
apply-check against the pinned, patched 23.2.7 working source. Neither check
changes those reference trees. Python syntax/help checks passed for both new
graphics scripts. Source-tree receipts reject directory symlinks and distinguish
file types, executable bits and content; the LLVM guard checks all seven AArch64
native definitions. The final Linux host test suite passed **26/26**, including
the original 21 downloader/package cases and five graphics guard cases.

These new component recipes have not been qualified by compiling every graphics
component into a new distribution. Their pins and historical runtime results
are recorded separately from the fresh Gamescope compile/link result above.
