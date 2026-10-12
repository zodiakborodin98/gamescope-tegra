# Tegra source provenance

Publication snapshot prepared 2026-10-12. The immutable build-input inventory
is `sources.lock.json`. This document provides attribution that older frozen
source comments sometimes reference through private development paths.
Those old path strings are historical provenance, not missing public build
dependencies. Frozen accepted translation-unit bytes are not changed merely
to rewrite a comment.

## Gamescope and accepted port

* Upstream: ValveSoftware/gamescope
  `6867f509874f9bc52e12d6f4c4596cdf0d5be6b4`.
* Port source snapshot: `d0a5dbfde9210adf1ec8b42055a9388a10a5c7be`.
* Accepted source overrides: recorded by file in `sources.lock.json`, including
  wlserver `834f9d3`, EGLStream consumer `0097b58`, original Version.cpp, and the
  retained orientation180/sx1 presenter, first-frame, GLSL and generated header.
* Historical accepted ELF reference:
  `b9f5847fe6378204126ba4beaa1b3c75b2b42831a4e1b91ebb3370d23723d3d8`.
  This executable is not bundled and its previous acceptance is not a new
  build or hardware qualification.

The old in-tree `gamescope/src/Backends/tegra_l4t/PROVENANCE.md` records the
development chain through `tegra-present-probe`. The public tree contains the
actual required Tegra source/headers and the exact preferred GLSL, rather than
requiring that private project directory to exist.

## Direct display / GOB material

Reference:
[theofficialgman/vk_layer_tegra_dc_present](https://github.com/theofficialgman/vk_layer_tegra_dc_present)
at `ecfb76aadbdc93cd7008e77bec944724cd70c364`.

* Tegra DC UAPI header was copied from this pin, with its NVIDIA copyright,
  Robert Morell/Erik Gilling attribution and GPLv2-or-later terms intact.
* GOB block addressing/swizzle and BGRA packing were copied/adapted from its
  shader. Rotation handling and accepted sx1 compensation are project changes.
  The shader retains GPL-2.0-or-later; the original project's main source
  attribution is reproduced in `NOTICE.md`.
* First-frame and persistent presenter reuse the accepted probe's GOB math,
  DC open/GET_WINDOW/FLIP4, display-head control, close-while-owning and fence
  retirement logic, adapted to the existing Gamescope VkDevice/queue.

The public preferred shader is
`gamescope/src/Backends/tegra_l4t/gob_swizzle_sx1.comp`, SHA256
`393360ed13071b443f640774455e75ea7f5684a593172bd33369edfdc098eba6`.
Generated `gob_swizzle_spv.h`, SHA256
`1104ff4d565311f1ead2d972765261c5f38cb7568c0ca8b9f3f0f4b643c89732`,
is retained from the same accepted source snapshot. Build tooling is responsible
for generation verification; the generated header has the source's license.

## Switchdeck reference in the broader project

The Steam integration work used
[SildurFX/Switchdeck](https://github.com/SildurFX/Switchdeck) at
`be0578fbf00c5a14a988a830f914f2c34cdaa1a1`. Relevant reference scripts are
`install-steam.sh` and the launch/update scripts in its `files/` tree. Their
roles include the Steam ARM64 setup, Proton/FEX release selection, Box64 setup,
and DXVK-Sarek/VKD3D compatibility recipes. The source of the historical FEX
tool was Switchdeck-Extras release `Proton-11.0-1-Armv8.0`.

Switchdeck is credited as a project foundation/reference, not as the author
of Valve's compositor or gman's Tegra DC/GOB code. Its scripts, Steam resources
and compatibility-tool payloads are external to this source-only publication.
The checked-out Switchdeck `LICENSE`/`NOTICE.txt` identify the scripts as
GPL-3.0 and distinguish proprietary Steam payloads. The public credits preserve
that separation; an acknowledgement does not grant a different component license.

## Switchroot kernel/BSP platform

The physical Switch ran the Switchroot Linux/L4T kernel/BSP platform. Historical
baseline records identify kernel `4.9.140-l4t #149` and BSP `5.1.2-34+24.04`.
Credit belongs to Switchroot and its kernel/boot/hardware contributors; these
are platform prerequisites, not new Gamescope code authored by this project.

Official community/source references:

- [Switchroot Linux](https://wiki.switchroot.org/wiki/linux).
- [Switchroot source and contributor listing](https://wiki.switchroot.org/wiki/linux/l4t-ubuntu-jammy-installation-guide).
- [Kernel build scripts](https://github.com/theofficialgman/l4t-kernel-build-scripts).
- [Image build scripts](https://github.com/theofficialgman/l4t-image-buildscripts).
- [L4T Debian source packaging](https://github.com/theofficialgman/l4t-debs-source).
- [CTCaer Tegra kernel tree](https://github.com/CTCaer/switch-l4t-kernel-nvidia).
- [Kernel reference tree used here](https://github.com/theofficialgman/switch-l4t-kernel-nvidia),
  pinned `7d95822acda1f6dab3f3d1d099b43ff9e0d0e626`.

The last pin is **not proven to be the exact source of running build #149**.
The available evidence establishes kernel/config identity, not that source
commit. This publication includes required userspace interface definitions,
not a kernel/firmware distribution. It introduces no kernel patch or build.

## Kernel interface definitions

`nvhost_ioctl.h` was copied from the development reference checkout named
`tegra-kernel-reference` at
`7d95822acda1f6dab3f3d1d099b43ff9e0d0e626`,
`include/uapi/linux/nvhost_ioctl.h`. Its actual NVIDIA GPL version 2 or later
notice is preserved. This local source pin is not evidence of the running
Switch kernel build or a promised fetchable upstream URL. The exact header is
included so building this source package does not depend on that missing URL.

Legacy Android fence-info ABI definitions were adapted from
[OE4T/linux-tegra-4.9 sync UAPI](https://github.com/OE4T/linux-tegra-4.9/blob/78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e/drivers/staging/android/uapi/sync.h)
at `78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e` (GPL-2.0 kernel UAPI,
Copyright 2012 Google, Inc.), file SHA256
`d90f5f6a2e77371d9de7ba91595883ceb84e302b98453e37db09f7124679d9eb`.
Field layout and ioctl value remain ABI-identical; type prefixes and omission
of trailing flexible-array declarations are adaptations, not verbatim copying.
The modern `linux/sync_file.h` include is supplied by the user's target sysroot.

## Protocols

The private OPAQUE_FD transport protocol XML in
`gamescope/protocol/tegra-l4t-buffer.xml` is project work with its existing
permission notice; the server transport TU is GPL-2.0-or-later.
Gamescope swapchain and other upstream protocols retain their own notices.

EGLStream server C/header files were generated with wayland-scanner 1.26.0 from
the NVIDIA `wayland-eglstream.xml` and `wayland-eglstream-controller.xml`
definitions. Their NVIDIA MIT notices are preserved in generated outputs.
The preferred inputs are included at
`gamescope/src/Backends/tegra_l4t/eglstream/source/`, from official
[NVIDIA/egl-wayland](https://github.com/NVIDIA/egl-wayland) release 1.1.23,
commit `2a953fc86ca5707466ff28c117b0b72de87d9316`, paths
`wayland-eglstream/wayland-eglstream.xml` and
`wayland-eglstream/wayland-eglstream-controller.xml`.

| Input | SHA256 of original upstream bytes |
|---|---|
| `wayland-eglstream.xml` | `e2f3594aff1a683e15f4290ce014ae0bc19261ea18e258b8138b8ff083750de4` |
| `wayland-eglstream-controller.xml` | `07012116ffcb426074acdb431fa32ab245cc02fd6aff27fb3ecf2dd73805dd66` |

Both retain their complete NVIDIA copyright and MIT permission/disclaimer
blocks. Their official git blobs agree with the historical build's protocol
prefix inputs. No speculative reconstructed XML or third-party mirror was used.
Do not replace the NVIDIA protocol notice with the project's copyright.

Regenerate from the `eglstream/` directory into a fresh output directory;
do not overwrite a frozen output during qualification:

```sh
wayland-scanner private-code source/wayland-eglstream.xml "$OUT/wayland-eglstream-protocol.c"
wayland-scanner server-header source/wayland-eglstream.xml "$OUT/wayland-eglstream-server-protocol.h"
wayland-scanner private-code source/wayland-eglstream-controller.xml "$OUT/wayland-eglstream-controller-protocol.c"
wayland-scanner server-header source/wayland-eglstream-controller.xml "$OUT/wayland-eglstream-controller-server-protocol.h"
```

The reproduction used **wayland-scanner 1.26.0** on the PC. All four outputs
matched the frozen files **byte-for-byte**, covering the complete interface,
message/signature/version metadata and generated server declarations, not just
matching interface names. Frozen source/header files were verified unchanged.

| Reproduced output | SHA256 (frozen = generated) |
|---|---|
| `wayland-eglstream-protocol.c` | `d9b4627c2f57e92fd85de09ab91a59464924e0eb550965242d9cb89ff48fd141` |
| `wayland-eglstream-server-protocol.h` | `02575c656483cd8e20c2bc1dc959b4230373d075a4247388f1be45150e01cca6` |
| `wayland-eglstream-controller-protocol.c` | `4fad68d5bcd34c08a65d255899848858fdc45544470e4027b8b77cac7be892de` |
| `wayland-eglstream-controller-server-protocol.h` | `92f25b9b26a3308b0d9a56cbc666fbe22e4811d369e0ec19a7eafba4d1216a53` |

## Experimental lifetime change

`patches/experimental/eglstream-lifetime.patch` is a project modification of
the GPL Tegra consumer and corresponding Gamescope integration. It is kept
separate from the historical accepted source snapshot. Its packaging does not
imply that it fixed the observed hardware consumer failure or has passed an
independent new hardware qualification. Original source licenses remain intact.

## Publication boundary

Fetched upstream dependency pins and source-input hashes are in
`sources.lock.json`. License notices accompany source, not private test logs.
This source package excludes private runtime/driver/Steam payloads and does
not import the private development repository's git history.

