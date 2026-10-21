/* M7g T3 — private Steam-compatibility GLX/drisw/Zink presentation adapter.
 *
 * Interface between the env-gated hooks in src/glx/drisw_glx.c and the adapter
 * implementation (both in the private libGLX_mesa build).  Enabled only by
 * M7G_T3_ADAPTER=1.
 *
 * Difference from the frozen T2 proof adapter: arbitrary real pixels (no
 * pattern/FNV table, no frame cap, no artificial pacing/hold), multiple window
 * drawables per process, per-drawable transport bound to the real XID, resize
 * via bounded transport recreation, and honest fail-closed behavior for
 * unsupported cases.  The transport contract itself is unchanged.
 */
#ifndef M7G_T3_ADAPTER_H
#define M7G_T3_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called from driswSwapBuffers() for a drawable when the adapter is enabled.
 * `kopper != 0` means the private zink/kopper screen is active.  For window
 * drawables: read the current default framebuffer and deliver it through the
 * qualified OPAQUE_FD transport + override for this XID (creating/recreating
 * the per-drawable transport as needed).  For non-window drawables: no
 * transport (pixmap/pbuffer content has no presentation).
 * On adapter/transport failure: "T3_FAIL stage=... rc=..." and _exit(rc)
 * (fail-closed).
 */
int m7g_t3_adapter_swap(void *pdraw, int kopper);

/* Called from driswDestroyDrawable() before the DRI drawable is destroyed.
 * Drains and tears down the per-drawable transport with the accepted
 * fail-closed semantics (rc28 if not provably safe).
 */
void m7g_t3_adapter_drawable_destroyed(void *pdraw);

#ifdef __cplusplus
}
#endif

#endif /* M7G_T3_ADAPTER_H */
