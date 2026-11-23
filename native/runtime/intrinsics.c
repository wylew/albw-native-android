/* Native implementations of the operations Ghidra could not express in C (ghidra_types.h) and of
 * the helpers the portability layer introduced (albw_native.h).
 *
 * Pure operations (saturation, exclusive load/store) are implemented exactly with C11 atomics.
 * System calls are NOT emulated: every svc sits inside a Nintendo SDK function, and those SDK
 * functions are replaced by native code at the API level. software_interrupt() is a tripwire that
 * names the SDK function still waiting for its native replacement. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime.h"

/* ------------------------------------------------------------ exclusive access (ldrex/strex)
 * Ghidra renders `ldrex; ...; strex; loop` as
 *     v = *p; ok = hasExclusiveAccess(p); if (ok) *p = new; } while (!ok);
 * Per-thread monitor: ExclusiveAccess(p) records the address and the value seen; the store is only
 * allowed if nobody changed the word since (compare-and-swap semantics, as on the hardware). */
#if defined(__ANDROID__)
#include "tlv.h"
#define monitor (albw_tlv()->monitor)   /* fixed address: runtime/tlv.h */
#else
static _Thread_local struct { void *addr; uint32_t seen; bool armed; } monitor;
#endif

void ExclusiveAccess(void *p) {
    monitor.addr = p;
    monitor.seen = atomic_load_explicit((_Atomic uint32_t *)p, memory_order_acquire);
    monitor.armed = true;
}

bool hasExclusiveAccess(void *p) {
    /* Ghidra often omits the ExclusiveAccess() call for the load: arm on first use */
    if (!monitor.armed || monitor.addr != p) ExclusiveAccess(p);
    uint32_t cur = atomic_load_explicit((_Atomic uint32_t *)p, memory_order_acquire);
    bool ok = cur == monitor.seen;
    monitor.armed = false;
    return ok;
}

void ClearExclusiveLocal(void) { monitor.armed = false; }

void DataMemoryBarrier(uint32_t opt) { (void)opt; atomic_thread_fence(memory_order_seq_cst); }
void DataSynchronizationBarrier(uint32_t opt) { (void)opt; atomic_thread_fence(memory_order_seq_cst); }
void InstructionSynchronizationBarrier(uint32_t opt) { (void)opt; atomic_thread_fence(memory_order_seq_cst); }

/* ------------------------------------------------------------ arithmetic */
uint32_t count_leading_zeroes(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }

uint32_t UnsignedSaturate(int32_t x, uint32_t bits) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    return (uint32_t)(x < 0 ? 0 : x > hi ? hi : x);
}
bool UnsignedDoesSaturate(int32_t x, uint32_t bits) { return x < 0 || (int64_t)x > ((int64_t)1 << bits) - 1; }

uint32_t SignedSaturate(int32_t x, uint32_t bits) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    return (uint32_t)(int32_t)(x < lo ? lo : x > hi ? hi : x);
}
bool SignedDoesSaturate(int32_t x, uint32_t bits) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
    return x > hi || x < -hi - 1;
}

/* ------------------------------------------------------------ tripwires */
void software_interrupt(uint32_t svc) {
    runtime_fatal("svc 0x%02X reached from %p: the SDK function containing it needs its native "
                  "replacement (see native/sdk_map.json)", svc, __builtin_return_address(0));
}

void halt_baddata(void) {
    runtime_fatal("halt_baddata reached from %p: Ghidra could not decode this function; "
                  "it needs hand decompilation", __builtin_return_address(0));
}

void halt_unimplemented(void) {
    runtime_fatal("halt_unimplemented reached from %p", __builtin_return_address(0));
}

bool isCurrentModePrivileged(void) { return false; }

uint32_t coprocessor_movefromRt(uint32_t cp, uint32_t op1, uint32_t op2, uint32_t crn, uint32_t crm) {
    runtime_fatal("coprocessor read p%u c%u c%u from %p", cp, crn, crm, __builtin_return_address(0));
}

void coprocessor_moveto(uint32_t cp, uint32_t op1, uint32_t op2, uint32_t rt, uint32_t crn, uint32_t crm) {
    runtime_fatal("coprocessor write p%u c%u c%u from %p", cp, crn, crm, __builtin_return_address(0));
}

/* ------------------------------------------------------------ thread-local block
 * The SDK keeps per-thread data in a 0x200-byte block (the 3DS TLS page). Native: a real
 * thread-local array. Returned as a full pointer-sized integer (64-bit clean). */
/* Game threads (the main thread and CreateThread threads, sdk/os.c) use the TLS block in their
 * fixed stack slot (runtime/pmem.c: the same address in every run, for persistent save states);
 * other threads a thread-local block (x86-64: low memory, the game keeps the address in 32-bit
 * words). */
#if defined(__ANDROID__)
#define tls_slot (albw_tlv()->tls_slot)
#else
static _Thread_local uint8_t *tls_slot;
#endif
void albw_tls_set(void *block) { tls_slot = block; }
#if defined(__x86_64__)
#include <sys/mman.h>
static _Thread_local uint8_t *tls_low;

uintptr_t albw_tls(void) {
    if (tls_slot) return (uintptr_t)tls_slot;
    if (!tls_low) {
        tls_low = albw_low_map(0x1000, PROT_READ | PROT_WRITE, 0);
        if (!tls_low) runtime_fatal("albw_tls: no low memory");
    }
    return (uintptr_t)tls_low;
}
#else
static _Thread_local _Alignas(16) uint8_t tls_block[0x200];

uintptr_t albw_tls(void) { return tls_slot ? (uintptr_t)tls_slot : (uintptr_t)tls_block; }
#endif
