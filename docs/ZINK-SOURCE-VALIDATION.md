# Zink source publication validation — 13 October 2026

- Mesa 24.2.8 archive SHA256 verified against the retained source input.
- Git-format patch applied to pristine upstream inputs; all 18 changed/new
  preferred-source hashes matched `graphics-stack/zink-t3-provenance.json`.
- Open early-ICD source compiled with the public helper as AArch64 ELF;
  output hash `4b9d1c9c2516a552f541d54f1a95a6633268324d2e754d204c77ee82752793b0`.
  This executable stays in ignored local work, is not distributed, installed
  or executed on the Switch, and has no inherited hardware PASS.
- Both persistent-session source templates passed Linux Bash syntax checks.
- Gamescope publication/helper suite: 26 tests, 2 Windows symlink skips, no failures.
- Image-builder suite: 61 tests, 25 Linux confinement/symlink skips on Windows,
  no failures with the existing local dependencies. Those skipped paths are not
  newly qualified by this source update.
- Four repository publication inventories contain source/documents only;
  license notices, pins, local links, secret/payload checks and source manifests
  verified. Ignored build outputs/dependencies are excluded from Git publication.

The complete new Mesa portable build was not recompiled end to end in this
publication task. Its recorded modified sources are from the working development
build, and the assembled public source inputs round-trip correctly. A fresh
whole-stack build and runtime installation still require their own qualification.

Two packaging issues were corrected before acceptance: Git-format creation
headers were missing `new file mode`, and inherited Git autocrlf could rewrite
source bytes on Windows. The final patch has explicit creation headers and the
fetch helper disables automatic line-ending conversion during patching.

The device UI/rendering evidence and optional-diagnostics-off change are
separate from these publication gates: see [Zink UI](ZINK-STEAM-UI.md).
