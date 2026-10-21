#ifndef M10K_GEOMETRY_DEFER_H
#define M10K_GEOMETRY_DEFER_H
#include <stdint.h>
#define M10K_GEOMETRY_BOUND_NS UINT64_C(2000000000)
struct m10k_geometry_wait { uint64_t key, first_ns; unsigned skipped; };
/* One independent, bounded budget per drawable, under the existing t3_lock. */
static inline int m10k_geometry_defer(struct m10k_geometry_wait *items,
                                    unsigned count,uint64_t key,uint64_t now,
                                    unsigned *skipped) {
    struct m10k_geometry_wait *free_item=0,*item=0;
    for(unsigned i=0;i<count;i++) {
        if(items[i].key==key) { item=&items[i]; break; }
        if(!items[i].key && !free_item) free_item=&items[i];
    }
    if(!key || (!item && !free_item)) return -1;
    if(!item) { item=free_item; item->key=key; item->first_ns=now; item->skipped=0; }
    *skipped=++item->skipped;
    if(now<item->first_ns || now-item->first_ns>=M10K_GEOMETRY_BOUND_NS) return -1;
    return 1;
}
static inline unsigned m10k_geometry_clear(struct m10k_geometry_wait *items,
                                          unsigned count,uint64_t key) {
    for(unsigned i=0;i<count;i++) if(items[i].key==key) {
        unsigned skipped=items[i].skipped;
        items[i]=(struct m10k_geometry_wait){0}; return skipped;
    }
    return 0;
}
#endif
