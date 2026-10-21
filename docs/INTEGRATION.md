<!-- publication-2026-10-21 -->
**Current 2026-10-21 status:** see [the update](UPDATE-2026-10-21.md). The dated evidence and build results below are historical, not fresh acceptance of the new source snapshot.

# Integrating with an existing Switch Linux installation

This repository is not a rootfs/driver/Steam installer. Start from your own
working Switch Linux stack with the compatible L4T 32.7.4 kernel/userspace,
NVIDIA device nodes and a known recovery path. Preserve the existing compositor
and startup configuration before testing a new source build.

Historical prerequisites, deliberately external to this source package:

- Compatible NVIDIA Vulkan/EGL userspace and the required `libnvvulkan-producer.so`
  reachable inside the selected runtime's loader closure.
- An EGLStream-capable Xwayland (the experiment used xwayland-eglstream-derived
  sources and a private GBM/EGL wrapper), not an arbitrary stock Xwayland.
- For 426x240 Steam UI independent from 1280x720 games, the separate Xwayland
  `XWL_SCREEN_SIZE_OVERRIDE` patch; this option is not a Gamescope feature.
- A lawful Steam ARM64 installation and the working private client Mesa/LLVM
  closure. A CPU fallback recipe does not establish NVIDIA client GLX support.
- Logind/user permissions, PipeWire runtime, controllers/touch and sleep hooks
  belong to the host system; building Gamescope does not configure them.

The `integration/launch-example.sh` is an opt-in foreground example only. It
requires explicit paths, invokes no systemctl/installer, and changes no global
provider. Configure these paths for your own installation; do not copy personal
deployment paths from historical research logs. It is not the original autostart
installer, whose fixed payload pins and privileged hardware helpers were tied to
one development installation.

Historical geometry: `-w 1280 -h 720 -W 720 -H 1280 --scaler stretch`, with the
accepted rotation/mirror-compensating presenter. Keep each dimension's purpose
clear when debugging. Native games used their Wayland option/windowed mode;
Proton/FEX/Box64 are outside this source package.

For any device test retain finite deadlines, a current journal/kernel watcher,
owned-process cleanup and a verified restoration plan. Minor packaging/wiring
errors can be corrected in the same sprint; a real consumer/GPU/kernel fault
or SSH loss stops hardware experiments. Do not mask BAD_STATE, fake frame
completion or introduce reconnect/retry loops.
