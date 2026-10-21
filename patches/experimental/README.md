# Experimental EGLStream lifetime variant

`eglstream-lifetime.patch` is the integrated ownership/retirement instrumentation
variant whose historical candidate ELF was `4b93ac3d…`. It is not enabled by default.
It adds owned jobs, a retirement admission barrier, owner pins across submit/
fetch/release, consumer-thread GL teardown, owner-thread host finalization,
bounded acquire configuration and diagnostic generation/lifetime fields.

Its PC ownership tests were clean, but hardware BAD_STATE/teardown issues remain.
A successful run on the variant is not proof of the cause of an earlier baseline
failure. The default baseline also has documented latent lifetime defects.

Use a separate source copy and build directory. Neither applying this patch nor
building an ELF is permission to replace a working device session. Preserve all
GPL/upstream notices and the source/build inputs when redistributing derivatives.
# Historical patch boundary — 21 October 2026

The patch below targets the old pre-M10O source snapshot. The main tree now
contains the later ownership/transport/discard lineage. Do not apply this
historical patch on top of the current tree. See
[current source status](../../docs/UPDATE-2026-10-21.md).

