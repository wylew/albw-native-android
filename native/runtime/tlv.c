/* Android: per-thread variables at fixed addresses (runtime/tlv.h). */
#if defined(__ANDROID__)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "tlv.h"

void *pmem_slot_data_of(uintptr_t sp);   /* runtime/pmem.c */

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void make_key(void) { pthread_key_create(&g_key, NULL); }

AlbwTlv *albw_tlv(void) {
    pthread_once(&g_once, make_key);
    AlbwTlv *p = pthread_getspecific(g_key);
    if (p) return p;
    /* first use in this thread: a game thread's first code runs on its own slot stack */
    uint8_t *d = pmem_slot_data_of((uintptr_t)__builtin_frame_address(0));
    p = d ? (AlbwTlv *)(d + ALBW_TLV_OFFSET) : calloc(1, sizeof *p);
    if (!p) runtime_fatal("albw_tlv: out of memory");
    if (d) memset(p, 0, sizeof *p);   /* a new thread: zero, as _Thread_local starts */
    pthread_setspecific(g_key, p);
    return p;
}
#endif
