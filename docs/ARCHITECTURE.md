# The Tegra path

The normal Gamescope DRM atomic backend is not the display owner on this legacy
Switch setup. The Tegra backend uses the NVIDIA nvgpu Vulkan device and legacy
Tegra DC interfaces. Existing DRM/Wayland/Headless backends remain upstream code.

```text
Steam / native client / Wine client
           |
Xwayland EGLStream or NVIDIA Wayland producer
           |
Tegra EGLStream GL consumer → transform feedback / GPU copy
           |
immutable external slot: OPAQUE_FD + ready / returned synchronization
           |
Gamescope Vulkan import / compose / scaler
           |
GOB compute conversion → legacy Tegra DC FLIP4 → physical LCD
```

The historical Steam UI uses private Mesa/llvmpipe on the client side; GPU
composition and scanout are separate. Native Wayland clients and Windows
DXVK clients have different initialization requirements. The FROG layer can
translate supported X11 Vulkan surfaces to real Wayland surfaces; this does
not make the headless L4T ICD's X11 WSI functional.

The port keeps per-generation immutable slots and explicit external ownership.
The ready/returned handshake and lifetime retirement are safety contracts,
not optional performance switches. No extension/device identity spoof is used.

The accepted orientation is 180 degrees with source-X compensation in the GOB shader.
The physical panel is 720x1280. Historically the main logical game output is
1280x720 with stretch, while the patched Xwayland reports 426x240 for Steam UI.
These values belong to different stages and must not be treated as game FPS
or as a universal game resolution limit.

The experimental lifetime patch adds owned jobs, admission/retirement barriers,
owner pins, consumer-thread GL teardown and owner-thread host finalization.
It is separate because its hardware behavior is not a repaired production
baseline. See STATUS.md and patches/experimental/README.md.
