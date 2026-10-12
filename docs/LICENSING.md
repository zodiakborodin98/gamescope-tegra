# Licensing and redistribution

## Source package scope

This repository is a source package. It contains the Gamescope Tegra port,
build tooling, integration examples, and a separately labelled experimental
EGLStream lifetime patch. It does not contain NVIDIA proprietary drivers,
firmware, a Steam client, games, SteamOS images, extracted sysroots, Proton,
FEX or Box64 runtime payloads.

The root [LICENSE](../LICENSE) is a component-scope declaration, not a promise
that all files share one license. The combined Tegra Gamescope program is
distributed under **GPL version 2**; the full text is in [COPYING](../COPYING).
Individual Tegra files retain their existing GPL-2.0-or-later notices. The
initial combined distribution uses version 2 while preserving broader
permissions wherever the individual component actually grants them.

Original upstream Gamescope remains under its BSD-2-Clause license and the
additional upstream notices. The complete upstream text has been preserved,
including the OpenColorIO and ReShade BSD-3-Clause terms. Combining this code
with GPL-covered Tegra code does not remove those original licenses or notices.
Do not describe this port or its combined executable as entirely BSD-licensed.

Original project packaging scripts, tests, integration examples and documents
outside `gamescope/` are **GPL-2.0-or-later** where they are not derived from another
component. The experimental patch inherits the GPL scope of the Tegra code it
changes. File-specific licenses and retained third-party notices take priority
over a general directory description.

This is the user's selected license for our original work, matching gman's
per-file grant. The combined Tegra program is currently distributed under
version 2 because broader rights are not assumed for GPL-2.0-only imported
material. Third-party BSD/MIT choices and copyright notices are preserved;
`LICENSES/MIT.txt` is a third-party license reference, not the packaging license.

## Third-party components

See [NOTICE.md](../NOTICE.md), [PROVENANCE.md](PROVENANCE.md), and
`sources.lock.json` for attribution and exact pins. Preserve each original
license/copyright header and the license file of any fetched component that
you redistribute.

vkroots runtime headers and the examined Vulkan headers explicitly offer
**Apache-2.0 OR MIT**. This distribution selects **MIT** for those files.
This is an option already granted by those components, not a change to their
license. Apache-only files do not gain a MIT option. Inspect the exact Vulkan
headers supplied by a different SDK/sysroot before making the same choice.
vkroots Python generators are separately LGPL-2.1-or-later; their original
license files remain applicable when those scripts are fetched/redistributed.

The NVIDIA Tegra DC and nvhost headers retain their complete original GPL
version 2 or later notices. Linux documents the syscall-interface exception
for UAPI, but this package does not add an exception that is absent from an
imported file's own applicable terms. The copied/adapted GPL GOB shader and
explicit GPL Tegra implementation are sufficient reasons for the port's GPL
distribution, independently of kernel-header interpretation.
[Linux kernel licensing rules](https://www.kernel.org/doc/html/latest/process/license-rules.html).

## Source and generated output

The preferred GLSL source for the embedded Tegra shader is included as
`gamescope/src/Backends/tegra_l4t/gob_swizzle_sx1.comp`. Its generated SPIR-V
header retains the source's GPL scope. Keep source, generation/build scripts
and related protocol/interface definitions together when redistributing.
An object file or generated array by itself is not the corresponding source.

The initial package publishes source, not the historical accepted executable.
A future executable release requires the corresponding complete source/build
inputs and the applicable linked-component notices for that actual build.
An old accepted ELF hash is a provenance reference, not proof that a newly
built executable is tested or automatically satisfies every distribution term.

## External dependencies

Users supply an appropriate target sysroot and compatible NVIDIA runtime.
An optional download helper may fetch vendor packages into an ignored local
cache after explicit opt-in. It must not install, flash, modify an accepted
device stack, or silently accept terms on a user's behalf.

Official NVIDIA R32.7.4 resources:

* [Release page](https://developer.nvidia.com/embedded/linux-tegra-r3274).
* [T210 / Nano / TX1 BSP](https://developer.nvidia.com/downloads/embedded/l4t/r32_release_v7.4/t210/jetson-210_linux_r32.7.4_aarch64.tbz2).
* [T210 sources](https://developer.nvidia.com/downloads/embedded/l4t/r32_release_v7.4/sources/t210/public_sources.tbz2).
* [Vendor software agreement](https://developer.download.nvidia.com/embedded/L4T/r32_Release_v7.4/T186/Tegra_Software_License_Agreement-Tegra-Linux.txt).

That agreement governs the vendor software independently; it includes a
conditional unmodified-binary redistribution provision for Linux/FreeBSD.
This source package does not rely on that provision to bundle vendor binaries.
Official Jetson BSP downloads are not a tested Switchroot installation or a
replacement for the user's accepted drivers. Vendor terms and compatibility
must be assessed for the actual package/use.

Steam is **not a Gamescope build dependency**. A user may point an integration
example at an already authorized local Steam installation. The official
[Steam Linux launcher page](https://repo.steampowered.com/steam/) documents
amd64/i386 packages; this package does not claim a qualified official ARM64
client download pin or redistribute its private development seed.
[Steam's terms](https://store.steampowered.com/subscriber_agreement/) remain
separate from the licenses of this source repository.

## Release checklist

* Preserve original file headers, complete upstream license appendices and
  this package's GPL text/component scope.
* Preserve source pins/hashes and preferred shader/protocol sources and build
  recipes; include complete component licenses for any flattened dependencies.
* Record modified files and dates. Preserve upstream authorship; do not replace
  it with the project's name or use it as an endorsement.
* Exclude proprietary runtime payloads, private credentials/account data,
  raw dumps/evidence, game assets and the private development history.
* Keep experimental changes and their untested hardware limits explicit.

These documents inventory the package and its terms; they are not a legal
warranty. Adding another component or distributing a new executable changes
the concrete redistribution checklist.

