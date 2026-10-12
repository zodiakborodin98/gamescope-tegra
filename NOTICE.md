# Attribution and third-party notices

This is an unofficial source port of Gamescope for Tegra L4T. It is not an
official Valve, NVIDIA, Nintendo or Switchroot product. Naming an upstream
project here identifies provenance, not endorsement.

This file supplements the original notices in source files and component
license files; it does not replace or narrow them. See [LICENSE](LICENSE),
[COPYING](COPYING), [licensing](docs/LICENSING.md) and
[source provenance](docs/PROVENANCE.md).

## Gamescope upstream

The entire upstream license text, including its additional notices, remains in
[gamescope/LICENSE](gamescope/LICENSE) and
[LICENSES/Gamescope.upstream.txt](LICENSES/Gamescope.upstream.txt).

* Copyright (c) 2013–2022, Valve Corporation.
* Copyright (c) 2022, NVIDIA CORPORATION.
* OpenColorIO tetrahedral 3D LUT material: Copyright Contributors to the
  OpenColorIO Project, BSD-3-Clause notice included by upstream.
* ReShade effects compiler: Copyright 2014 Patrick Mours. All rights reserved.
  BSD-3-Clause notice included by upstream.

These copyright notices apply to the corresponding original components.
Tegra modifications are separately GPL-licensed as recorded in their files.

## Tegra direct presentation and GOB conversion reference

The direct-present/GOB work was copied or adapted from
[theofficialgman/vk_layer_tegra_dc_present](https://github.com/theofficialgman/vk_layer_tegra_dc_present)
at `ecfb76aadbdc93cd7008e77bec944724cd70c364`. Its main source file carries the
following original notices:

```text
SPDX-License-Identifier: GPL-2.0-or-later

Copyright (C) 2026 theofficialgman
Copyright (C) 2026 gavin_darkglider
Copyright (C) 2026 Anthropic (Claude AI assistant contributions)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.
```

This preserves the reference project's attribution, without claiming that
each listed contributor wrote every shader line. The preferred GLSL source
used for this port is
`gamescope/src/Backends/tegra_l4t/gob_swizzle_sx1.comp`. The generated
`gob_swizzle_spv.h` inherits that source's license and is not independently
relicensed as an anonymous binary array. Rotation and accepted sx1 compensation
are project adaptations, recorded in the source and source lock.

## Switchdeck / Steam and compatibility-tool integration

The broader SteamOS-on-Switch project used
[SildurFX/Switchdeck](https://github.com/SildurFX/Switchdeck), reference commit
`be0578fbf00c5a14a988a830f914f2c34cdaa1a1`, as a foundation for Steam ARM64
setup and compatibility-tool integration. Its installer/update/launch recipes
provided the Proton 11.0-1 Armv8.0 FEX release source and references for Box64,
DXVK-Sarek and VKD3D-Proton selection/overlay behavior. The FEX package source was
[SildurFX/Switchdeck-Extras](https://github.com/SildurFX/Switchdeck-Extras/releases/tag/Proton-11.0-1-Armv8.0).
These contributions are part of the project's development lineage even though
the standalone Gamescope compositor does not require those tools to compile.

Switchdeck's `NOTICE.txt` identifies its bash scripts as **GPL-3.0** and its
Steam payloads as Valve proprietary material under separate terms. This source
package does not bundle those scripts/payloads or relicense them as MIT/GPLv2.
If Switchdeck script code is added in a future integration release, retain its
actual GPL-3.0 license, copyright notices and corresponding source obligations.
Each Proton/FEX/Box64/DXVK/VKD3D component also retains its own applicable terms.

Switchdeck's README credits **Ivy** for the original steam-arm64 download
script; that upstream acknowledgement remains part of this reference chain.

## Switchroot / Linux kernel and hardware platform

The project used the **Switchroot Linux/L4T kernel and BSP platform** for
actual Switch development, boot, device drivers and hardware access.
[Switchroot's Linux documentation](https://wiki.switchroot.org/wiki/linux)
and [source/credits listing](https://wiki.switchroot.org/wiki/linux/l4t-ubuntu-jammy-installation-guide)
identify the community work and kernel/build tools. Credit includes Switchroot
contributors such as CTCaer, theofficialgman, Gavin_Darkglider and the wider
kernel, boot and hardware-support community; the source listing retains its
full upstream contributor acknowledgements.

Historical baseline records identify Linux `4.9.140-l4t`, build `#149`, and
Switchroot BSP `5.1.2-34+24.04`. The exact running-kernel source commit was
not established. The pinned `theofficialgman/switch-l4t-kernel-nvidia` checkout
used for UAPI/reference analysis is a source reference, not an asserted match
to that running build. See PROVENANCE for URLs and the actual source pin.

No Linux kernel image, module or firmware is bundled here. Linux kernel and
imported UAPI licenses are retained separately; this Gamescope package does
not claim authorship of the Switchroot kernel or relicense it.

## NVIDIA kernel UAPI headers

`gamescope/src/Backends/tegra_l4t/tegra_dc_ext.h` retains:

```text
Copyright (C) 2011-2019, NVIDIA Corporation. All rights reserved.
Author: Robert Morell <rmorell@nvidia.com>
Some code based on fbdev extensions written by:
    Erik Gilling <konkers@android.com>
```

`gamescope/src/Backends/tegra_l4t/nvhost_ioctl.h` retains:

```text
Copyright (c) 2009-2019, NVIDIA Corporation. All rights reserved.
```

Both headers retain their complete original GPL version 2 or later boilerplate
in the files themselves. The userspace `uapi/linux/types.h` adapter is original
project build glue, not a copied kernel implementation. The port does not
redistribute a kernel image or NVIDIA proprietary driver with these headers.

## Legacy Android synchronization UAPI

The legacy fence-info field layouts and ioctl definitions adapted into
`tegra_l4t_first_frame.cpp` and `tegra_l4t_presenter.cpp` originate from
`drivers/staging/android/uapi/sync.h` in the L4T Linux kernel reference:

```text
Copyright (C) 2012 Google, Inc.
```

The original header is GPL-2.0 kernel UAPI. Its provenance is
OE4T/linux-tegra-4.9 at
`78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e`; source SHA256
`d90f5f6a2e77371d9de7ba91595883ceb84e302b98453e37db09f7124679d9eb`.
The adaptations rename the types and omit trailing flexible-array declarations
while preserving the ABI layout and ioctl value. This notice restores the
origin alongside the frozen accepted source files without changing their bytes.
No new syscall exception is invented for legacy file notices.

## Wayland protocols

* The generated EGLStream protocol files retain their NVIDIA MIT notices:
  Copyright (c) 2014–2019 NVIDIA CORPORATION (stream protocol); controller
  protocol source XML carries Copyright (c) 2017–2018 NVIDIA CORPORATION.
  Their preferred XML inputs are included under
  `gamescope/src/Backends/tegra_l4t/eglstream/source/`, copied from official
  NVIDIA/egl-wayland release 1.1.23, commit
  `2a953fc86ca5707466ff28c117b0b72de87d9316`, with the complete original
  permission/copyright blocks intact. Regeneration with wayland-scanner 1.26.0
  reproduced all four frozen server C/header files byte-for-byte.
* `gamescope/protocol/gamescope-swapchain.xml` retains Copyright © 2023 Joshua
  Ashton for Valve Software and its permission/disclaimer text.
* `gamescope/protocol/tegra-l4t-buffer.xml` retains Copyright © 2026
  SteamOS-Switch native-gamescope effort and its existing permission text.

Other bundled upstream XML files retain their own copyright and permission
blocks. Generated files inherit their protocol source notices.

Publication reconstruction and packaging were prepared on 2026-10-12.
The changed/added file inventory is in `docs/MODIFIED-FILES.txt`; it identifies
the port's adaptations while retaining original file notices and authorship.

## Other included upstream material

* sol2 (`gamescope/thirdparty/sol/sol.hpp`): Copyright (c) 2013–2020 Rapptz,
  ThePhD and contributors; MIT. Generated sol 3.3.1 / revision 2b0d2fe8.
* GLM (fetched dependency): Copyright (c) 2005 G-Truc Creation; the project
  offers MIT or The Happy Bunny License. This distribution selects MIT.
* AMD FidelityFX headers (`ffx_a.h`, `ffx_fsr1.h`): Copyright (c) 2021 Advanced
  Micro Devices, Inc.; MIT. `ffx_a.h` also retains the MIT attribution for
  Copyright (c) 2014 Michal Drobot's floating-point concepts.
* NVIDIA Image Scaling SDK 1.0.3: Copyright (c) 2022 NVIDIA CORPORATION &
  AFFILIATES; MIT. Notices remain in the SDK headers.
* Qualcomm SGSR1 adaptation: Copyright (c) 2023, 2025 Qualcomm Innovation
  Center, Inc.; BSD-3-Clause. The source retains revision
  `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e` and modification notes.

Fetched upstream subprojects retain their individual license files. Runtime
vkroots headers are Apache-2.0 OR MIT (MIT selected here); its Python generator
scripts are LGPL-2.1-or-later. wlroots, libliftoff and stb carry their own MIT
notices; stb also offers its Unlicense alternative. See the source lock for
fetch pins and the original component licenses after fetching. License texts
provided for reference do not imply that every optional component is bundled
or enabled.

## Project packaging work

Copyright (c) 2026 SteamOS-Switch contributors.

Original project packaging scripts, tests, integration examples and documents
are available under **GPL-2.0-or-later** (GPL version 2 or, at your option, any
later version; see [COPYING](COPYING)), except material derived from
another component, whose license is retained. The Tegra implementation and its
experimental lifetime patch retain GPL scope. No claim of authorship or
relicensing is made over upstream code.

