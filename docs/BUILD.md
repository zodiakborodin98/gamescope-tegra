# Building the source port

## Scope

Build the Gamescope executable and its WSI layer from source. This does not
build/install SteamOS, Steam, proprietary drivers, a Switch kernel or firmware.
Keep the existing working device baseline available separately. New outputs
require separate hardware validation before replacing a deployed executable.

## Tools and target dependencies

The reference host uses GCC 13.3, Meson 1.3.2, Ninja, native wayland-scanner and
glslang 15.1.0. A different compiler may produce different bytes. On Debian/
Ubuntu, host packages generally include `gcc-aarch64-linux-gnu`,
`g++-aarch64-linux-gnu`, `meson`, `ninja-build`, `pkg-config`, `git`, `python3`,
`glslang-tools`, `libwayland-bin`. Check glslang's version: distribution packages
may be older than the pinned shader generator.
For the Meson native-scanner recipe also install host `libexpat1-dev` and
`libxml2-dev`; these are build-host dependencies, not target libraries.

Generate a native **Wayland 1.26.0** scanner and pkg-config metadata in a fresh
local prefix. Older distribution scanners can generate incompatible wlroots code:

```sh
python3 scripts/build_host_tools.py --prefix "$PWD/_work/host-tools" \
    --build-dir "$PWD/_work/build-host-scanner"
```

This fetches the official Wayland 1.26.0 tag, verifies its exact locked commit,
and builds only the scanner. It never installs into `/usr`. `--source` accepts
an existing clean checkout at that pin. Alternatively, if only the host Expat
runtime is installed, use `--expat-includedir /path/to/expat-headers`; this
minimal native-compiler route uses `expat.h`/`expat_external.h` (architecture
independent) and links the **host** `libexpat.so.1`, never a target library.

The user-provided target sysroot needs headers and libraries for Wayland
client/server/protocols, Vulkan, EGL/GLVND, X11/XCB extensions, libdrm, libinput,
libseat, libudev, pixman, xkbcommon, lcms2, libcap, libdisplay-info, libdecor,
libeis, libavif and LuaJIT. Meson reports missing/version-incompatible packages.
`pkg-config` for target dependencies is isolated from host dependency paths.
OpenVR, SDL2, PipeWire capture, unit tests and benchmarks are disabled in the
reference build. Core wlroots/libliftoff/vkroots pins are mandatory.

## Fetching open-source dependencies

```sh
python3 scripts/fetch_deps.py
```

This checks out exact commits in `sources.lock.json`, preserves their license
files and refuses unexpected revisions or modified tracked files. Dependency
directories are ignored by Git. `libdisplay-info` is normally supplied by the
sysroot; `--include-optional` fetches its fallback plus unused OpenVR sources.
For an existing local Gamescope checkout with matching subproject mirrors:

```sh
python3 scripts/fetch_deps.py --offline --mirror-root /path/to/local/gamescope
```

## Shader source completeness

`gob_swizzle_sx1.comp` is the preferred source for `gob_swizzle_spv.h`. Run:

```sh
python3 scripts/verify_shader.py
```

The script compiles into a temporary file with Vulkan 1.1 target semantics and
compares every word to the embedded accepted header. A mismatch refuses rather
than silently changing orientation or accepting a different shader. The known
accepted SPIR-V is 6668 bytes, SHA256
`a3792dd6056456d75f9616d4a137b5d53dbdc40ce0be339f98f84ea4bd2dad04`.

## Clean cross build

```sh
python3 scripts/build.py --sysroot /path/to/aarch64-sysroot \
    --host-tools /path/to/native-tool-prefix \
    --build-dir /path/to/new-build --jobs 2
```

Cross builds require `--host-tools` containing `bin/wayland-scanner` and
`lib/pkgconfig/wayland-scanner.pc` for the build host. Native pkg-config lookup
is separated from the target's metadata. A new machine file selects `aarch64-linux-gnu-*`,
`-march=armv8-a -mtune=cortex-a57`, the exact sysroot and target-only pkg-config.
The known `<utility>` and cross-libstdc++ `math.h` forced includes are retained.
Build directories and machine-file collisions refuse; nothing is deleted.
`--wrap-mode=nodownload` prevents unpinned Meson downloads.

Output: `BUILD-DIR/src/gamescope`, layer outputs and `BUILD-RESULT.json`, which
records compile/link result, commands, versions, SHA and shader embedding.
No target binary is executed. Source-path/version/debug metadata changes mean
historical `b9f5847f…` byte identity is not required or claimed.

The builder also checks exact dependency revisions/clean tracked content and
reproduces all four EGLStream generated C/header files from their included XML.
See [the publication build result](BUILD-VALIDATION.md) for the measured recipe
and its limits. Run packaging checks with:

```sh
python3 -m unittest discover -s tests -v
python3 scripts/package_source.py
```

A native AArch64 Linux build may omit `--sysroot`; install equivalent development
dependencies first. That route is not qualified by the PC cross-build result.

## Experimental lifetime patch

The default is the historical baseline, including its documented defects.
`patches/experimental/eglstream-lifetime.patch` is separate and never applied by
the build script. Apply only in a separate clean copy and use a new build dir:

```sh
cd gamescope
git apply --check ../patches/experimental/eglstream-lifetime.patch
git apply ../patches/experimental/eglstream-lifetime.patch
```

That variant changes consumer ownership/retirement and remains experimental.
Its successful PC tests or source build do not imply a repaired device baseline.
