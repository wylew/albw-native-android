/* Fixed-address memory for persistent save states (native/SAVESTATES.md).
 *
 * A state written to disk is loaded by a later run of the same binary; every pointer it contains
 * must mean the same thing there. The binary is non-PIE (code and static data are fixed), VRAM is
 * mapped at its 3DS address; everything else the saved data can point into is placed here, at
 * addresses reserved before anything else maps memory (pmem_init, first thing in main):
 *
 *   PMEM_HEAP    game heap (sdk/memory.c, 3DS 0x08000000)         64 MiB
 *   PMEM_DEVICE  device memory / linear heap (sdk/memory.c)       64 MiB
 *   PMEM_ARENA   native objects the game state refers to: kernel objects and scheduler records
 *                (sdk/os.c), command lists / buffers and display buffers (sdk/gfx.c), vertex
 *                layouts and blob ops (render/h3d.c), decoder tables (sdk/h3d.c); allocator below
 *   PMEM_STACKS  host stacks of the game threads: slot 0 the main game thread, 1.. CreateThread
 *
 * If a reservation fails (address taken), that area falls back to ordinary memory and states can
 * still be saved / loaded within the run, but not across runs (pmem_fixed() == 0).
 *
 * The arena allocator keeps all its metadata inside the arena, so restoring the arena bytes
 * restores the allocator too. First fit over an address-ordered free list; blocks 16-aligned. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "runtime.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static const struct { uintptr_t base; size_t size; } AREA[PMEM_COUNT] = {
    [PMEM_HEAP] = {0x40000000u, 0x04000000u},
    [PMEM_DEVICE] = {0x44000000u, 0x04000000u},
    [PMEM_ARENA] = {0x48000000u, 0x10000000u},
    [PMEM_STACKS] = {0x58000000u, 0x04000000u},
};
static uint8_t *g_base[PMEM_COUNT];
static int g_fixed = -1;

void pmem_init(void) {
    if (g_fixed >= 0) return;
    g_fixed = !getenv("ALBW_PMEM_OFF");
    for (int i = 0; i < PMEM_COUNT; i++) {
        void *p = MAP_FAILED;
        if (g_fixed)
            p = mmap((void *)AREA[i].base, AREA[i].size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                     -1, 0);
        if (p != MAP_FAILED && p != (void *)AREA[i].base) { munmap(p, AREA[i].size); p = MAP_FAILED; }
        if (p == MAP_FAILED) {
            if (g_fixed) fprintf(stderr, "pmem: cannot reserve %#lx (+%#zx): save states will not survive a restart\n",
                                 (unsigned long)AREA[i].base, AREA[i].size);
            g_fixed = 0;
            p = albw_low_map(AREA[i].size, PROT_NONE, MAP_NORESERVE);
            if (!p) runtime_fatal("pmem: cannot map %#zx bytes", AREA[i].size);
        }
        g_base[i] = p;
    }
}

int pmem_fixed(void) { return g_fixed > 0; }
void *pmem_area(int i, size_t *size) {
    pmem_init();
    if (size) *size = AREA[i].size;
    return g_base[i];
}

/* ------------------------------------------------------------------ thread stacks
 * slot 0: the main game thread (16 MiB), slots 1..: CreateThread threads (1 MiB each); each slot
 * has a guard below it and starts with PMEM_SLOT_DATA bytes of per-thread data (the thread's SDK
 * TLS block and wait record: fixed addresses the game state may point to), the stack above it. */
#define STACK_GUARD 0x10000u
#define MAIN_STACK (16u << 20)
#define WORKER_STACK (1u << 20)
static uint8_t *slot_lo(int slot, size_t *size) {
    size_t off = slot == 0 ? 0 : MAIN_STACK + STACK_GUARD + (size_t)(slot - 1) * (WORKER_STACK + STACK_GUARD);
    *size = slot == 0 ? MAIN_STACK : WORKER_STACK;
    if (off + STACK_GUARD + *size > AREA[PMEM_STACKS].size) return NULL;
    return g_base[PMEM_STACKS] + off + STACK_GUARD;
}
int pmem_stack_slots(void) {
    return 1 + (int)((AREA[PMEM_STACKS].size - MAIN_STACK - STACK_GUARD) / (WORKER_STACK + STACK_GUARD));
}
/* the per-thread data of the stack slot that contains sp, or NULL (Android: runtime/tlv.c) */
void *pmem_slot_data_of(uintptr_t sp) {
    if (!g_base[PMEM_STACKS]) return NULL;
    uintptr_t b = (uintptr_t)g_base[PMEM_STACKS];
    if (sp < b || sp >= b + AREA[PMEM_STACKS].size) return NULL;
    for (int k = 0; k < pmem_stack_slots(); k++) {
        size_t size;
        uint8_t *m = slot_lo(k, &size);
        if (m && sp >= (uintptr_t)m && sp < (uintptr_t)m + size) return m;
    }
    return NULL;
}

/* the slot's memory made usable: *data = its per-thread data, *lo / return = the stack [lo, top) */
void *pmem_stack_slot(int slot, void **lo, void **data) {
    pmem_init();
    size_t size;
    uint8_t *m = slot >= 0 ? slot_lo(slot, &size) : NULL;
    if (!m || mprotect(m, size, PROT_READ | PROT_WRITE)) return NULL;
    if (data) *data = m;
    if (lo) *lo = m + PMEM_SLOT_DATA;
    return m + size;
}

/* ------------------------------------------------------------------ arena allocator */
typedef struct Blk { uint32_t size, free; uint32_t next, pad; } Blk;   /* next: offset of the next free block */
typedef struct {
    uint32_t magic, top, committed, free_head;
} ArenaHdr;
#define ARENA_MAGIC 0x504D454Du   /* "PMEM" */
#define HDR ((ArenaHdr *)g_base[PMEM_ARENA])
#define AT(off) ((Blk *)(g_base[PMEM_ARENA] + (off)))
static pthread_mutex_t g_am = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void commit_to(uint32_t end) {
    if (end <= HDR->committed) return;
    uint32_t c = (end + 0xFFFFFu) & ~0xFFFFFu;   /* 1 MiB steps */
    if (c > AREA[PMEM_ARENA].size) runtime_fatal("pmem: arena full (%u bytes)", c);
    if (mprotect(g_base[PMEM_ARENA] + HDR->committed, c - HDR->committed, PROT_READ | PROT_WRITE))
        runtime_fatal("pmem: arena commit failed");
    HDR->committed = c;
}

static int g_arena_ready;
static void arena_init_locked(void) {
    if (g_arena_ready) return;
    g_arena_ready = 1;
    pmem_init();
    if (mprotect(g_base[PMEM_ARENA], 0x100000, PROT_READ | PROT_WRITE)) runtime_fatal("pmem: arena commit failed");
    if (HDR->magic == ARENA_MAGIC) return;
    HDR->magic = ARENA_MAGIC;
    HDR->committed = 0x100000;
    HDR->top = 64;
    HDR->free_head = 0;
}

void *pmem_alloc(size_t n) {
    if (!n) n = 1;
    if (n > 0x08000000u) return NULL;
    uint32_t need = (uint32_t)((n + sizeof(Blk) + 15) & ~(size_t)15);
    pthread_mutex_lock(&g_am);
    arena_init_locked();
    uint32_t *pp = &HDR->free_head;
    Blk *b = NULL;
    while (*pp) {
        Blk *c = AT(*pp);
        if (c->size >= need) {
            uint32_t off = *pp;
            if (c->size - need >= 64) {   /* split: the rest stays free */
                Blk *r = AT(off + need);
                r->size = c->size - need;
                r->free = 1;
                r->next = c->next;
                *pp = off + need;
                c->size = need;
            } else {
                *pp = c->next;
            }
            b = c;
            break;
        }
        pp = &c->next;
    }
    if (!b) {
        uint32_t off = HDR->top;
        commit_to(off + need);
        HDR->top = off + need;
        b = AT(off);
        b->size = need;
    }
    b->free = 0;
    b->next = 0;
    pthread_mutex_unlock(&g_am);
    return b + 1;
}

void *pmem_calloc(size_t a, size_t b) {
    if (b && a > SIZE_MAX / b) return NULL;
    void *p = pmem_alloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

static int in_arena(const void *p) {
    return g_base[PMEM_ARENA] && (const uint8_t *)p > g_base[PMEM_ARENA] &&
           (const uint8_t *)p < g_base[PMEM_ARENA] + AREA[PMEM_ARENA].size;
}

void pmem_free(void *p) {
    if (!p) return;
    if (!in_arena(p)) { free(p); return; }
    pthread_mutex_lock(&g_am);
    Blk *b = (Blk *)p - 1;
    uint32_t off = (uint32_t)((uint8_t *)b - g_base[PMEM_ARENA]);
    b->free = 1;
    uint32_t *pp = &HDR->free_head, prev = 0;
    while (*pp && *pp < off) { prev = *pp; pp = &AT(*pp)->next; }
    b->next = *pp;
    *pp = off;
    if (b->next && off + b->size == b->next) {            /* merge with the following block */
        Blk *n = AT(b->next);
        b->size += n->size;
        b->next = n->next;
    }
    if (prev && prev + AT(prev)->size == off) {           /* merge into the previous block */
        Blk *pb = AT(prev);
        pb->size += b->size;
        pb->next = b->next;
    }
    pthread_mutex_unlock(&g_am);
}

void *pmem_realloc(void *p, size_t n) {
    if (!p) return pmem_alloc(n);
    if (!in_arena(p)) return realloc(p, n);
    size_t have = ((Blk *)p - 1)->size - sizeof(Blk);
    if (n <= have) return p;
    void *q = pmem_alloc(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    pmem_free(p);
    return q;
}

/* save states: the arena's used bytes [0, top) (allocator state included) */
size_t pmem_arena_used(void) {
    pthread_mutex_lock(&g_am);
    arena_init_locked();
    size_t n = HDR->top;
    pthread_mutex_unlock(&g_am);
    return n;
}
void pmem_arena_restore(const void *bytes, size_t n) {
    pthread_mutex_lock(&g_am);
    arena_init_locked();
    uint32_t committed = HDR->committed;
    commit_to((uint32_t)n);
    committed = HDR->committed > committed ? HDR->committed : committed;
    memcpy(g_base[PMEM_ARENA], bytes, n);
    HDR->committed = committed;   /* what is mapped here, not what was mapped in the saving run */
    pthread_mutex_unlock(&g_am);
}

/* ------------------------------------------------------------------ byte streams for states */
void ss_put(SsBuf *b, const void *p, size_t n) {
    if (b->err) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 1 << 20;
        while (c < b->n + n) c *= 2;
        uint8_t *q = realloc(b->p, c);
        if (!q) { b->err = 1; return; }
        b->p = q;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
}
int ss_get(SsBuf *b, void *p, size_t n) {
    if (b->err || b->rd + n > b->n) { b->err = 1; if (p && n) memset(p, 0, n); return -1; }
    if (n) memcpy(p, b->p + b->rd, n);
    b->rd += n;
    return 0;
}
const void *ss_peek(SsBuf *b, size_t n) {   /* the next n bytes in place (advances) */
    if (b->err || b->rd + n > b->n) { b->err = 1; return NULL; }
    const void *p = b->p + b->rd;
    b->rd += n;
    return p;
}
void ss_put_u32(SsBuf *b, uint32_t v) { ss_put(b, &v, 4); }
uint32_t ss_get_u32(SsBuf *b) { uint32_t v = 0; ss_get(b, &v, 4); return v; }
/* a tagged section: tag, length, bytes */
void ss_section(SsBuf *b, uint32_t tag, const void *p, size_t n) { ss_put_u32(b, tag); ss_put_u32(b, (uint32_t)n); ss_put(b, p, n); }
const void *ss_get_section(SsBuf *b, uint32_t tag, size_t *n) {
    uint32_t t = ss_get_u32(b), len = ss_get_u32(b);
    if (b->err || t != tag) { b->err = 1; return NULL; }
    *n = len;
    return ss_peek(b, len);
}
