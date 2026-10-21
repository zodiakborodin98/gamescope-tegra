/* M7g T3 — transport interface (generalized from the accepted T1/T2 OPAQUE_FD
 * transport; see tools/m7g-zink-t3/mesa-overlay/.../m7g_t3_transport_vk.c).
 *
 * Contract kept: immutable slots, exported OPAQUE_FD memory, ready/returned
 * binary semaphores, external ownership barriers, bounded reuse, fail-closed
 * teardown.  Change for T3: the per-instance drawable SIZE is a parameter
 * (arbitrary real windows), not a fixed 1280x720 proof size.
 */
#pragma once

#include <stdint.h>

typedef struct m7g_t3_xport m7g_t3_xport;

typedef struct {
    unsigned slots;       /* registered immutable slots */
    unsigned submitted;   /* generations submitted (ready signaled) */
    unsigned commits;     /* wl_surface attach+damage+commit */
    unsigned releases;    /* wl_buffer.release events observed (client-visible) */
    unsigned reuse_waits; /* bounded slot-reuse waits performed */
    unsigned refresh_cycle;        /* gamescope_swapchain.refresh_cycle events */
    unsigned past_present_timing;  /* gamescope_swapchain.past_present_timing events */
    unsigned retired;              /* gamescope_swapchain.retired events */
} m7g_t3_xport_counters;

/* failure stage codes: identical numbers to the accepted T1/T2 transport */
enum {
    M7G_T3_XS_INIT = 20,   /* wayland connect / bridge Vulkan init */
    M7G_T3_XS_BIND = 21,   /* missing compositor/factory/manager globals */
    M7G_T3_XS_STAGE = 22,  /* surface/create_swapchain/override protocol error */
    M7G_T3_XS_SLOTS = 23,  /* slot registration rejected / export unsupported */
    M7G_T3_XS_UPLOAD = 24, /* staging upload / gpu copy / submit failed */
    M7G_T3_XS_REUSE = 25,  /* bounded slot-reuse wait failed (no in-flight reuse) */
    M7G_T3_XS_COMMIT = 26, /* attach/damage/commit/flush failed */
    M7G_T3_XS_TEARDOWN = 28, /* teardown not provably safe: resources left to exit */
    M7G_T3_XS_RETIRED = 29,  /* swapchain retired while generations still to send */
    M7G_T3_XS_DRAIN = 30,    /* flush/dispatch/drain/service error */
};

int  m7g_t3_xport_init(m7g_t3_xport **out, uint32_t server_id, uint32_t xid,
                       uint32_t w, uint32_t h);
int  m7g_t3_xport_commit(m7g_t3_xport *x, unsigned slot, unsigned gen, const unsigned char *pixels);
int  m7g_t3_xport_service(m7g_t3_xport *x, int timeout_ms);  /* bounded event pump */
int  m7g_t3_xport_drain(m7g_t3_xport *x);                    /* flush+roundtrip, exact FAIL */
int  m7g_t3_xport_retired(const m7g_t3_xport *x);
void m7g_t3_xport_get_counters(const m7g_t3_xport *x, m7g_t3_xport_counters *c);
int  m7g_t3_xport_destroy(m7g_t3_xport *x);                  /* 0 clean; nonzero = unsafe */
const char *m7g_t3_xport_backend(void);
uint32_t m7g_t3_xport_w(const m7g_t3_xport *x);
uint32_t m7g_t3_xport_h(const m7g_t3_xport *x);

/* Pure poll-result verdict for the service pump (same as T1/T2).
 * POLLIN=0x001, POLLERR=0x008, POLLHUP=0x010, POLLNVAL=0x020, EINTR=4. */
static inline int m7g_t3_poll_verdict(int pr, short revents, int err)
{
    if (pr < 0)
        return err == 4 ? 0 : M7G_T3_XS_DRAIN;
    if (pr > 0 && (revents & (short)(0x008 | 0x010 | 0x020)))
        return M7G_T3_XS_DRAIN;
    return 0;
}
