# Source validation — 21 October 2026

* Linux source/helper suite:27 tests, zero failures/skips. Includes internal
  directory symlink hashing and refusal of links escaping the source tree.
* Official Mesa24.2.8 archive and the original T3 preferred-source hashes
  verified. Later GPU-child patch round-tripped against that base; all25
  preferred-source hashes are recorded in the lock and SOURCE-INPUTS.json.
* Updated Gamescope compiled/linked for AArch64/Cortex-A57 in a new workdir,
  using the existing external target sysroot and pinned source dependencies.
* New Mesa GPU-child profile compiled/linked in a new workdir: GLX producer
  and Zink/Gallium/state-tracker code. No retained private Mesa objects were
  copied into the build. Host Python generators, Bison/Flex/M4 were supplied
  separately; the reference sysroot is not bundled.

Corrected pre-publication issues are retained in the private build record:
the matching game-orientation header was initially absent; official Mesa's
internal CI-directory link was rejected by the old fetch helper; the initial
GLX export selected a scratch drisw source with an unused experimental hook.
The final export uses the drisw source inherited by the accepted archive.
Missing host Mako/Bison/Flex/M4 were environment prerequisites, not GPU faults.

These are compile/link and source-publication results. Neither output was
uploaded, installed or executed on Switch. Existing device acceptance applies
to the historical full bundle, not automatically to these new public builds.
No fresh hardware, physical-FPS, arbitrary-game or Beta1-boot PASS is claimed.
