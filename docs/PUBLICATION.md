# Local publication package

This folder is prepared for the source-only GitHub repository
`zodiakborodin98/gamescope-tegra`. Its Git repository is isolated; the private
development repository's history is not imported. Published revisions identify
actual source snapshots, not the original private experiment history.

Included: accepted-source reconstruction, pinned source dependency fetcher,
fresh cross-build recipe, shader/protocol preferred sources and reproduction
checks, optional official-download helper, license/attribution inventory,
integration example, known issues and a separate experimental lifetime patch.
Unused upstream Steam Deck remote-deployment helpers, editor settings and CI
workflows are omitted. The original development workspace is preserved.

Not included: Steam/client seed, NVIDIA binaries or firmware, sysroot/rootfs,
game data, Proton/FEX/Box64 packages, account configuration, credentials,
test evidence/dumps, compiled output or cloned dependency trees. The upstream
EDID template is a source asset and remains included. Source dependency clones
are obtained separately at locked commits, with their licenses intact.

`SOURCE-MANIFEST.sha256` hashes all publication files except itself. Validate:

```sh
python3 scripts/package_source.py
python3 -m unittest discover -s tests -v
```

After intentionally changing public files, review the diff, then regenerate:

```sh
python3 scripts/package_source.py --write-manifest
python3 scripts/package_source.py --archive ../gamescope-tegra-source.zip
```

The archive is created outside the source folder, cannot overwrite an existing
archive, excludes local output/dependency directories and verifies archived
bytes. It contains no `.git` metadata. Do not upload the whole private workspace
or use an unfiltered recursive archive command.

The optional external downloader is independent of compilation and installation:
it checks official HTTPS hosts/redirects, an independently trusted hash and
component terms. Downloading a package does not itself establish redistribution
permission. NVIDIA artifacts without a supplied trusted hash refuse; no pin is
invented. The Steam runtime entry does not supply the ARM64 Steam client.

The source build succeeded; new hardware acceptance is still pending. Describe
the project as an experimental port with known EGLStream faults, not a stable
release or complete SteamOS distribution. See BUILD-VALIDATION, STATUS and
LICENSING before a future public release.
