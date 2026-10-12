# Evidence and limits (2026-10-12)

This is an alpha source port, not a general Switch Linux graphics distribution.

| Area | Evidence / limitation |
|---|---|
| Steam baseline | Automatic Gamescope+Steam startup and UI were confirmed on the physical Switch. Client CPU rendering remains slow. |
| Native games | FEZ and Super Meat Boy worked with the accepted Wayland launch options and in-game windowed mode. Resolution selection was visually accepted. |
| NVIDIA Vulkan | Compute, readback and Wayland/EGLStream presentation were demonstrated on the hardware. |
| FEX | Windows x64 execution, SteamAPI, D3D11 offscreen GPU rendering and synthetic visible Vulkan patterns demonstrated. Ordinary game readiness is not established. |
| Box64 | HL2DM reached a visible menu; orientation/performance and EGLStream lifecycle issues remain. Not a stable gaming claim. |
| Baseline EGLStream startup | Two recent session starts showed an initial 1280x800 frame, Release 0x321c, then a 426x240 stream and continuing UI. Producer state/cause not yet proved. |
| Long/teardown paths | Consumer timeouts and BAD_STATE failures observed. Existing safety STOPs remain; no automatic retries of a failing GPU path. |
| Screenshot | Historical `gamescopectl screenshot` caused a compositor abort. Do not use it. |
| New public build | Compile/link qualification is recorded separately; no hardware PASS is inherited merely from the historical ELF hash. |

Compositor presents/s, repeated textures and generic flips are not NEW game FPS.
Visual/interface acceptance requires a user's observation of the physical screen.
A clean target exit, helper/shim lifecycle, consumer retirement and baseline
recovery are distinct outcomes.

The public source snapshot retains known baseline lifetime defects to preserve
its source identity; do not treat it as an audited stable replacement. A labelled
experimental lifetime patch is provided separately. Changes to initialization,
ownership, global providers or kernel/display stack need independent qualification.
