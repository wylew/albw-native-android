/* Low memory for the x86-64 low-4GB mode (native/64BIT.md).
 *
 * The decompiled game keeps addresses in 32-bit words (game objects, its data, Ghidra-derived C
 * with int-typed pointers). On x86-64 every address the game can see must therefore fit in 31
 * bits (below 2 GiB, so signed and unsigned int casts both give it back): the binary is linked
 * non-PIE at 0x400000, and all memory handed to the game (heaps, VRAM, thread stacks, the main
 * stack, the SDK TLS block) comes from here (MAP_32BIT: the first 2 GiB).
 * On i386 these are plain allocations: the 32-bit build's behaviour is unchanged. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "runtime.h"

#if defined(__x86_64__)
#define LOW_FLAGS MAP_32BIT
#else
#define LOW_FLAGS 0
#endif

void *albw_low_map(size_t size, int prot, int extra_flags) {
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | LOW_FLAGS | extra_flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    albw_check_low(p, "albw_low_map");
    return p;
}

void albw_check_low(const void *p, const char *what) {
#if defined(__x86_64__)
    if ((uintptr_t)p >= 0x80000000u)
        runtime_fatal("%s: address %p is not below 2 GiB (low-4GB mode, native/64BIT.md)", what, p);
#else
    (void)p; (void)what;
#endif
}

/* A thread whose stack (and, with glibc, its static TLS and thread descriptor, which live in a
 * user-provided stack) is low memory. On i386: an ordinary thread with `stack_size`. */
int albw_thread_create_low(pthread_t *out, const pthread_attr_t *attr_in, void *(*fn)(void *), void *arg,
                           size_t stack_size) {
#if defined(__x86_64__)
    pthread_attr_t attr;
    if (attr_in) attr = *attr_in; else pthread_attr_init(&attr);
    size_t guard = 0x10000;
    uint8_t *m = albw_low_map(stack_size + guard, PROT_READ | PROT_WRITE, MAP_NORESERVE);
    if (!m) return ENOMEM;
    mprotect(m, guard, PROT_NONE);  /* stack overflow guard below the stack */
    pthread_attr_setstack(&attr, m + guard, stack_size);
    int r = pthread_create(out, &attr, fn, arg);
    if (!attr_in) pthread_attr_destroy(&attr);
    if (r) munmap(m, stack_size + guard);
    return r;  /* the stack of a detached thread stays mapped after it exits (few threads) */
#else
    pthread_attr_t attr;
    if (attr_in) attr = *attr_in; else pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    int r = pthread_create(out, &attr, fn, arg);
    if (!attr_in) pthread_attr_destroy(&attr);
    return r;
#endif
}

/* Run fn(arg) on the calling thread on the fixed main-thread stack (pmem.c slot 0: at the same
 * address in every run, for persistent save states; low memory, as x86-64 also needs). Same thread:
 * SDL / Qt keep their main-thread state. */
static int (*g_low_fn)(void *);
static void *g_low_arg;
static int g_low_result;
#if defined(__ANDROID__)
/* bionic has no getcontext / makecontext / swapcontext: the save-state context helpers
 * (ss_ctx.c) switch stacks instead. Same thread, as below. */
static AlbwCtx g_ret_actx;
static void low_trampoline_a(void *unused) {
    (void)unused;
    g_low_result = g_low_fn(g_low_arg);
    albw_ctx_jump(&g_ret_actx, 1);
}
#else
static ucontext_t g_ret_ctx, g_low_ctx;
static void low_trampoline(void) { g_low_result = g_low_fn(g_low_arg); }
#endif

int albw_run_on_low_stack(int (*fn)(void *), void *arg, size_t stack_size) {
    void *lo = NULL;
    uint8_t *top = pmem_stack_slot(0, &lo, NULL);
    if (!top) runtime_fatal("main stack: no fixed stack slot");
    stack_size = (size_t)(top - (uint8_t *)lo);
    g_low_fn = fn;
    g_low_arg = arg;
#if defined(__ANDROID__)
    (void)stack_size;
    if (albw_ctx_save(&g_ret_actx) == 0) albw_ctx_run_on(top, low_trampoline_a, NULL);
    return g_low_result;
#else
    getcontext(&g_low_ctx);
    g_low_ctx.uc_stack.ss_sp = lo;
    g_low_ctx.uc_stack.ss_size = stack_size;
    g_low_ctx.uc_link = &g_ret_ctx;
    makecontext(&g_low_ctx, low_trampoline, 0);
    swapcontext(&g_ret_ctx, &g_low_ctx);
    return g_low_result;
#endif
}
