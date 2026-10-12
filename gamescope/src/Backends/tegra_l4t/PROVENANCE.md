# Provenance — vendored Tegra DC / GOB material

Publication note (2026-10-12): the table below is the historical development
record. The accepted orientation180/sx1 shader and source reconstruction are
described in the public `docs/PROVENANCE.md` and `sources.lock.json`. Their
preferred GLSL and NVIDIA protocol XML are included; the private probe checkout
is not required to build this publication package.

These files are reused (verbatim or minimally adapted) from the proven
`tools/tegra-present-probe` in this repository, which is the accepted M3e/M3h
evidence path. They are vendored here so the Gamescope Tegra L4T backend can
reuse the same algorithms without a new dependency.

| File | Origin | Notes |
|------|--------|-------|
| `tegra_dc_ext.h` | `tools/tegra-present-probe/third_party/tegra/tegra_dc_ext.h` | Vendored Tegra DC UAPI, same pin as `theofficialgman/vk_layer_tegra_dc_present` @ `ecfb76aadbdc93cd7008e77bec944724cd70c364` (GPL-2.0-or-later). Unmodified; includes `<uapi/linux/types.h>`. |
| `nvhost_ioctl.h` | `tools/tegra-present-probe/third_party/tegra/nvhost_ioctl.h` | Vendored nvhost UAPI (GPL-2.0-or-later). Unmodified. |
| `uapi/linux/types.h` | `tools/tegra-present-probe/third_party/tegra/uapi/linux/types.h` | Build-glue shim (no kernel implementation); redirects `<uapi/linux/types.h>` to installed `<linux/types.h>`. |
| `gob_swizzle_spv.h` | `tools/tegra-present-probe/src/gen/gob_swizzle_spv.h` | Generated SPIR-V for `shaders/gob_swizzle.comp`, adapted from the same pin and preserved verbatim. |
| `tegra_l4t_first_frame.cpp` | `tools/tegra-present-probe/src/{gob_math.c,dc.c,sync_release.c}` | GOB math, DC open/GET_WINDOW/FLIP4/fbdev head-enable/close-while-owning and kernel fence-status retirement adapted to C++/Gamescope. The GOB Vulkan submission is new: it runs on the existing Gamescope `VkDevice`/queue, not a second device. |
