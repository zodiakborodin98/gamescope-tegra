/* M7g T3 — stub transport backend (test-only; never linked into the
 * shipped ELF; generalized from the T2 stub: symbol/log prefix plus
 * per-instance size parameters; same orchestration surface).
 *
 * Exercises the client orchestration, counters and failure injection without
 * Wayland/Vulkan.  Compiled only with -DM7G_TEST_HOOKS.
 */
#include "m7g_t3_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct m7g_t3_xport {
    unsigned frames;
    unsigned slots;
    uint32_t w;
    uint32_t h;
    uint32_t bytes;
    m7g_t3_xport_counters c;
    unsigned magic;
    int retired;
    long retired_after_gen; /* -1 = never */
    unsigned service_calls;
    unsigned pending_events;
    unsigned require_service;
};

static int t3_retired_fired; /* one-shot injected retirement across instances */

static int fail_at(const char *stage)
{
#ifdef M7G_TEST_HOOKS
    const char *e = getenv("M7G_TEST_FAIL_AT");
    if (e && *e && strcmp(e, stage) == 0) {
        printf("T3_TEST fail_at=%s (injected)\n", stage);
        return 1;
    }
#else
    (void)stage;
#endif
    return 0;
}

const char *m7g_t3_xport_backend(void)
{
    return "stub";
}
uint32_t m7g_t3_xport_w(const m7g_t3_xport *x) { return x ? x->w : 0; }
uint32_t m7g_t3_xport_h(const m7g_t3_xport *x) { return x ? x->h : 0; }

int m7g_t3_xport_init(m7g_t3_xport **out, uint32_t server_id, uint32_t xid,
                      uint32_t w, uint32_t h)
{
    struct m7g_t3_xport *x;
    printf("T3_XPORT backend=stub server_id=%u xid=0x%x w=%u h=%u\n", server_id, xid, w, h);
    if (fail_at("init"))
        return M7G_T3_XS_INIT;
    x = calloc(1, sizeof(*x));
    if (!x)
        return M7G_T3_XS_INIT;
    x->frames = 0; /* frames come from the T2 adapter scenario */
    x->slots = 3;
    x->w = w;
    x->h = h;
    x->bytes = w * h * 4u;
    x->magic = 0x7b1u;
    x->retired_after_gen = -1;
    x->require_service = 1; /* T3: the stream must service queued events */
    x->c.slots = x->slots;
#ifdef M7G_TEST_HOOKS
    {
        const char *r = getenv("M7G_TEST_RETIRED_AFTER");
        if (r && *r)
            x->retired_after_gen = strtol(r, NULL, 10);
        r = getenv("M7G_T3_STUB_REQUIRE_SERVICE");
        if (r && *r)
            x->require_service = (strtol(r, NULL, 10) != 0);
        if (t3_retired_fired)
            x->retired_after_gen = -1; /* the injected retirement is one-shot */
    }
#endif
    printf("T3_XPORT stub slot count=3 (fake register)\n");
    *out = x;
    return 0;
}

int m7g_t3_xport_commit(m7g_t3_xport *x, unsigned slot, unsigned gen, const unsigned char *pixels)
{
    if (!x || x->magic != 0x7b1u)
        return M7G_T3_XS_UPLOAD;
    if (slot >= x->slots)
        return M7G_T3_XS_UPLOAD;
    if (x->retired) {
        printf("T3_TEST swapchain-retired (injected state)\n");
        return M7G_T3_XS_RETIRED;
    }
    if (gen > 0 && (gen % x->slots) == 0)
        x->c.reuse_waits++;
    /* T3: queued events (release/retired/protocol) must be dispatched by
     * service() between generations; otherwise this is not event-progressing. */
    if (x->require_service && x->pending_events > 0) {
        printf("T3_TEST stub commit-blocked gen=%u pending_events=%u (service required)\n",
               gen, x->pending_events);
        return M7G_T3_XS_REUSE;
    }
    if (fail_at("upload"))
        return M7G_T3_XS_UPLOAD;
    if (fail_at("reuse"))
        return M7G_T3_XS_REUSE;
    if (fail_at("commit"))
        return M7G_T3_XS_COMMIT;
    /* Deterministic stub-side content fingerprint (no assumptions about the
     * real transport; lets local smoke verify the producer->commit handoff). */
    {
        unsigned h = 2166136261u;
        for (uint32_t i = 0; i < x->bytes; i++) {
            h ^= pixels[i];
            h *= 16777619u;
        }
        printf("T3_XPORT stub commit gen=%u slot=%u w=%u h=%u fnv=0x%08x\n",
               gen, slot, x->w, x->h, h);
    }
    x->c.submitted++;
    x->c.commits++;
    x->pending_events++; /* one queued event per generation; service() dispatches it */
    if (x->retired_after_gen >= 0 && (long)gen >= x->retired_after_gen) {
        x->retired = 1;
        t3_retired_fired = 1;
    }
    return 0;
}

int m7g_t3_xport_service(m7g_t3_xport *x, int timeout_ms)
{
    if (fail_at("service"))
        return M7G_T3_XS_DRAIN;
    if (!x)
        return 0;
    x->service_calls++;
    /* timeout_ms == 0 is the no-pacing per-swap progress call: dispatch all
     * queued events without sleeping.  A positive timeout models a bounded
     * wait (short sleep), like the real poll-based service. */
    if (timeout_ms > 0) {
        struct timespec ts = { 0, (long)timeout_ms * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (x->pending_events) {
        printf("T3_TEST stub service dispatched=%u (releases=%u)\n",
               x->pending_events, x->c.releases + x->pending_events);
        x->c.releases += x->pending_events;
        x->pending_events = 0;
    }
    return 0;
}

int m7g_t3_xport_drain(m7g_t3_xport *x)
{
    (void)x;
    if (fail_at("drain"))
        return M7G_T3_XS_DRAIN;
    return 0;
}

int m7g_t3_xport_retired(const m7g_t3_xport *x)
{
    return x ? x->retired : 0;
}

void m7g_t3_xport_get_counters(const m7g_t3_xport *x, m7g_t3_xport_counters *c)
{
    *c = x->c;
}

int m7g_t3_xport_destroy(m7g_t3_xport *x)
{
    if (!x)
        return 0;
    printf("T3_TEST service_calls=%u\n", x->service_calls);
    if (fail_at("teardown")) {
        printf("T3_TEARDOWN_UNSAFE resources-left-to-process-exit=1 (stub injection)\n");
        return M7G_T3_XS_TEARDOWN;
    }
    free(x);
    return 0;
}
