/* M7g T3 — private Steam-compatibility GLX/drisw/Zink adapter env gate.
 *
 * Task-local.  Enabled only for the exact value M7G_T3_ADAPTER=1
 * (fail-closed: anything else, including unset, leaves the upstream path
 * byte-identical).  The frozen T1/T2 env gates are separate.
 */
#ifndef M7G_T3_H
#define M7G_T3_H

#include <stdlib.h>
#include <stdbool.h>

static inline bool
m7g_t3_adapter_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("M7G_T3_ADAPTER");
      cached = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
   }
   return cached != 0;
}

#endif /* M7G_T3_H */
