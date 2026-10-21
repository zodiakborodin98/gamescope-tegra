# CEF-child GPU transfer — source closure

These preferred sources are from the accepted M10K full-unit15 provider line,
including GLX reassertion and Gallium external-wait tracking. They complement
the current Gamescope consumer; the old mesa-zink-t3 profile is historical.

The sources in overlay retain their existing Mesa/SGI/per-file notices. New
project files retain their own SPDX terms. SOURCE-INPUTS.json identifies the
exported source bytes; no NVIDIA, Steam, CEF, driver or runtime binary is shipped.

Build on Linux with the existing cross toolchain, host Mako/PyYAML/PLY,
Bison, Flex, M4 and
the target development sysroot:

```sh
python3 scripts/fetch_graphics.py mesa-zink-gpu-child --output /path/to/new-mesa-source
python3 scripts/build_graphics.py mesa-zink-gpu-child \
  --source /path/to/new-mesa-source --sysroot /path/to/aarch64-sysroot \
  --host-tools /path/to/native-wayland-tools --build-dir /path/to/new-build --jobs 4
```

Run these commands from the repository root. The source profile applies the
original T3 patch followed by mesa-24.2.8-zink-gpu-child.patch, then verifies
all25 preferred-source hashes. It builds the GPU helper into the GLX library
and carries the matching Gallium/state-tracker changes. Source overlays are
included for inspection; use the locked fetch path rather than ad-hoc copying.
The existing external
sysroot, NVIDIA Vulkan driver and compatible Wayland/CEF integration are still
required. A successful compile is not device acceptance. Do not install only
one member of the provider/consumer bundle on an existing baseline.
