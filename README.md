# Gamescope for Tegra X1 / Nintendo Switch

> **Development status: not suitable for everyday use.**
> This project is a research prototype, not a practical SteamOS gaming system at
> this stage. Serious performance, stability and game compatibility limitations
> remain. The sources are published for research and development.
>
> **Текущее состояние: проект непригоден для повседневного использования.**
> Это исследовательский прототип. Существенные ограничения производительности,
> стабильности и совместимости с играми делают практическое использование системы
> крайне затруднительным. Исходники опубликованы для разработки и исследований.

An **experimental source port**, based on Valve Gamescope, for the legacy
NVIDIA Tegra X1 / L4T 32.7.4 stack used by this Switch Linux development setup.
The historical baseline displayed Steam GamepadUI and native FEZ/Super Meat Boy
on a physical Switch. This is a source release candidate, **not a complete
SteamOS distribution or a stable Vulkan gaming release**.

## What this port implements

- A Tegra L4T backend with the legacy `/dev/tegra_dc_*` FLIP4 display interface.
- A GPU GOB conversion/presenter path, with the accepted 180-degree orientation
  and source-X compensation for the portrait 720x1280 panel.
- Xwayland EGLStream consumption and private OPAQUE_FD transport into Gamescope.
- NVIDIA Vulkan rendering/presentation in Gamescope. The Steam UI in the
  historical configuration uses private CPU Mesa/llvmpipe; this is distinct
  from the compositor's NVIDIA renderer.
- A separately labelled experimental EGLStream ownership/lifetime patch.

The source snapshot reconstructs the accepted `b9f5847f…` implementation from
its actual translation-unit inputs, including the retained presenter and GLSL.
It deliberately excludes later unaccepted focus experiments. The historical
ELF hash is a provenance reference, not a promise of bit-identical output or
hardware acceptance of a new build.

## Foundations and credits

This work builds on several projects:

- **Valve / Gamescope** — the compositor and its upstream infrastructure.
- **[theofficialgman and contributors](https://github.com/theofficialgman/vk_layer_tegra_dc_present)**
  — Tegra DC direct presentation, UAPI references and GOB conversion material
  adapted into this port. Original attribution and GPL notices are retained.
- **[SildurFX / Switchdeck](https://github.com/SildurFX/Switchdeck)** and
  **[Switchdeck-Extras](https://github.com/SildurFX/Switchdeck-Extras)** — the
  project's Steam ARM64 setup and Proton/FEX/Box64 compatibility recipes,
  including the DXVK-Sarek/VKD3D integration references used during development.
- **[Switchroot](https://wiki.switchroot.org/wiki/linux) and its contributors**
  — the Switch Linux kernel/BSP, boot and hardware-support platform used for
  development and device testing. Kernel/build-source links and the recorded
  baseline identity are in [PROVENANCE](docs/PROVENANCE.md).

The source port and the broader Steam integration have different dependency
scopes. Switchdeck's installers, Steam payloads and Proton tools are not bundled
in this Gamescope source package. See [NOTICE](NOTICE.md) and
[source provenance](docs/PROVENANCE.md) for exact origins and license scope.

## Quick start: source build

Use a Linux build host (WSL2 is suitable), GCC 13 cross tools, Meson, Ninja,
pkg-config, native wayland-scanner and glslang 15.1.0. Supply a compatible
AArch64 development sysroot from your own installation.

```sh
python3 scripts/fetch_deps.py
python3 scripts/build_host_tools.py
python3 scripts/verify_shader.py
python3 scripts/build.py --sysroot /path/to/aarch64-sysroot \
    --host-tools "$PWD/_work/host-tools" \
    --build-dir /path/to/fresh-build --jobs 2
```

The builder performs a clean source compile/link; it never installs, starts
services, flashes a device or reuses historical object files. The default
`debug`/O0 profile follows the tested historical recipe. See
[build instructions](docs/BUILD.md), [architecture](docs/ARCHITECTURE.md),
[integration prerequisites](docs/INTEGRATION.md), and [known issues](docs/STATUS.md).
Mesa llvmpipe/Zink, Xwayland, Wayland and egl-wayland source pins, actual patches
and private build recipes are in [graphics-stack](graphics-stack/README.md).
The measured clean build is recorded in [build validation](docs/BUILD-VALIDATION.md).

## External components

Steam, NVIDIA driver binaries/firmware, rootfs/sysroots, games, Proton, FEX and
Box64 payloads are **not included**. An opt-in downloader supports official
vendor URLs, immutable checksums and local-file verification without installing
anything. Items without a qualified hash refuse until a trusted pin is supplied.
The Steam ARM64 client is user-supplied; an official Steam runtime archive is
not a replacement for that client.

```sh
python3 scripts/fetch_external.py --list
```

See [external downloads](docs/EXTERNAL-DEPENDENCIES.md).
For local source archive verification, see [publication contents](docs/PUBLICATION.md).

## Status and licensing

Working Steam UI/native-game demonstrations do not imply stable FEX/Box64 gaming.
Startup resize and teardown EGLStream faults remain open, including a reproduced
`EGL_BAD_STATE_KHR` during the initial 1280x800 to 426x240 UI transition.
Gamescope screenshot is unsafe in the historical build; do not use it as a
validation command. Compositor presents/s are not game FPS.

Upstream Gamescope retains its BSD notices. Tegra code includes GPL material;
the combined Tegra program is distributed under GPLv2 with all individual
component notices retained. Our original packaging scripts, tests, examples
and documents use **GPL-2.0-or-later**, matching gman's source notice; third-party
BSD/MIT and other notices remain applicable. Read [LICENSE](LICENSE), [NOTICE](NOTICE.md) and
[licensing details](docs/LICENSING.md).

This project is not an official Valve, NVIDIA, Nintendo or Switchroot release.

Related repositories: [LCD sources/probes](https://github.com/zodiakborodin98/tegra-lcd-backend),
[source image builders](https://github.com/zodiakborodin98/steamos-tegra), and
[implementation/blocker history](https://github.com/zodiakborodin98/steamos-tegra-research).
