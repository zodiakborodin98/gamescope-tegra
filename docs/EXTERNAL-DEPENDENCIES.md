# External packages and official downloads

The public Git repository and source archive exclude downloaded binaries.
`downloads/`, `sysroots/`, `_work/` and dependency clones are ignored. The
download helper writes files only; it never extracts, runs package scripts,
installs drivers, changes mounts, writes boot/eMMC or touches a live session.

```sh
python3 scripts/fetch_external.py --list
python3 scripts/fetch_external.py valve-arm64-runtime --accept-license
```

That Valve artifact is an optional **runtime**, not the ARM64 Steam client or
the development sysroot. The pin was taken from the official historical
SHA256SUMS. The helper will refuse if the official URL disappears or bytes differ.

NVIDIA R32.7.4 T210 download URLs are verified official references, but this
package does not have an independently qualified SHA256 for the whole BSP.
It refuses an unpinned download. Supply a trusted verified hash explicitly:

```sh
python3 scripts/fetch_external.py nvidia-l4t-t210-32.7.4 \
    --sha256 TRUSTED_64_HEX_DIGIT_SHA256 --accept-license
```

Do not use the hash of the script's own first download as independent provenance.
No fake pin is supplied. Hashes and URL roles are in external-artifacts.lock.json.
A lawfully obtained local file can be checked with `--local-file PATH`; login,
license agreement and unavailable downloads are not bypassed.

The Jetson BSP is a reference package, **not a replacement Switchroot driver
installer**. Hardware compatibility requires the matching Switchroot stack.
Read the vendor terms before requesting a download. Redistribution permissions
depend on the actual component terms; this project takes the simpler source-only
route instead of bundling closed payloads.

The standalone Steam ARM64 client has no qualified official pin here. Supply
your own lawful installation with `STEAM_ROOT`/`STEAM_BIN`. No Switchdeck repack
is silently substituted for an official download. Steam is not required to build
Gamescope.

Open-source dependencies use official repository URLs and exact commits in
sources.lock.json; their license notices remain present in fetched checkouts.
