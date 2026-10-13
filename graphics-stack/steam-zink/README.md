# Native Steam/CEF early NVIDIA initialization

Original project source: GPL-2.0-or-later. No Steam/NVIDIA binaries are included.
`early-icd-forkexec.c` is the measured fresh-exec bootstrap source with its project
license notice added. It must run before application threads with an exact
owned executable/token/direct-v1 context; an inherited CEF zygote context is
refused. DISPLAY is restored before ordinary UI work; the ICD context remains
real and initialized. Do not preload it into arbitrary processes.

`build-bootstrap.py` requires a compatible target sysroot and creates only an
AArch64 .so in an explicitly fresh local path; it never installs or runs it.
`prepare-local-cef.py` accepts only the historical source SHA and AArch64 ELF,
changes the application request/shader dialect to actual 4.2/420, and refuses
in-place replacement/unknown revisions. Output is user-owned, ignored local
work and still subject to vendor terms, not a redistributable Steam payload.

[Source recipe and remaining CPU copies](../../docs/ZINK-STEAM-UI.md).
The sibling steamos-tegra repository supplies adaptable session templates.
