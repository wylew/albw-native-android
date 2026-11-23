/* Android: the port's per-thread variables at fixed addresses (README: Android save states).
 *
 * On the PC these are _Thread_local; glibc keeps them in the thread's static TLS, which sits in the
 * thread's fixed stack slot (runtime/pmem.c), so frames restored from a save state that cached a
 * thread-local's address stay valid in a later run. Android's 32-bit runtime emulates TLS
 * (__emutls_get_address) with malloc'd blocks at different addresses every run: after a load the
 * restored frames used the saving run's heap pointers (SIGBUS in ss_park). Here the variables live
 * at slot data + 0xF00 of the thread's stack slot (same address every run); threads without a slot
 * (audio, SDL) get a heap copy, they are not part of a state. */
#pragma once
#if defined(__ANDROID__)
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t t_id;
    void *t_self, *t_sched, *t_wp, *t_ss;                  /* sdk/os.c */
    struct { void *addr; uint32_t seen; bool armed; } monitor;   /* runtime/intrinsics.c */
    uint8_t *tls_slot;                                      /* runtime/intrinsics.c */
} AlbwTlv;
AlbwTlv *albw_tlv(void);
#define ALBW_TLV_OFFSET 0xF00u   /* in the slot's PMEM_SLOT_DATA (TLS block +0, wait record +0x200) */
#endif
