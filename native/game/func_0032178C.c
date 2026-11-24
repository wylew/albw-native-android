/* USA 0032178C: call scene listener n (event command 0x2b, func_0030A5B4): the 16-byte delegate n of
 * the table at obj+0x54 (n < 17), invoked through its vtable slot 0 with (delegate, arg). Same as
 * the Ghidra text; native to trace the delegates: ALBW_TRACE_LISTENER=1 logs every call (index,
 * argument, the entry's four words, the invoke function) and flags re-entry (README: Android stop
 * A16, the endless 0030A5B4 -> listener -> 004EFB10 frame update -> event -> 0030A5B4 recursion).
 * Confidence: high (4 instructions + the tail call, our ARM build checked). */
#include <stdio.h>
#include <stdlib.h>
#include "ghidra_types.h"

uint32_t func_0032178C(int obj, uint32_t idx, uint32_t arg) {
    if (idx >= 0x11) return 0;
    uint32_t *e = (uint32_t *)(uintptr_t)(*(uint32_t *)(uintptr_t)(obj + 0x54) + idx * 0x10);
    uint32_t (*inv)(uint32_t *, uint32_t) = (uint32_t (*)(uint32_t *, uint32_t))(uintptr_t)*(uint32_t *)(uintptr_t)e[0];
    static int tr = -1;
    static int depth;
    if (tr < 0) tr = getenv("ALBW_TRACE_LISTENER") != NULL;
    if (tr) fprintf(stderr, "listener: depth %d obj %#x idx %u arg %#x entry %#x %#x %#x %#x invoke %p\n", depth, (unsigned)obj,
                    (unsigned)idx, (unsigned)arg, e[0], e[1], e[2], e[3], (void *)inv);
    depth++;
    uint32_t r = inv(e, arg);
    depth--;
    return r;
}
