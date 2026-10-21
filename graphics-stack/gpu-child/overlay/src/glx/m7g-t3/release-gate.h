#pragma once
#include <stdint.h>

/* CPU ordering guard only. The original GPU semaphore wait is STILL required.
 * release implies consumer copy/discard fence completion in the accepted server.
 * This state never grants access to pixel memory and never signals a semaphore.
 */
struct m10k_release_state {
    unsigned pending;
    uint64_t serial;
    uint64_t duplicates;
};

static inline int m10k_release_begin(struct m10k_release_state *s)
{
    if (s->pending) return 25;
    s->pending = 1;
    s->serial++;
    return 0;
}

static inline int m10k_release_received(struct m10k_release_state *s)
{
    if (!s->pending) { s->duplicates++; return 0; }
    s->pending = 0;
    return 1;
}

static inline int m10k_release_wait(struct m10k_release_state *s,
    uint64_t deadline_ns, uint64_t (*now)(void *),
    int (*pump)(void *, int), void *opaque)
{
    while (s->pending) {
        uint64_t t = now(opaque);
        if (t >= deadline_ns) return 25;
        uint64_t remaining_ms = (deadline_ns - t + 999999) / 1000000;
        int ms = remaining_ms < 50 ? (int)remaining_ms : 50;
        int rc = pump(opaque, ms);
        if (rc) return rc;
    }
    return 0;
}
