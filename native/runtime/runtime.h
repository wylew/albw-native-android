/* Native runtime shared by the platform layer, SDK replacements and intrinsics. */
#pragma once
#include <stdint.h>

/* print "FATAL: ...", the native backtrace, and abort */
_Noreturn void runtime_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* native/runtime/crash.c: crash reports (signal, pc, frame, backtrace) to stderr + crash.log */
void runtime_install_crash_handler(void);
void prof_init(void);    /* ALBW_PROFILE=<file>: sampling CPU profiler (prof.c) */
void prof_frame(void);   /* once per presented frame: periodic dump */

/* thread-local SDK block (see intrinsics.c) */
uintptr_t albw_tls(void);
void albw_tls_set(void *block);   /* a game thread's fixed TLS block (sdk/os.c, pmem slot data) */

/* low memory for the x86-64 low-4GB mode (lowmem.c; plain allocations on i386) */
#include <stddef.h>
#include <pthread.h>
void *albw_low_map(size_t size, int prot, int extra_flags);
void albw_check_low(const void *p, const char *what);
int albw_thread_create_low(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg,
                           size_t stack_size);
int albw_run_on_low_stack(int (*fn)(void *), void *arg, size_t stack_size);

/* save states (savestate.c, ss_ctx.c; native/SAVESTATES.md) */
#include <stdint.h>
#if defined(__arm__)
typedef struct { uintptr_t r[26]; } AlbwCtx;  /* ARM: r4-r11, sp, lr, then d8-d15 (16 words) */
#else
typedef struct { uintptr_t r[8]; } AlbwCtx;   /* callee-saved registers, sp, return address */
#endif
__attribute__((returns_twice)) int albw_ctx_save(AlbwCtx *c);
__attribute__((noreturn)) void albw_ctx_jump(const AlbwCtx *c, int v);
__attribute__((noreturn)) void albw_ctx_run_on(void *stack_top, void (*fn)(void *), void *arg);
uintptr_t albw_ctx_sp(const AlbwCtx *c);
uintptr_t albw_canary_get(void);   /* stack-protector canary of this thread (ss_ctx.c) */
void albw_canary_set(uintptr_t v);
/* extra byte ranges a module wants in every save state (call once, e.g. at init) */
void albw_ss_add_range(const char *name, void *p, size_t n);
/* host threads that run game code outside the scheduler (the nn::snd sound thread): bracket each
 * piece of work; a save / load waits until no such work runs and holds new work back */
void albw_ss_host_enter(void);
void albw_ss_host_leave(void);

/* fixed-address memory for persistent save states (pmem.c) */
enum { PMEM_HEAP, PMEM_DEVICE, PMEM_ARENA, PMEM_STACKS, PMEM_COUNT };
void pmem_init(void);                       /* first thing in main */
int pmem_fixed(void);                       /* all areas at their fixed addresses */
void *pmem_area(int area, size_t *size);    /* reserved (PROT_NONE) area base */
#define PMEM_SLOT_DATA 0x1000u
int pmem_stack_slots(void);
void *pmem_stack_slot(int slot, void **lo, void **data);  /* stack top of a thread slot (NULL: none) */
void *pmem_alloc(size_t n);
void *pmem_calloc(size_t a, size_t b);
void *pmem_realloc(void *p, size_t n);      /* p from pmem_alloc (or malloc: plain realloc) */
void pmem_free(void *p);
size_t pmem_arena_used(void);
void pmem_arena_restore(const void *bytes, size_t n);
typedef struct { uint8_t *p; size_t n, cap, rd; int err; } SsBuf;
void ss_put(SsBuf *b, const void *p, size_t n);
int ss_get(SsBuf *b, void *p, size_t n);
const void *ss_peek(SsBuf *b, size_t n);
void ss_put_u32(SsBuf *b, uint32_t v);
uint32_t ss_get_u32(SsBuf *b);
void ss_section(SsBuf *b, uint32_t tag, const void *p, size_t n);
const void *ss_get_section(SsBuf *b, uint32_t tag, size_t *n);
