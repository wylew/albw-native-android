/* Native replacement of the 3DS graphics SDK bottom layers (Tier A of native/GRAPHICS_STACK.md):
 * the GSP client (gsp::Gpu IPC wrappers + GX command builders, USA 0x003E31E4-0x003E3F20 and the
 * IPC stubs 0x0010AE3C-0x0010AED0) and libnngx (USA 0x005FE800-0x00602A00), on SDL3 + OpenGL 3.3
 * through native/platform/platform.h.
 *
 * Every func_<USA ADDR> below is a STRONG definition replacing the weak decompiled one. Nothing is
 * emulated: no PICA200 command word is ever read, no GSP shared memory / GX queue / GPU register
 * exists. Each function is reimplemented by its API meaning:
 *
 *   command lists   nngxGen/Bind/Storage/Clear/Delete/Run/Stop/Wait..., GetCmdlistParameteri,
 *                   MoveCommandbufferPointer: bookkeeping only. Each list owns a host scratch buffer
 *                   the game writes PICA words into (CURRENT_BUFADDR = 0x208 + Move...). The words
 *                   are DISCARDED, never interpreted. Used size, request counts, top address etc.
 *                   stay consistent so the game's writers and its sub-list logic (002FE7B4/002FE918)
 *                   work unchanged.
 *   requests        DMA, MemoryFill, DisplayTransfer (B2L / L2B), TextureCopy, 3D chunks are stored
 *                   in a native request array per list and executed when the list runs (RunCmdlist,
 *                   or immediately if appended to the running list), like nngx's dispatcher 0060226C
 *                   but synchronously: the "GPU" is never busy, so Wait/IsRunning return at once.
 *                   Execution meaning:
 *                     DMA            memmove(dst, src, size) on host memory (texture uploads)
 *                     MemoryFill     remember {range, value, width}; nothing is written
 *                     B2L / TexCopy  if the destination is a display buffer: that buffer now holds
 *                                    "the frame"; its colour = the fill that covered the source
 *                                    render target, decoded with the transfer's source format
 *                     3D chunk       discarded (Tier B replaces the producers)
 *                     L2B            no-op (HOME-menu capture conversion)
 *                   The list-completion callback (00601450 / 005FFBDC) is called after a run, with
 *                   the completed request count, as the GSP interrupt thread did.
 *   display         Gen/Bind/DisplaybufferStorage allocate host memory; ActiveDisplay selects the
 *                   slot (0x400 top, 0x401 bottom, 0x410 top right eye). SwapBuffers(0x400/0x401/
 *                   0x402) shows each screen's bound display buffer from the next vblank on
 *                   (it does not block): the next CheckVSync / WaitVSync on the GL thread presents
 *                   it: the screen render target is cleared with that frame's colour (plat_begin_screen
 *                   + glClearColor/glClear) and plat_end_frame() composites, swaps and paces 60 Hz;
 *                   plat_poll() pumps SDL every present (quit -> clean exit).
 *   vsync           a host 60 Hz counter (CheckVSync / WaitVSync): sead's vblank-interval logic
 *                   (os.c 00496238 / 0049ABAC) runs unchanged on it.
 *   VRAM            6 MiB of host memory. It is mapped at the 3DS address 0x1F000000 when that
 *                   host range is free (so the game's own VRAM range checks, e.g. 00504ED0
 *                   `dst - 0x1F000000 < 0x600000`, keep working, and the values fit the int fields
 *                   of the decompiled code on both -m32 and -m64); otherwise anywhere below 2 GiB.
 *                   nngxGetVramStartAddr (00600660) / GetVramEndAddr (0010BB14) return that range.
 *   addresses       GetPhysicalAddr (001139F8/001139FC) is the IDENTITY: physical addresses only
 *                   end up in PICA words, which are discarded; host pointers must never get the
 *                   3DS FCRAM/VRAM offsets (+0x0C000000 / -0x07000000) applied.
 *   GSP             every IPC wrapper returns success without IPC; GX builders execute their
 *                   command natively (same executors as the nngx requests); the framebuffer-info
 *                   write (003E34D8) selects what a screen shows.
 *
 * Tier B (native/render): requests are also executed on real GL render targets when a GL context
 * is current on the executing thread. 3D chunks run the native ops recorded by the replaced
 * producers (sdk/gr.c, render_exec_cmds); MemoryFill = glClear of the surface at that address
 * (render_fill); B2L / TextureCopy = blit from the surface into the display image / surface at the
 * destination (render_transfer, render_block_copy); DMA marks cached textures dirty; SwapBuffers
 * scans each screen's display image out into the platform screen target (render_scanout). The
 * Tier A colour tracking above stays as the fallback for a display buffer with no GL image.
 *
 * 64-bit: the decompiled callers keep command-buffer addresses in 32-bit ints (0x206/0x208 outputs,
 * 0041F7CC/0041F7DC, 002FE7B4 fields). All memory this file hands out (scratch buffers, display
 * buffers, VRAM) is therefore mapped below 2 GiB (MAP_32BIT on x86-64), so the 32-bit value
 * round-trips (zero- or sign-extended). Out-parameters that the callers declare as int are written
 * as 32-bit values. When those callers are retyped, use the typed accessors at the end of this file
 * (albw_gfx_cmdbuf_current etc.) instead.
 *
 * Threading: every entry point takes one recursive mutex (game loader threads use the explicit-list
 * DMA path 00504ED0). OpenGL is only touched on the thread where the GL context is current (the
 * main thread: entry.c calls plat_init there and the sead frame loop runs on it). If SwapBuffers is
 * called from another thread, the frame is only recorded (and a warning printed once); the GL
 * thread presents it the next time it enters this file (CheckVSync/WaitVSync) or calls
 * albw_gfx_service().
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "../platform/platform.h"
#include "../render/render.h"
#include "../runtime/runtime.h"
#include "../include/gptr.h" /* ALBW_GP: game pointer words (x86-64 low-4GB build) */

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ================================================================== constants */

/* nngx display ids (from the game's own calls, e.g. 0032259C / 00322CF4) */
#define NN_GX_DISPLAY0      0x400 /* top LCD (left eye) */
#define NN_GX_DISPLAY1      0x401 /* bottom LCD */
#define NN_GX_DISPLAY_BOTH  0x402
#define NN_GX_DISPLAY0_EXT  0x410 /* top LCD right eye */

/* nngx GL-style pixel formats (literal pools of 00601AF0 / 005FFAF0 / 004964F8) */
#define FMT_RGBA8   0x8058
#define FMT_RGB8    0x8051
#define FMT_RGB5_A1 0x8057
#define FMT_RGBA4   0x8056
#define FMT_RGB565  0x8D62

/* PICA framebuffer format codes (what a DisplayTransfer / MemoryFill means) */
enum { PICA_RGBA8 = 0, PICA_RGB8 = 1, PICA_RGB565 = 2, PICA_RGB5A1 = 3, PICA_RGBA4 = 4 };

/* nngx memory areas (allocator / DisplaybufferStorage / GetVramStartAddr) */
#define AREA_FCRAM 0x10000
#define AREA_VRAMA 0x20000
#define AREA_VRAMB 0x30000

/* VRAM: 3DS virtual layout 0x1F000000..0x1F5FFFFF, VRAM-A = first half, VRAM-B = second half */
#define VRAM_3DS_BASE 0x1F000000u
#ifndef ALBW_ANDROID_VRAM
#define ALBW_ANDROID_VRAM 0x3E000000u   /* Android VRAM address: same value as native/albw_native.h */
#endif
#define VRAM_SIZE     0x600000u
#define VRAM_HALF     0x300000u

#define VBLANK_NS 16666667ull       /* host vblank: 60 Hz, same grid as plat_end_frame */
#define VCOUNT_WRAP 0x7FFFFFFFu     /* the SDK counter wraps like 006020AC (mod 0x7FFFFFFF) */
#define KEEPALIVE_NS 100000000ull   /* re-present at least every 100 ms while the game swaps */
#define PUMP_NS 4000000ull          /* SDL pump interval while the game spins on CheckVSync */

#define REQ_RECORD_SIZE 0x1C        /* nngx request record size (006005B0) */

/* ================================================================== small helpers */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void albw_sched_block_begin(void);  /* native/sdk/os.c: other game threads run meanwhile */
void albw_sched_block_end(void);

static void sleep_ns(uint64_t ns) {
    struct timespec req = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}, rem;
    albw_sched_block_begin();
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
    albw_sched_block_end();
}

static bool g_trace;

static void gfx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gfx_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("gfx: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define WARN_ONCE(...) do { static bool once_; if (!once_) { once_ = true; gfx_log(__VA_ARGS__); } } while (0)

/* Host memory the game may keep in a 32-bit int: below 2 GiB on x86-64, anywhere on i386. */
static void *low_map(size_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
    flags |= MAP_32BIT;
#endif
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) runtime_fatal("gfx: mmap of %zu bytes failed (%s)", size, strerror(errno));
    return p;
}

static size_t page_round(size_t n) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    return (n + pg - 1) & ~(pg - 1);
}

/* address as the 32-bit value the decompiled callers store (see the 64-bit note above) */
static uint32_t addr32(const void *p) {
    uintptr_t v = (uintptr_t)p;
#if UINTPTR_MAX > 0xFFFFFFFFu
    if (v >> 32) WARN_ONCE("address %p does not fit a 32-bit game field", p);
#endif
    return (uint32_t)v;
}

/* game objects with the 3DS layout: 32-bit fields at fixed offsets */
#define OBJ_U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))

/* ================================================================== state */

typedef enum { RQ_DMA, RQ_CMD3D, RQ_FILL, RQ_B2L, RQ_L2B, RQ_COPY } ReqType;

typedef struct {
    uint8_t type;
    uint8_t irq; /* per-request completion callback (005FFBDC(n)) */
    union {
        struct { const uint8_t *src; uint8_t *dst; uint32_t size; } dma;
        struct { uintptr_t addr; uint32_t size; } cmd;
        struct { uintptr_t start[2]; uint32_t size[2], value[2]; uint8_t bits[2]; } fill;
        struct { uintptr_t src, dst; uint16_t sw, sh, dw, dh; uint8_t sfmt, dfmt, flip, aa; } xfer;
        struct { uintptr_t src, dst; uint32_t size, sw, sgap, dw, dgap; } copy;
    } u;
} Request;

typedef struct CmdList {
    uint32_t id;            /* nngx object id (0 for game-owned explicit objects) */
    void *game_obj;         /* explicit object (005FF938), else NULL */
    uint8_t *buf;           /* scratch command buffer (host, below 2 GiB) */
    uint32_t buf_size;      /* NN max bufsize */
    uint8_t *req_rec;       /* 0x1C-byte request records (only reported by param 0x20B) */
    size_t map_size;        /* 0: buffer not owned by us */
    uint32_t used;          /* write offset saved while not bound */
    uint32_t flushed;       /* end offset of the last closed 3D chunk */
    Request *req;
    uint32_t req_max, req_used, req_run;
    uint32_t run_bytes;     /* bytes of 3D chunks executed (param 0x209) */
    void (*callback)(int32_t);
    uint8_t irq_on_done;    /* 005FFBDC(-1) */
} CmdList;

typedef struct {
    uint32_t id;
    uint8_t *mem;
    size_t map_size;
    uint32_t format, width, height, area, bytes;
    bool written;
    uint64_t serial;        /* bumped by every transfer into this buffer */
    float rgba[4];          /* colour of the frame it holds (Tier A) */
} DisplayBuffer;

typedef struct {
    uintptr_t start, end;
    uint32_t value;
    uint8_t bits;
} FillRec;

#define FILL_RING 32
#define MAX_EXPLICIT 32

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static struct {
    bool initialized;
    void *alloc_fn, *free_fn;        /* the game's allocator pair from nngxInitialize */

    CmdList **lists; size_t nlists, caplists;
    uint32_t next_list_id;
    CmdList *bound;                  /* nngx +0x9C */
    uint8_t *cur, *end;              /* the global write pointer (0x00715480) and its end */
    CmdList *running;                /* nngx +0xA0 */
    bool run_enabled;                /* nngx +0x12 */

    struct { void *obj; CmdList *list; } explicit_lists[MAX_EXPLICIT];

    DisplayBuffer **dbufs; size_t ndbufs, capdbufs;
    uint32_t next_dbuf_id;
    int active_slot;                 /* 0 top, 1 bottom, 2 top right */
    DisplayBuffer *slot_buf[3];
    uint32_t display_mode;           /* 001060C0 (0x700 2D / 0x701 800px / 0x702 3D) */

    FillRec fills[FILL_RING]; unsigned fill_next;
    uint64_t serial;

    /* presentation */
    float shown[2][4];               /* colour each screen shows */
    const void *shown_mem[2];        /* display buffer memory each screen shows (GL image) */
    uint64_t shown_serial[2];
    bool dirty;                      /* a screen got a new frame since the last present */
    uint64_t last_present, last_pump;
    uint64_t presents, swaps, b2l_count, fill_count;
    bool warned_thread;
} G = {.next_list_id = 1, .next_dbuf_id = 1, .shown = {{0, 0, 0, 1}, {0, 0, 0, 1}}};

static uint8_t *g_vram;              /* VRAM_SIZE bytes */
static uint64_t g_t0;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;


static void init_once(void) {
    g_trace = getenv("ALBW_GFX_TRACE") != NULL;
    g_t0 = now_ns();
#if defined(__ANDROID__)
    /* Android: 0x1F000000 is inside ART's Java heap; VRAM goes to ALBW_ANDROID_VRAM (0x3E000000), where
     * the game's hard-coded VRAM addresses are remapped at build time (ALBW_VRAM_ADDR in albw_native.h,
     * VRAM pool words in tools/link_android.sh). Below 0x80000000: signed address arithmetic (stop A6) */
    void *want = (void *)(uintptr_t)ALBW_ANDROID_VRAM;
#else
    /* VRAM at its 3DS address if free (see header), else anywhere low */
    void *want = (void *)(uintptr_t)VRAM_3DS_BASE;
#endif
    void *p = mmap(want, VRAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p != MAP_FAILED && p != want) {
        munmap(p, VRAM_SIZE); /* old kernel treated NOREPLACE as a hint */
        p = MAP_FAILED;
    }
#if defined(__ANDROID__)
    if (p == MAP_FAILED) runtime_fatal("gfx: cannot map VRAM at %p (taken in this process?)", want);
#endif
    g_vram = p != MAP_FAILED ? p : low_map(VRAM_SIZE);
    gfx_log("VRAM %p..%p%s", (void *)g_vram, (void *)(g_vram + VRAM_SIZE - 1),
            g_vram == want ? (want == (void *)(uintptr_t)VRAM_3DS_BASE ? " (3DS address)" : " (fixed Android address)") : " (relocated: game VRAM range checks will miss)");
}
static void ensure_init(void) { pthread_once(&g_once, init_once); }

static void lock(void) { ensure_init(); pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

/* ================================================================== presentation (GL thread) */

static bool on_gl_thread(void) { return SDL_GL_GetCurrentContext() != NULL; }

static _Noreturn void quit_now(void) {
    gfx_log("window closed: exiting");
    fflush(NULL);
    plat_shutdown();
    _exit(0);
}

/* SDL event pump: every present, and every PUMP_NS while the game spins on the vblank counter.
 * GL thread only; never called with g_lock held across plat_* calls that sleep. */
static void pump(bool force) {
    if (!on_gl_thread()) return;
    uint64_t t = now_ns();
    if (!force && t - G.last_pump < PUMP_NS) return;
    G.last_pump = t;
    if (!plat_poll()) quit_now();
}

/* GL thread, lock NOT held (plat_end_frame sleeps for the 60 Hz pacing) */
static void present_now(float shown[2][4], const void *const shown_mem[2]) {
    for (int s = 0; s < 2; s++) {
        plat_begin_screen(s);
        if (render_scanout(s, shown_mem[s])) continue; /* the GL image of the display buffer */
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(shown[s][0], shown[s][1], shown[s][2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    plat_end_frame();
    render_frame_end();
    G.presents++;
    { /* debug: ALBW_FRAME_TRACE=1: per present, the work since the last one and the shown buffers */
        static int ft = -1;
        static RStats prev;
        if (ft < 0) ft = getenv("ALBW_FRAME_TRACE") != NULL;
        if (ft) {
            RStats rs = render_stats();
            gfx_log("frame %llu: draws %llu fills %llu xfers %llu | top %p bottom %p",
                    (unsigned long long)G.presents, (unsigned long long)(rs.draws - prev.draws),
                    (unsigned long long)(rs.fills - prev.fills), (unsigned long long)(rs.transfers - prev.transfers),
                    shown_mem[0], shown_mem[1]);
            prev = rs;
        }
    }
    G.last_present = now_ns();
    if (G.presents == 1)
        gfx_log("first frame presented (top %.2f %.2f %.2f, bottom %.2f %.2f %.2f)", shown[0][0], shown[0][1],
                shown[0][2], shown[1][0], shown[1][1], shown[1][2]);
    if (g_trace && G.presents % 60 == 0) {
        RStats rs = render_stats();
        gfx_log("render: ops run %llu recorded %llu pooled %llu fills %llu transfers %llu draws %llu tex %llu",
                (unsigned long long)rs.ops_run, (unsigned long long)rs.ops_recorded,
                (unsigned long long)rs.ops_pooled, (unsigned long long)rs.fills,
                (unsigned long long)rs.transfers, (unsigned long long)rs.draws,
                (unsigned long long)rs.tex_uploads);
    }
    if (g_trace && G.presents % 60 == 0)
        gfx_log("presents %llu swaps %llu B2L %llu fills %llu top %.2f %.2f %.2f bottom %.2f %.2f %.2f",
                (unsigned long long)G.presents, (unsigned long long)G.swaps,
                (unsigned long long)G.b2l_count, (unsigned long long)G.fill_count, shown[0][0], shown[0][1],
                shown[0][2], shown[1][0], shown[1][1], shown[1][2]);
    pump(true);
}

/* present if a screen got a new frame (or keep the window alive). Call WITHOUT g_lock held. */
static void present_maybe(void) {
    if (!on_gl_thread()) {
        lock();
        bool first = !G.warned_thread;
        G.warned_thread = true;
        unlock();
        if (first)
            gfx_log("SwapBuffers called from a thread without the GL context: frames are recorded "
                    "and presented by the GL thread (CheckVSync / albw_gfx_service)");
        return;
    }
    float shown[2][4];
    const void *shown_mem[2];
    lock();
    bool go = G.dirty || now_ns() - G.last_present >= KEEPALIVE_NS;
    if (go) {
        memcpy(shown, G.shown, sizeof shown);
        memcpy(shown_mem, G.shown_mem, sizeof shown_mem);
        G.dirty = false;
    }
    unlock();
    if (go) present_now(shown, shown_mem);
}

/* a screen now shows display buffer `b` (SwapBuffers / GSP framebuffer info) */
static void screen_show(int screen, const DisplayBuffer *b) {
    if (!b || !b->written) return;
    memcpy(G.shown[screen], b->rgba, sizeof b->rgba);
    G.shown_mem[screen] = b->mem;
    if (b->serial != G.shown_serial[screen]) {
        G.shown_serial[screen] = b->serial;
        G.dirty = true;
    }
}

/* ================================================================== display buffers */

static DisplayBuffer *dbuf_find(uint32_t id) {
    for (size_t i = 0; i < G.ndbufs; i++)
        if (G.dbufs[i]->id == id) return G.dbufs[i];
    return NULL;
}

static DisplayBuffer *dbuf_create(uint32_t id) {
    DisplayBuffer *b = pmem_calloc(1, sizeof *b);   /* fixed arena (persistent save states) */
    if (!b) runtime_fatal("gfx: out of memory");
    b->id = id;
    if (G.ndbufs == G.capdbufs) {
        G.capdbufs = G.capdbufs ? G.capdbufs * 2 : 16;
        G.dbufs = pmem_realloc(G.dbufs, G.capdbufs * sizeof *G.dbufs);
        if (!G.dbufs) runtime_fatal("gfx: out of memory");
    }
    G.dbufs[G.ndbufs++] = b;
    return b;
}

/* display buffer whose memory contains `addr` */
static DisplayBuffer *dbuf_at(uintptr_t addr) {
    for (size_t i = 0; i < G.ndbufs; i++) {
        DisplayBuffer *b = G.dbufs[i];
        if (b->mem && addr >= (uintptr_t)b->mem && addr < (uintptr_t)b->mem + b->bytes) return b;
    }
    return NULL;
}

static int fmt_to_pica(uint32_t gl_fmt) {
    switch (gl_fmt) {
    case FMT_RGBA8: return PICA_RGBA8;
    case FMT_RGB8: return PICA_RGB8;
    case FMT_RGB565: return PICA_RGB565;
    case FMT_RGB5_A1: return PICA_RGB5A1;
    case FMT_RGBA4: return PICA_RGBA4;
    default: return -1;
    }
}

static uint32_t fmt_bytes(uint32_t gl_fmt) {
    switch (gl_fmt) {
    case FMT_RGBA8: return 4;
    case FMT_RGB8: return 3;
    case FMT_RGB565: case FMT_RGB5_A1: case FMT_RGBA4: return 2;
    default: return 0;
    }
}

/* ================================================================== fills and transfers */

static void fill_record(uintptr_t start, uint32_t size, uint32_t value, uint8_t bits) {
    if (!start || !size) return;
    FillRec *f = &G.fills[G.fill_next++ % FILL_RING];
    f->start = start;
    f->end = start + size;
    f->value = value;
    f->bits = bits;
    G.fill_count++;
}

static const FillRec *fill_covering(uintptr_t addr) {
    for (unsigned i = 1; i <= FILL_RING; i++) {
        const FillRec *f = &G.fills[(G.fill_next - i) % FILL_RING];
        if (f->end && addr >= f->start && addr < f->end) return f;
    }
    return NULL;
}

/* MemoryFill value -> colour, given the format of the buffer it was written to.
 * Packings as built by the game's clear code (003CB588 / 003CB5F0). */
static void fill_decode(const FillRec *f, int pica_fmt, float out[4]) {
    uint32_t v = f->value;
    if (pica_fmt < 0) pica_fmt = f->bits == 32 ? PICA_RGBA8 : f->bits == 24 ? PICA_RGB8 : PICA_RGB565;
    float r, g, b;
    switch (pica_fmt) {
    case PICA_RGBA8: r = (v >> 24 & 0xFF) / 255.f; g = (v >> 16 & 0xFF) / 255.f; b = (v >> 8 & 0xFF) / 255.f; break;
    case PICA_RGB8: r = (v >> 16 & 0xFF) / 255.f; g = (v >> 8 & 0xFF) / 255.f; b = (v & 0xFF) / 255.f; break;
    case PICA_RGB565: r = (v >> 11 & 31) / 31.f; g = (v >> 5 & 63) / 63.f; b = (v & 31) / 31.f; break;
    case PICA_RGB5A1: r = (v >> 11 & 31) / 31.f; g = (v >> 6 & 31) / 31.f; b = (v >> 1 & 31) / 31.f; break;
    default: r = (v >> 12 & 15) / 15.f; g = (v >> 8 & 15) / 15.f; b = (v >> 4 & 15) / 15.f; break;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = 1.f;
}

/* a transfer from `src` (format `pica_fmt`, -1 = unknown) into display buffer `b` */
static void frame_into(DisplayBuffer *b, uintptr_t src, int pica_fmt) {
    const DisplayBuffer *sb = dbuf_at(src);
    if (sb && sb->written) {
        memcpy(b->rgba, sb->rgba, sizeof b->rgba);
    } else {
        const FillRec *f = fill_covering(src);
        if (f) {
            fill_decode(f, pica_fmt, b->rgba);
        } else {
            static const float black[4] = {0, 0, 0, 1};
            memcpy(b->rgba, black, sizeof black);
        }
    }
    b->written = true;
    b->serial = ++G.serial;
    G.b2l_count++;
}

/* is [p, p+n) inside memory this file owns (VRAM or a display buffer)? */
static bool owned_range(uintptr_t p, uint32_t n) {
    if (p >= (uintptr_t)g_vram && p + n <= (uintptr_t)g_vram + VRAM_SIZE) return true;
    const DisplayBuffer *b = dbuf_at(p);
    return b && p + n <= (uintptr_t)b->mem + b->bytes;
}

/* TextureCopy: copy `size` bytes; lines of sw bytes then sgap skipped (src), dw/dgap (dst) */
static void texture_copy(uintptr_t src, uintptr_t dst, uint32_t size, uint32_t sw, uint32_t sgap,
                         uint32_t dw, uint32_t dgap) {
    DisplayBuffer *db = dbuf_at(dst);
    if (db) frame_into(db, src, fmt_to_pica(db->format));
    if (!size || !src || !dst) return;
    /* real byte copy only between buffers we own: everything else is GPU-side data that does
     * not exist on the host (render targets live in GL) */
    uint64_t sspan = sw ? (uint64_t)size / sw * (sw + sgap) + sw : size;
    uint64_t dspan = dw ? (uint64_t)size / dw * (dw + dgap) + dw : size;
    if (sspan > 0xFFFFFFFFu || dspan > 0xFFFFFFFFu || !owned_range(src, (uint32_t)sspan) ||
        !owned_range(dst, (uint32_t)dspan))
        return;
    if (!sw || !dw) {
        memmove((void *)dst, (const void *)src, size);
        return;
    }
    uint32_t si = 0, di = 0, s = 0, d = 0;
    for (uint32_t n = 0; n < size; n++) {
        ((uint8_t *)dst)[d++] = ((const uint8_t *)src)[s++];
        if (++si == sw) { si = 0; s += sgap; }
        if (++di == dw) { di = 0; d += dgap; }
    }
}

static void exec_request(CmdList *l, const Request *r) {
    switch (r->type) {
    case RQ_DMA:
        if (r->u.dma.src && r->u.dma.dst && r->u.dma.size) {
            memmove(r->u.dma.dst, r->u.dma.src, r->u.dma.size);
            render_mem_written(r->u.dma.dst, r->u.dma.size);
        }
        break;
    case RQ_CMD3D:
        l->run_bytes += r->u.cmd.size;
        /* PICA words are never read: only the native markers of the replaced producers run */
        { static int ft = -1; if (ft < 0) ft = getenv("ALBW_FRAME_TRACE") != NULL;
          if (ft > 0) fprintf(stderr, "render: dump: CHUNK list %u req %u %#lx +%u\n", l->id, l->req_run,
                              (unsigned long)r->u.cmd.addr, (unsigned)r->u.cmd.size); }
        render_exec_cmds((const void *)r->u.cmd.addr, r->u.cmd.size);
        break;
    case RQ_FILL:
        for (int i = 0; i < 2; i++) {
            fill_record(r->u.fill.start[i], r->u.fill.size[i], r->u.fill.value[i], r->u.fill.bits[i]);
            if (r->u.fill.start[i] && r->u.fill.size[i])
                render_fill((const void *)r->u.fill.start[i], r->u.fill.size[i], r->u.fill.value[i], r->u.fill.bits[i]);
        }
        break;
    case RQ_B2L: {
        DisplayBuffer *b = dbuf_at(r->u.xfer.dst);
        { static int ft = -1; if (ft < 0) ft = getenv("ALBW_FRAME_TRACE") != NULL;
          if (ft) gfx_log("  B2L %#x -> %#x%s (%dx%d)", (unsigned)r->u.xfer.src, (unsigned)r->u.xfer.dst,
                          b ? " [display]" : "", r->u.xfer.dw, r->u.xfer.dh); }
        if (b) frame_into(b, r->u.xfer.src, r->u.xfer.sfmt);
        render_transfer((const void *)r->u.xfer.src, r->u.xfer.sw, r->u.xfer.sh, r->u.xfer.sfmt,
                        (void *)r->u.xfer.dst, r->u.xfer.dw, r->u.xfer.dh, r->u.xfer.dfmt, r->u.xfer.flip,
                        r->u.xfer.aa);
        break;
    }
    case RQ_L2B:
        break; /* linear -> tiled conversion of a capture: no host-side meaning in Tier A */
    case RQ_COPY:
        { static int ft = -1; if (ft < 0) ft = getenv("ALBW_FRAME_TRACE") != NULL;
          if (ft) gfx_log("  COPY %#lx -> %#lx size %#x (list %u req %u)", (unsigned long)r->u.copy.src,
                          (unsigned long)r->u.copy.dst, (unsigned)r->u.copy.size, l->id, l->req_run); }
        render_block_copy((const void *)r->u.copy.src, (void *)r->u.copy.dst, r->u.copy.size);
        texture_copy(r->u.copy.src, r->u.copy.dst, r->u.copy.size, r->u.copy.sw, r->u.copy.sgap,
                     r->u.copy.dw, r->u.copy.dgap);
        break;
    }
}

/* ================================================================== command lists */

typedef struct { void (*fn)(int32_t); int32_t arg; } Callback;

static void fire(Callback cb) {
    if (cb.fn) cb.fn(cb.arg);
}

/* run every pending request of the running list (nngx dispatcher + completion interrupt) */
static Callback kick(CmdList *l) {
    Callback cb = {0, 0};
    if (!l || l != G.running || !G.run_enabled) return cb;
    uint32_t done = 0;
    while (l->req_run < l->req_used) {
        Request *r = &l->req[l->req_run];
        exec_request(l, r);
        l->req_run++;
        done++;
        if (r->irq && l->callback) cb = (Callback){l->callback, (int32_t)l->req_run};
    }
    if (done && l->irq_on_done && l->callback) cb = (Callback){l->callback, (int32_t)l->req_run};
    return cb;
}

static CmdList *list_new(uint32_t id) {
    CmdList *l = pmem_calloc(1, sizeof *l);
    if (!l) runtime_fatal("gfx: out of memory");
    l->id = id;
    return l;
}

static void list_free_storage(CmdList *l) {
    if (l->map_size) pmem_free(l->buf);
    pmem_free(l->req);
    l->buf = NULL;
    l->req_rec = NULL;
    l->req = NULL;
    l->map_size = 0;
    l->buf_size = l->req_max = 0;
}

static void list_reset(CmdList *l) {
    if (l->req) memset(l->req, 0, l->req_max * sizeof *l->req);
    l->used = l->flushed = 0;
    l->req_used = l->req_run = 0;
    l->run_bytes = 0;
}

static CmdList *list_find(uint32_t id) {
    for (size_t i = 0; i < G.nlists; i++)
        if (G.lists[i]->id == id) return G.lists[i];
    return NULL;
}

static CmdList *list_create(uint32_t id) {
    CmdList *l = list_new(id);
    if (G.nlists == G.caplists) {
        G.caplists = G.caplists ? G.caplists * 2 : 16;
        G.lists = pmem_realloc(G.lists, G.caplists * sizeof *G.lists);
        if (!G.lists) runtime_fatal("gfx: out of memory");
    }
    G.lists[G.nlists++] = l;
    return l;
}

/* game-owned list object (005FF938 layout): its native shadow */
static CmdList *list_explicit(void *obj) {
    if (!obj) return NULL;
    for (int i = 0; i < MAX_EXPLICIT; i++)
        if (G.explicit_lists[i].obj == obj) return G.explicit_lists[i].list;
    for (int i = 0; i < MAX_EXPLICIT; i++)
        if (!G.explicit_lists[i].obj) {
            CmdList *l = list_new(0);
            l->game_obj = obj;
            G.explicit_lists[i].obj = obj;
            G.explicit_lists[i].list = l;
            return l;
        }
    runtime_fatal("gfx: more than %d game-owned command-list objects", MAX_EXPLICIT);
}

static uint32_t cur_offset(const CmdList *l) {
    return l == G.bound && G.cur ? (uint32_t)(G.cur - l->buf) : l->used;
}

/* new request slot on `l` (fill the args, then req_commit) */
static Request *req_add(CmdList *l, uint8_t type) {
    if (!l) { WARN_ONCE("request without a bound command list (dropped)"); return NULL; }
    if (!l->req || l->req_used >= l->req_max) {
        WARN_ONCE("command list %u: request array full (%u) - request dropped", l->id, l->req_max);
        return NULL;
    }
    Request *r = &l->req[l->req_used];
    uint8_t irq = r->irq;
    memset(&r->u, 0, sizeof r->u);
    r->type = type;
    r->irq = irq;
    return r;
}

static Callback req_commit(CmdList *l) {
    l->req_used++;
    return kick(l);
}

/* close the open 3D chunk of the bound list (nngxSplitDrawCmdlist family). The original appends
 * 0x18/0x20 bytes of PICA terminator words; nothing is written here since words are discarded. */
static Callback chunk_close(bool has_size, uint32_t size) {
    Callback none = {0, 0};
    CmdList *l = G.bound;
    if (!l || !l->buf || !G.cur) return none;
    uint32_t off = (uint32_t)(G.cur - l->buf);
    if (off == l->flushed) return none;
    Request *r = req_add(l, RQ_CMD3D);
    uint32_t start = l->flushed;
    l->flushed = off;
    if (!r) return none;
    r->u.cmd.addr = (uintptr_t)(l->buf + start);
    r->u.cmd.size = has_size ? size : off - start;
    return req_commit(l);
}

/* ================================================================== GSP client (L0) */

/* USA 0010AE3C  gsp::Gpu AcquireRight IPC stub (session*, flags, u8) -> Result. Callers: applet
 * code 001088D4. Native: no GPU right to acquire. Confidence: medium (name), high (no-op). */
uint32_t func_0010AE3C(void *session, uint32_t flags, uint32_t arg) { return 0; }
/* USA 0010AE80  gsp::Gpu ReleaseRight IPC stub (session*) -> Result (0010893C). Confidence: medium. */
uint32_t func_0010AE80(void *session) { return 0; }
/* USA 0010AEA8  gsp::Gpu RestoreVramSysArea IPC stub (session*) -> Result (00108A7C). Confidence: medium. */
uint32_t func_0010AEA8(void *session) { return 0; }
/* USA 0010AED0  gsp::Gpu UnregisterInterruptRelayQueue IPC stub (session*) -> Result (00108990).
 * Confidence: medium. */
uint32_t func_0010AED0(void *session) { return 0; }

/* USA 003E31E4 (+4 alias 003E31E8)  GSP client init: AcquireRight + RegisterInterruptRelayQueue,
 * shared-memory mapping; only caller nngxInitialize (replaced). Native: nothing to set up.
 * () -> void. Confidence: high. */
void func_003E31E4(void) { ensure_init(); }
void func_003E31E8(void) { ensure_init(); }

/* USA 003E32EC  GX command 0 RequestDma (dst, src, size, flush_flag, check) -> void. Evidence:
 * request word [2] = param_1 checked against VRAM 0x1F000000..0x1F5FFFFF, [1] = param_2; called
 * by 00601920 for nngx request type 0 (+8 dst, +4 src). Native: the copy itself. Confidence: high. */
void func_003E32EC(void *dst, const void *src, uint32_t size, uint32_t flush, uint32_t check) {
    lock();
    if (dst && src && size) {
        memmove(dst, src, size);
        render_mem_written(dst, size);
    }
    unlock();
}

/* USA 003E336C (+4 alias 003E3370)  wait for the next GSP interrupt (WaitSynchronization on the
 * relay event) () -> void; used by nngx Wait* loops. Native: the only periodic interrupt left is
 * the host vblank, so wait for the next one. Confidence: medium. */
static void wait_vblank(void);
void func_003E336C(void) { wait_vblank(); }
void func_003E3370(void) { wait_vblank(); }

/* USA 003E3380  GX command-queue header init (queue*, shm) -> void. Native: no queue. High. */
void func_003E3380(void *queue, void *shm) {}

/* USA 003E33E0  GX command-queue enqueue (queue*, const u32 cmd[8]) -> Result; TriggerCmdReqQueue
 * when the 15-entry ring was empty. Native: every builder below executes its command directly, so
 * nothing is queued. Confidence: high. */
uint32_t func_003E33E0(void *queue, const uint32_t *cmd) { return 0; }

/* USA 003E34D8  write the framebuffer info of `screen` (0 top, 1 bottom) into GSP shared memory
 * and set its update flag = the actual buffer swap on hardware. Args (screen, active, left addr,
 * right addr, stride, format, select) from the 0x1C-byte entry it fills; caller 00601934 <-
 * nngxSwapBuffers. Native: the screen shows the display buffer containing `left`. High. */
void func_003E34D8(uint32_t screen, uint32_t active, const void *left, const void *right, uint32_t stride,
                   uint32_t format, uint32_t select) {
    if (screen >= 2) return;
    lock();
    screen_show((int)screen, dbuf_at((uintptr_t)left));
    unlock();
}

/* USA 003E356C  GX command 2 MemoryFill (start0, end0, value0, ctl0, start1, end1, value1, ctl1);
 * ctl bit 8 = 24-bit, bit 9 = 32-bit pattern (dispatcher 0060226C builds it as
 * (req >> 15) & 0x300 | 1). Native: remember the fill. Confidence: high. */
static uint8_t ctl_bits(uint32_t ctl) { return ctl & 0x200 ? 32 : ctl & 0x100 ? 24 : 16; }
void func_003E356C(void *start0, void *end0, uint32_t value0, uint16_t ctl0, void *start1, void *end1,
                   uint32_t value1, uint16_t ctl1) {
    lock();
    if (start0 && end0 > start0) {
        fill_record((uintptr_t)start0, (uint32_t)((uintptr_t)end0 - (uintptr_t)start0), value0, ctl_bits(ctl0));
        render_fill(start0, (uint32_t)((uintptr_t)end0 - (uintptr_t)start0), value0, ctl_bits(ctl0));
    }
    if (start1 && end1 > start1) {
        fill_record((uintptr_t)start1, (uint32_t)((uintptr_t)end1 - (uintptr_t)start1), value1, ctl_bits(ctl1));
        render_fill(start1, (uint32_t)((uintptr_t)end1 - (uintptr_t)start1), value1, ctl_bits(ctl1));
    }
    unlock();
}

/* USA 003E35DC (+4 alias 003E35E0)  FlushDataCache(addr, size) wrapper (gsp IPC 0x00080082);
 * ~16 game callers before texture/vertex uploads. Native: one coherent address space, nothing to
 * flush (see flush_announce: an experiment to invalidate textures here, off by default).
 * -> Result 0. Confidence: high. */
static void flush_announce(const void *addr, uint32_t size) {
    /* OFF by default: the game flushes ranges that overlap textures every frame, so this forced
     * 6-36 texture re-uploads per frame (frame time up, BOOT_LOG 156) and never fixed anything
     * (the terrain bug was BOOT_LOG 155). ALBW_FLUSH_INVAL=1 turns it on (comparison). */
    static int on = -1;
    if (on < 0) on = getenv("ALBW_FLUSH_INVAL") != NULL;
    if (on) render_mem_written(addr, size);
}
uint32_t func_003E35DC(const void *addr, uint32_t size) { flush_announce(addr, size); return 0; }
uint32_t func_003E35E0(const void *addr, uint32_t size) { flush_announce(addr, size); return 0; }

/* USA 003E3610 (+4 alias 003E3614)  GX command 5 FlushCacheRegions + command 1
 * ProcessCommandList (addr, size, flush, flags) = submit PICA words. Native: discarded (Tier B
 * records native draws instead). Confidence: high. */
void func_003E3610(const void *addr, uint32_t size, uint32_t flush, uint32_t flags) {}
void func_003E3614(const void *addr, uint32_t size, uint32_t flush, uint32_t flags) {}

/* USA 003E36E0  GX command 4 TextureCopy (src, dst, size, src_width, src_gap, dst_width, dst_gap,
 * flags); widths/gaps in 16-byte units (packed width | gap << 16). Native: texture_copy (frame
 * into a display buffer / byte copy between host buffers). Confidence: high. */
void func_003E36E0(const void *src, void *dst, uint32_t size, uint32_t sw, uint32_t sgap, uint32_t dw,
                   uint32_t dgap, uint32_t flags) {
    lock();
    render_block_copy(src, dst, size);
    texture_copy((uintptr_t)src, (uintptr_t)dst, size, sw * 16, sgap * 16, dw * 16, dgap * 16);
    unlock();
}

/* USA 003E3744  SaveVramSysArea wrapper () -> Result (applet sleep/HOME: 003E489C, 003E4DDC,
 * 003E4FBC). Native: no system VRAM area. Confidence: high. */
uint32_t func_003E3744(void) { return 0; }

/* USA 003E376C  RegisterInterruptRelayQueue + interrupt thread setup (int *state) -> void;
 * USA 003E395C  the interrupt relay thread body (int *state);
 * USA 003E3AB4  wait on the interrupt event (int *state).
 * Native: there are no GPU interrupts (requests complete synchronously, vblank is a host timer).
 * Confidence: medium. */
void func_003E376C(int32_t *state) {}
void func_003E395C(int32_t *state) {}
void func_003E3AB4(int32_t *state) { wait_vblank(); }

/* USA 003E3AFC  GX command 3 DisplayTransfer (src, src_w, src_h, dst, dst_w, dst_h, flags);
 * flags bits 8-10 = input format, 12-14 = output format (dispatcher 0060226C). Native: if dst is
 * a display buffer, that buffer receives the frame. Confidence: high. */
void func_003E3AFC(const void *src, uint32_t sw, uint32_t sh, void *dst, uint32_t dw, uint32_t dh,
                   uint32_t flags) {
    lock();
    DisplayBuffer *b = dbuf_at((uintptr_t)dst);
    if (b) frame_into(b, (uintptr_t)src, (int)(flags >> 8 & 7));
    /* flags: bit 0 vertical flip, bits 8-10 input format, 12-14 output format, 24-25 scaling */
    render_transfer(src, (int)sw, (int)sh, (int)(flags >> 8 & 7), dst, (int)dw, (int)dh, (int)(flags >> 12 & 7),
                    (int)(flags & 1), (int)(flags >> 24 & 3));
    unlock();
}

/* USA 003E3B68 (+4 alias 003E3B6C)  WriteHWRegsWithMask wrapper (reg, data*, mask*, size) -> void.
 * USA 003E3D04  WriteHWRegs wrapper (reg, data*, size) -> Result.
 * USA 003E3D2C  IPC WriteHWRegs (session*, reg, data*, size) -> Result.
 * USA 003E3E00  IPC WriteHWRegsWithMask (session*, reg, data*, mask*, size) -> Result.
 * Native: there are no GPU/LCD registers (nngxInitialize used them for LCD setup). High. */
void func_003E3B68(uint32_t reg, const void *data, const void *mask, uint32_t size) {}
void func_003E3B6C(uint32_t reg, const void *data, const void *mask, uint32_t size) {}
uint32_t func_003E3D04(uint32_t reg, const void *data, uint32_t size) { return 0; }
uint32_t func_003E3D2C(void *session, uint32_t reg, const void *data, uint32_t size) { return 0; }
uint32_t func_003E3E00(void *session, uint32_t reg, const void *data, const void *mask, uint32_t size) {
    return 0;
}

/* USA 003E3B9C (+4 alias 003E3BA0)  "GSP first initialisation" flag (byte +0x75 of the GSP
 * state) () -> int; nngxInitialize programs the LCDs when set. Native: 0. Confidence: medium. */
int32_t func_003E3B9C(void) { return 0; }
int32_t func_003E3BA0(void) { return 0; }

/* capture info = 2 x {left addr, right addr, format, stride}; "no capture" = null + format -1 */
static void capture_info_none(int32_t *info) {
    if (!info) return;
    memset(info, 0, 8 * sizeof *info);
    info[2] = -1;
    info[6] = -1;
}

/* USA 003E3BB4  ImportDisplayCaptureInfo wrapper (int32 info[8], ...) -> void (003E4838, HOME menu
 * screenshot). Native: there is no HOME menu; report "no capture" exactly as the original does for
 * null addresses. Confidence: high. */
void func_003E3BB4(int32_t *info) { capture_info_none(info); }

/* USA 003E3C28 (+4 alias 003E3C2C)  set GSP interrupt callback slot (fn, index<7) -> previous.
 * Native: no interrupts; 0. Confidence: medium. */
uint32_t func_003E3C28(uint32_t fn, uint32_t index) { return 0; }
uint32_t func_003E3C2C(uint32_t fn, uint32_t index) { return 0; }

/* USA 003E3C74 (+4 alias 003E3C78)  GSP state field +0xC (interrupt thread id) () -> u32. 0. */
uint32_t func_003E3C74(void) { return 0; }
uint32_t func_003E3C78(void) { return 0; }

/* USA 003E3C88  ReadHWRegs wrapper (reg, out*, size) -> Result; USA 003E3CB0 IPC ReadHWRegs
 * (session*, reg, out*, size). Native: registers read as 0. Confidence: high. */
uint32_t func_003E3C88(uint32_t reg, void *out, uint32_t size) {
    if (out && size) memset(out, 0, size);
    return 0;
}
uint32_t func_003E3CB0(void *session, uint32_t reg, void *out, uint32_t size) {
    if (out && size) memset(out, 0, size);
    return 0;
}

/* USA 003E3D74  IPC FlushDataCache (session*, process, addr, size); USA 003E3DB0 IPC
 * SaveVramSysArea (session*); USA 003E3DD8 IPC TriggerCmdReqQueue (session*). -> Result 0. High. */
uint32_t func_003E3D74(void *session, uint32_t process, const void *addr, uint32_t size) {
    flush_announce(addr, size);   /* see 003E35DC */
    return 0;
}
uint32_t func_003E3DB0(void *session) { return 0; }
uint32_t func_003E3DD8(void *session) { return 0; }

/* USA 003E3E60  IPC ImportDisplayCaptureInfo (session*, int32 info[8]) -> Result. High. */
uint32_t func_003E3E60(void *session, int32_t *info) {
    capture_info_none(info);
    return 0;
}

/* USA 003E3EB0  IPC RegisterInterruptRelayQueue (session*, flags, event, u32 *thread_index,
 * u32 *shm_handle) -> Result. Native: no queue; outputs 0. Confidence: high. */
uint32_t func_003E3EB0(void *session, uint32_t flags, uint32_t event, uint32_t *thread_index,
                       uint32_t *shm_handle) {
    if (thread_index) *thread_index = 0;
    if (shm_handle) *shm_handle = 0;
    return 0;
}

/* USA 003E3F00 / 003E3F10 (+4 aliases 003E3F04 / 003E3F14)  enter / leave the GSP+nngx critical
 * section, paired around every nngx request append. Native: the same lock as this file. High. */
void func_003E3F00(void) { lock(); }
void func_003E3F04(void) { lock(); }
void func_003E3F10(void) { unlock(); }
void func_003E3F14(void) { unlock(); }

/* ================================================================== addresses / VRAM */

/* USA 001139F8 (and its twin 001139FC)  GetPhysicalAddr(va) -> pa: FCRAM 0x14000000.. +0x0C000000,
 * VRAM 0x1F000000.. -0x07000000, else 0. 27 callers, all turning buffer addresses into PICA
 * command words (which are discarded natively). Native: IDENTITY, so a host pointer is never
 * offset; a "physical" address is the host address and round-trips trivially. uintptr_t in/out so
 * it is exact for pointer-typed callers and for int-typed ones (low 32 bits). Confidence: high. */
uintptr_t func_001139F8(uintptr_t va) { return va; }
uintptr_t func_001139FC(uintptr_t va) { return va; }

/* USA 00600660  nngxGetVramStartAddr(area) -> addr: 0x20000 VRAM-A 0x1F000000 (0x1F180000 when
 * the system reserves part of it, flag 0x00715918+1), 0x30000 VRAM-B 0x1F300000 (0x1F380000),
 * else 0. Callers: VRAM manager 00499738 and texture loaders. Native: the host VRAM block; the
 * system-reserved variant does not exist natively. Confidence: high. */
void *func_00600660(uint32_t area) {
    ensure_init();
    if (area == AREA_VRAMA) return g_vram;
    if (area == AREA_VRAMB) return g_vram + VRAM_HALF;
    return NULL;
}

/* USA 0010BB14  GetVramEndAddr(area) -> last byte: 0x20000 -> 0x1F2FFFFF, 0x30000 -> 0x1F5FFFFF,
 * else 0 (EU literal pool). Paired with 00600660 by 00499738, so it must describe the same host
 * block. Confidence: high. */
void *func_0010BB14(uint32_t area) {
    ensure_init();
    if (area == AREA_VRAMA) return g_vram + VRAM_HALF - 1;
    if (area == AREA_VRAMB) return g_vram + VRAM_SIZE - 1;
    return NULL;
}

/* ================================================================== nngx: init, misc */

/* USA 006006B0  nngxInitialize(allocator, deallocator) -> 1 ok / 0 (null args or already done).
 * Sole caller sead 004966A4. Original: GSP init, GL-state backend (00591B0C), LCD register setup,
 * a temporary command list running the init words, then deletes it (nothing stays bound).
 * Native: remember the allocator pair (returned by 006003B4), start the vblank clock. High. */
int32_t func_006006B0(void *alloc_fn, void *free_fn) {
    if (!alloc_fn || !free_fn) return 0;
    lock();
    int32_t ok = 0;
    if (!G.initialized) {
        G.initialized = true;
        G.alloc_fn = alloc_fn;
        G.free_fn = free_fn;
        ok = 1;
        gfx_log("nngxInitialize (native)");
    }
    unlock();
    return ok;
}

/* USA 006003B4  nngxGetAllocator(void **alloc, void **free) (nngx data 0x00714820 {alloc, free});
 * used by 003F3668 / 003F9A00 / 003F9AA4 / 003FDDC0 to free VRAM through the game's allocator.
 * Confidence: high (body). */
void func_006003B4(void *ALBW_GP *alloc_fn, void *ALBW_GP *free_fn) { /* game words */
    lock();
    if (alloc_fn) *alloc_fn = G.alloc_fn;
    if (free_fn) *free_fn = G.free_fn;
    unlock();
}

/* USA 00600614  nngx initialised flag (state byte +0x10) () -> bool. Confidence: high. */
int32_t func_00600614(void) { return G.initialized; }

/* USA 00600624  "GPU busy" byte (+0x11) () -> bool; 00504ED0 spins on it after Stop. Native: the
 * GPU is never busy. Confidence: high. */
int32_t func_00600624(void) { return 0; }

/* USA 00600634  is command list object `obj` running and busy (obj) -> 0/1. Native: 0. High. */
int32_t func_00600634(void *obj) { return 0; }

/* USA 006005B0  request-array bytes for n requests (n * 0x1C); game code sizes the arrays it
 * gives 005FF938. Confidence: high. */
int32_t func_006005B0(int32_t n) { return n * REQ_RECORD_SIZE; }

/* USA 001060C0  nngxSetDisplayMode(0x700 2D / 0x701 800-px / 0x702 3D): writes nngx state +0x164
 * and the top width (+8). Native: recorded; both eyes show the left image. Confidence: medium. */
void func_001060C0(uint32_t mode) {
    if (mode < 0x700 || mode > 0x702) return;
    lock();
    G.display_mode = mode;
    unlock();
}

/* ================================================================== nngx: command lists */

/* USA 00600148  nngxGenCmdlists(n, u32 ids[n]): new unused ids, objects created. Callers sead
 * 00496204, game 002FE7B4. Confidence: medium (name), high (body). */
void func_00600148(int32_t n, uint32_t *ids) {
    lock();
    for (int32_t i = 0; i < n; i++) {
        while (G.next_list_id == 0 || list_find(G.next_list_id)) G.next_list_id++;
        list_create(G.next_list_id);
        ids[i] = G.next_list_id++;
    }
    unlock();
}

/* USA 005FF418  nngxBindCmdlist(id): saves the old list's used size, binds (creating the object
 * for an unknown id, like the original), restores the write pointer. Extra Ghidra args are
 * pass-through registers. ~18 callers (os.c passes the id as a pointer-sized value: the low 32
 * bits are the id). Confidence: high. */
void func_005FF418(uint32_t id) {
    lock();
    if (G.bound && G.cur) G.bound->used = (uint32_t)(G.cur - G.bound->buf);
    CmdList *l = NULL;
    if (id) {
        l = list_find(id);
        if (!l) l = list_create(id);
    }
    G.bound = l;
    if (l && l->buf) {
        G.cur = l->buf + l->used;
        G.end = l->buf + l->buf_size;
    } else {
        G.cur = G.end = NULL;
    }
    unlock();
}

/* USA 005FF850  nngxCmdlistStorage(bufsize, reqcount) on the bound list: (re)allocates the word
 * buffer and the request array, resets it. Callers 00496204, 002FE7B4. Confidence: high. */
void func_005FF850(uint32_t bufsize, uint32_t reqcount) {
    lock();
    CmdList *l = G.bound;
    if (l) {
        list_free_storage(l);
        size_t bufpart = page_round(bufsize + 0x100); /* + slack for a sloppy final word pair */
        l->map_size = bufpart + page_round((size_t)reqcount * REQ_RECORD_SIZE + 1);
        l->buf = pmem_calloc(1, l->map_size);   /* fixed arena: the game keeps the write pointer */
        if (!l->buf) runtime_fatal("gfx: out of memory");
        l->req_rec = l->buf + bufpart;
        l->buf_size = bufsize;
        l->req_max = reqcount;
        l->req = pmem_calloc(reqcount ? reqcount : 1, sizeof *l->req);
        if (!l->req) runtime_fatal("gfx: out of memory");
        list_reset(l);
        G.cur = l->buf;
        G.end = l->buf + bufsize;
    }
    unlock();
}

/* USA 005FF938  init a game-owned command-list object (obj[0x44], bufsize, buf, reqcount,
 * reqarray[reqcount*0x1C]); callers 00231EE4, 0050547C. Writes the 3DS object layout (id 0,
 * +4 buf, +8 size, +0x18 reqs, +0x1C reqcount, +0x2C 0x300) and creates the native shadow that
 * holds its requests. Confidence: high. */
void func_005FF938(void *obj, uint32_t bufsize, void *buf, uint32_t reqcount, void *reqarray) {
    if (!obj) return;
    lock();
    memset(obj, 0, 0x44);
    OBJ_U32(obj, 0x04) = addr32(buf);
    OBJ_U32(obj, 0x08) = bufsize;
    OBJ_U32(obj, 0x18) = addr32(reqarray);
    OBJ_U32(obj, 0x1C) = reqcount;
    OBJ_U32(obj, 0x2C) = 0x300;
    if (reqarray) memset(reqarray, 0, (size_t)reqcount * REQ_RECORD_SIZE);
    CmdList *l = list_explicit(obj);
    pmem_free(l->req);
    l->buf = buf;
    l->buf_size = bufsize;
    l->map_size = 0;
    l->req_rec = reqarray;
    l->req_max = reqcount;
    l->req = pmem_calloc(reqcount ? reqcount : 1, sizeof *l->req);
    if (!l->req) runtime_fatal("gfx: out of memory");
    list_reset(l);
    unlock();
}

/* USA 005FF9AC  nngxDeleteCmdlists(n, const u32 ids[n]); unbinds / stops a deleted list. Caller
 * 002FEAFC. Confidence: high. */
void func_005FF9AC(int32_t n, const uint32_t *ids) {
    lock();
    for (int32_t i = 0; i < n; i++) {
        if (!ids[i]) continue;
        for (size_t k = 0; k < G.nlists; k++) {
            CmdList *l = G.lists[k];
            if (l->id != ids[i]) continue;
            if (G.running == l) { G.running = NULL; G.run_enabled = false; }
            if (G.bound == l) { G.bound = NULL; G.cur = G.end = NULL; }
            if (ids[i] < G.next_list_id) G.next_list_id = ids[i];
            list_free_storage(l);
            pmem_free(l);
            G.lists[k] = G.lists[--G.nlists];
            break;
        }
    }
    unlock();
}

/* USA 005FF760  nngxClearCmdlist() on the bound list: used size, request counts, chunk state = 0;
 * write pointer back to the buffer start. 9 callers (frame end 0032259C / 004961C4 ...). High. */
void func_005FF760(void) {
    lock();
    CmdList *l = G.bound;
    if (l && (l->buf || l->req)) {
        list_reset(l);
        G.cur = l->buf;
        G.end = l->buf ? l->buf + l->buf_size : NULL;
    }
    unlock();
}

/* USA 005FF7F0  clear a game-owned list object (obj). Confidence: high. */
void func_005FF7F0(void *obj) {
    if (!obj) return;
    lock();
    static const int counters[] = {0x0C, 0x10, 0x14, 0x20, 0x24, 0x28, 0x30, 0x34};
    for (size_t i = 0; i < sizeof counters / sizeof *counters; i++) OBJ_U32(obj, counters[i]) = 0;
    list_reset(list_explicit(obj));
    unlock();
}

/* USA 006003D8  nngxGetCmdlistParameteri(pname, out) on the bound list. pnames from the switch:
 * 0x200 run mode (+0x2C, 0x300), 0x201 is running (running list && busy), 0x202 used bufsize,
 * 0x203 used reqcount, 0x204 max bufsize, 0x205 max reqcount, 0x206 top bufaddr, 0x207 bound id,
 * 0x208 CURRENT_BUFADDR (46 game sites), 0x209 executed 3D bytes, 0x20A executed reqcount,
 * 0x20B top request address, 0x20C/0x20D next pending request (none: requests run synchronously,
 * the original writes nothing then), 0x20E a GPU status register (0). Outputs are 32-bit (the
 * callers' slots are ints; addresses are below 2 GiB, see header). Confidence: high. */
void func_006003D8(uint32_t pname, uint32_t *out) {
    if (!out) return;
    lock();
    CmdList *l = G.bound;
    bool have = true;
    uint32_t v = 0;
    switch (pname) {
    case 0x200: v = 0x300; break;
    case 0x201: v = 0; break; /* never busy */
    case 0x202: v = l ? cur_offset(l) : 0; if (l) l->used = v; break;
    case 0x203: v = l ? l->req_used : 0; break;
    case 0x204: v = l ? l->buf_size : 0; break;
    case 0x205: v = l ? l->req_max : 0; break;
    case 0x206: v = l ? addr32(l->buf) : 0; break;
    case 0x207: v = l ? l->id : 0; break;
    case 0x208: v = addr32(G.cur); break;
    case 0x209: v = l ? l->run_bytes : 0; break;
    case 0x20A: v = l ? l->req_run : 0; break;
    case 0x20B: v = l ? addr32(l->req_rec) : 0; break;
    case 0x20E: v = 0; break;
    default: have = false; break; /* 0x20C / 0x20D: nothing pending */
    }
    if (have) *out = v;
    unlock();
}

/* USA 00601378  nngxMoveCommandbufferPointer(bytes): write pointer += bytes (always after the
 * caller wrote raw PICA words at CURRENT_BUFADDR). 26 callers. Confidence: high. */
void func_00601378(int32_t bytes) {
    lock();
    if (G.cur) {
        G.cur += bytes;
        if (G.cur > G.end) WARN_ONCE("command list %u overflowed its %u-byte buffer", G.bound ? G.bound->id : 0,
                                      G.bound ? G.bound->buf_size : 0);
    }
    unlock();
}

/* USA 0041F7CC / 0041F7DC  engine helpers reading / writing the nngx write-pointer global
 * (0x00715480) directly: get () -> u32, set (u32). 17 callers hand it to virtual Draw methods that
 * return the advanced pointer. 32-bit values like the callers' ints. Confidence: high (body). */
uint32_t func_0041F7CC(void) {
    lock();
    uint32_t v = addr32(G.cur);
    unlock();
    return v;
}
void func_0041F7DC(uint32_t p) {
    lock();
    G.cur = (uint8_t *)(uintptr_t)p;
    unlock();
}

/* USA 005FFDC0  nngxSplitDrawCmdlist(): close the open 3D chunk as a request. 15 callers.
 * USA 005FFC18  same without the chunk interrupt flag (Flush3DCommand variant). 5 callers.
 * USA 00601468  internal terminator/flush, also called by 002C30E8 / 003CB5F0.
 * USA 005FFF7C  same with an explicit size for the request (size). 8 callers.
 * Native: request bookkeeping; no terminator words written. Confidence: medium (names). */
void func_005FFDC0(void) { lock(); Callback cb = chunk_close(false, 0); unlock(); fire(cb); }
void func_005FFC18(void) { lock(); Callback cb = chunk_close(false, 0); unlock(); fire(cb); }
void func_00601468(void) { lock(); Callback cb = chunk_close(false, 0); unlock(); fire(cb); }
void func_005FFF7C(uint32_t size) { lock(); Callback cb = chunk_close(true, size); unlock(); fire(cb); }

/* USA 005FEF5C  append an external 3D command buffer (addr, size) as a request (closing the open
 * chunk first). Callers 001321B8, 002FE918 (sub-lists). Confidence: low (name), high (body). */
void func_005FEF5C(uint32_t addr, uint32_t size) {
    lock();
    Callback cb = chunk_close(false, 0);
    Request *r = G.bound && G.cur ? req_add(G.bound, RQ_CMD3D) : NULL;
    if (r) {
        r->u.cmd.addr = addr;
        r->u.cmd.size = size;
        cb = req_commit(G.bound);
    }
    unlock();
    fire(cb);
}

/* USA 005FF104  write a CMDBUF subroutine call to (addr, size) into the bound list and close the
 * chunk. Callers 001321B8, 002FE918 (the double-buffered scene sub-lists: 002FE9CC records a pass
 * into its own list, 002FEA3C ends it, the next frame's 002FE918 calls the finished buffer from the
 * main list; the title's top-screen layouts Mn_TitleT / Mn_TitleLogoT and the other layer-2 2D
 * objects of 0012E048 go this way). Native: ROP_CALL(addr, size) at the write pointer, so the
 * sub-list's markers run in place like the hardware jumping there and returning, followed by a
 * zero return-segment slot (as 003CC230 writes: render_exec_cmds stops there), then the chunk is
 * closed. Before, the target was discarded and those passes never ran (boot: top-screen 2D
 * missing; "(c) 2013 Nintendo", title logo). Confidence: medium (name), high (body). */
void func_005FF104(uint32_t addr, uint32_t size) {
    lock();
    if (G.bound && G.cur && addr && size >= 8 && G.cur + 16 <= G.end) {
        ROp op;
        memset(&op, 0, sizeof op);
        op.type = ROP_CALL;
        op.u.call.addr = (uintptr_t)addr;
        op.u.call.size = size;
        uint32_t *q = render_rec((uint32_t *)(void *)G.cur, &op);
        q[0] = 0;
        q[1] = 0;
        G.cur = (uint8_t *)(q + 2);
    }
    Callback cb = chunk_close(false, 0);
    unlock();
    fire(cb);
}

/* USA 00601450  set the bound list's completion callback (+0x38) (fn). Caller 00322CF4 with
 * 0049AD24 (-> framework vtbl+0x8C, stores the GPU-done tick). Confidence: medium. */
void func_00601450(void (*fn)(int32_t)) {
    lock();
    if (G.bound) G.bound->callback = fn;
    unlock();
}

/* USA 005FFBDC  enable the callback on request n (1-based) or, with -1, on list completion
 * (+0x3C). Callers 00132784, 00322CF4. Confidence: low (name), high (body). */
void func_005FFBDC(int32_t n) {
    lock();
    CmdList *l = G.bound;
    if (l) {
        if (n == -1) l->irq_on_done = 1;
        else if (n >= 1 && (uint32_t)n <= l->req_max) l->req[n - 1].irq = 1;
    }
    unlock();
}

/* USA 00601390  nngxRunCmdlist(): the bound list becomes the running one and its pending
 * requests execute (synchronously here). Callers: sead frame 004960AC / 0049A944, game
 * 00322460 / 0032274C / 00322944 ... Confidence: high. */
void func_00601390(void) {
    lock();
    Callback cb = {0, 0};
    if (G.bound) {
        G.running = G.bound;
        G.run_enabled = true;
        cb = kick(G.bound);
    }
    unlock();
    fire(cb);
}

/* USA 006013F0  run a game-owned list object (obj). Callers 00231C00, 00504ED0. High. */
void func_006013F0(void *obj) {
    if (!obj) return;
    lock();
    CmdList *l = list_explicit(obj);
    G.running = l;
    G.run_enabled = true;
    Callback cb = kick(l);
    unlock();
    fire(cb);
}

/* USA 00601600  nngxStopCmdlist(): requests appended later no longer start. Confidence: high. */
void func_00601600(void) {
    lock();
    if (G.running) G.run_enabled = false;
    unlock();
}

/* USA 006017F0  nngxWaitCmdlistDone(): loops on the GSP event until the running list is idle
 * (with a timeout callback). Native: requests complete synchronously -> returns at once. High. */
void func_006017F0(void) {}

/* ---- requests on the bound list */

/* USA 005FF314  nngxAddVramDmaCommand(src, dst, size) (request type 0). Callers 0023387C,
 * 00233F4C, 003C1C74, 003F3668, 003F9AA4. Confidence: high. */
void func_005FF314(const void *src, void *dst, uint32_t size) {
    lock();
    Callback cb = {0, 0};
    Request *r = req_add(G.bound, RQ_DMA);
    if (r) {
        r->u.dma.src = src;
        r->u.dma.dst = dst;
        r->u.dma.size = size;
        cb = req_commit(G.bound);
    }
    unlock();
    fire(cb);
}

/* USA 005FF398  same on a game-owned list object (obj, src, dst, size). Callers 003F3668,
 * 003F9AA4, 00504ED0. Confidence: high. */
void func_005FF398(void *obj, const void *src, void *dst, uint32_t size) {
    lock();
    Callback cb = {0, 0};
    CmdList *l = list_explicit(obj);
    Request *r = req_add(l, RQ_DMA);
    if (r) {
        r->u.dma.src = src;
        r->u.dma.dst = dst;
        r->u.dma.size = size;
        cb = req_commit(l);
    }
    unlock();
    fire(cb);
}

static Callback add_fill(CmdList *l, void *b0, uint32_t s0, uint32_t v0, uint32_t w0, void *b1, uint32_t s1,
                         uint32_t v1, uint32_t w1) {
    Callback none = {0, 0};
    Request *r = req_add(l, RQ_FILL);
    if (!r) return none;
    r->u.fill.start[0] = (uintptr_t)b0;
    r->u.fill.size[0] = b0 ? s0 & 0x7FFFFF : 0;
    r->u.fill.value[0] = v0;
    r->u.fill.bits[0] = (uint8_t)w0;
    r->u.fill.start[1] = (uintptr_t)b1;
    r->u.fill.size[1] = b1 ? s1 & 0x7FFFFF : 0;
    r->u.fill.value[1] = v1;
    r->u.fill.bits[1] = (uint8_t)w1;
    return req_commit(l);
}

/* USA 005FF2C8  nngxAddMemoryFillCommand(buf0, size0, value0, width0, buf1, size1, value1,
 * width1); widths 0x10/0x18/0x20 bits; buf0 = colour, buf1 = depth (callers 003CB588 / 003CB5F0
 * pack the colour as R<<24|G<<16|B<<8|A, 565, 5551 or 4444). Request type 2. Confidence: high. */
void func_005FF2C8(void *b0, uint32_t s0, uint32_t v0, uint32_t w0, void *b1, uint32_t s1, uint32_t v1,
                   uint32_t w1) {
    lock();
    Callback cb = add_fill(G.bound, b0, s0, v0, w0, b1, s1, v1, w1);
    unlock();
    fire(cb);
}

/* USA 00601F38  the same on an explicit list (list, ...); reached only from 005FF2C8 in the
 * original, kept for any other caller with a game-owned object. Confidence: high. */
void func_00601F38(void *obj, void *b0, uint32_t s0, uint32_t v0, uint32_t w0, void *b1, uint32_t s1,
                   uint32_t v1, uint32_t w1) {
    lock();
    Callback cb = add_fill(list_explicit(obj), b0, s0, v0, w0, b1, s1, v1, w1);
    unlock();
    fire(cb);
}

static Callback add_b2l(CmdList *l, const void *src, uint32_t sw, uint32_t sh, uint32_t sfmt, void *dst,
                        uint32_t dw, uint32_t dh, uint32_t dfmt, uint32_t aa, uint32_t flip) {
    Callback none = {0, 0};
    int in = fmt_to_pica(sfmt), out = fmt_to_pica(dfmt);
    if (aa < 0x500 || aa > 0x502 || in < 0 || out < 0 || !sw || !sh || !dw || !dh) return none;
    Request *r = req_add(l, RQ_B2L);
    if (!r) return none;
    r->u.xfer.src = (uintptr_t)src;
    r->u.xfer.dst = (uintptr_t)dst;
    r->u.xfer.sw = (uint16_t)sw;
    r->u.xfer.sh = (uint16_t)sh;
    r->u.xfer.dw = (uint16_t)dw;
    r->u.xfer.dh = (uint16_t)dh;
    r->u.xfer.sfmt = (uint8_t)in;
    r->u.xfer.dfmt = (uint8_t)out;
    r->u.xfer.flip = flip != 0;
    r->u.xfer.aa = (uint8_t)(aa - 0x500); /* 0 none, 1 2x horizontal, 2 2x2 */
    return req_commit(l);
}

/* USA 005FF018  nngxAddB2LTransferCommand(src, src_w, src_h, src_fmt, dst, dst_w, dst_h, dst_fmt,
 * aa_mode 0x500/0x501/0x502, flip, block 8/32) - render target (tiled) -> display buffer (linear),
 * request type 3. Evidence: 00601AF0 body + caller 004964F8 (dst = GetDisplaybufferParameteri
 * 0x600/0x602/0x603/0x601). Callers 0014B788, 0014C108, 0014C804, 004964F8. Confidence: high. */
void func_005FF018(const void *src, uint32_t sw, uint32_t sh, uint32_t sfmt, void *dst, uint32_t dw,
                   uint32_t dh, uint32_t dfmt, uint32_t aa, uint32_t flip, uint32_t block) {
    lock();
    Callback cb = add_b2l(G.bound, src, sw, sh, sfmt, dst, dw, dh, dfmt, aa, flip);
    unlock();
    fire(cb);
}

/* USA 00601AF0  the same on an explicit list (list, ...). Confidence: high. */
void func_00601AF0(void *obj, const void *src, uint32_t sw, uint32_t sh, uint32_t sfmt, void *dst,
                   uint32_t dw, uint32_t dh, uint32_t dfmt, uint32_t aa, uint32_t flip, uint32_t block) {
    lock();
    Callback cb = add_b2l(list_explicit(obj), src, sw, sh, sfmt, dst, dw, dh, dfmt, aa, flip);
    unlock();
    fire(cb);
}

/* USA 005FF2A8 / 00601D98  linear -> block DisplayTransfer on a game-owned list (obj, src, dst,
 * width, height, format, block 8/32); caller 00231C00 (capture conversion). Native: recorded and
 * executed as a no-op (no host-side tiled data). Confidence: medium (name), high (body). */
static void add_l2b(void *obj, const void *src, void *dst, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!w || !h || fmt_to_pica(fmt) < 0) return;
    lock();
    Callback cb = {0, 0};
    CmdList *l = list_explicit(obj);
    Request *r = req_add(l, RQ_L2B);
    if (r) {
        r->u.xfer.src = (uintptr_t)src;
        r->u.xfer.dst = (uintptr_t)dst;
        r->u.xfer.sw = r->u.xfer.dw = (uint16_t)w;
        r->u.xfer.sh = r->u.xfer.dh = (uint16_t)h;
        cb = req_commit(l);
    }
    unlock();
    fire(cb);
}
void func_005FF2A8(void *obj, const void *src, void *dst, uint32_t w, uint32_t h, uint32_t fmt, uint32_t block) {
    add_l2b(obj, src, dst, w, h, fmt);
}
void func_00601D98(void *obj, const void *src, void *dst, uint32_t w, uint32_t h, uint32_t fmt, uint32_t block) {
    add_l2b(obj, src, dst, w, h, fmt);
}

/* USA 005FF064  nngxAddBlockImageCopyCommand(src, src_width, src_gap, dst, dst_width, dst_gap,
 * size) - bytes (stored >> 4 as GX TextureCopy units), request type 4. Callers 0014B788,
 * 0014C108, 0014C804, 003C1C74. Confidence: medium (name), high (body). */
void func_005FF064(const void *src, uint32_t sw, uint32_t sgap, void *dst, uint32_t dw, uint32_t dgap,
                   uint32_t size) {
    lock();
    Callback cb = {0, 0};
    Request *r = req_add(G.bound, RQ_COPY);
    if (r) {
        r->u.copy.src = (uintptr_t)src;
        r->u.copy.dst = (uintptr_t)dst;
        r->u.copy.size = size;
        r->u.copy.sw = sw & ~15u;
        r->u.copy.sgap = sgap & ~15u;
        r->u.copy.dw = dw & ~15u;
        r->u.copy.dgap = dgap & ~15u;
        cb = req_commit(G.bound);
    }
    unlock();
    fire(cb);
}

/* ================================================================== nngx: display buffers */

/* USA 0060027C  nngxGenDisplaybuffers(n, u32 ids[n]). Caller 00322CF4. Confidence: high. */
void func_0060027C(int32_t n, uint32_t *ids) {
    lock();
    for (int32_t i = 0; i < n; i++) {
        while (G.next_dbuf_id == 0 || dbuf_find(G.next_dbuf_id)) G.next_dbuf_id++;
        dbuf_create(G.next_dbuf_id);
        ids[i] = G.next_dbuf_id++;
    }
    unlock();
}

/* USA 005FEF2C  nngxActiveDisplay(display): 0x400 -> slot 0, 0x401 -> 1, 0x410 -> 2 (+0x124);
 * other values ignored. Extra Ghidra args are pass-through registers. Confidence: high. */
void func_005FEF2C(uint32_t display) {
    lock();
    if (display == NN_GX_DISPLAY0) G.active_slot = 0;
    else if (display == NN_GX_DISPLAY1) G.active_slot = 1;
    else if (display == NN_GX_DISPLAY0_EXT) G.active_slot = 2;
    unlock();
}

/* USA 005FF5DC  nngxBindDisplaybuffer(id) for the active display (creates unknown ids, 0 =
 * unbind). Confidence: high. */
void func_005FF5DC(uint32_t id) {
    lock();
    DisplayBuffer *b = NULL;
    if (id) {
        b = dbuf_find(id);
        if (!b) b = dbuf_create(id);
    }
    G.slot_buf[G.active_slot] = b;
    unlock();
}

/* USA 005FFAF0  nngxDisplaybufferStorage(format, width, height, area 0x10000/0x20000/0x30000)
 * for the bound display buffer of the active display. On hardware the memory comes from the
 * game allocator in VRAM; natively it is host memory below 2 GiB (it only identifies the target of
 * transfers and is never scanned out). Caller 00322CF4. Confidence: high. */
void func_005FFAF0(uint32_t format, uint32_t width, uint32_t height, uint32_t area) {
    uint32_t bpp = fmt_bytes(format);
    if (!bpp || (area != AREA_FCRAM && area != AREA_VRAMA && area != AREA_VRAMB)) return;
    lock();
    DisplayBuffer *b = G.slot_buf[G.active_slot];
    if (b) {
        if (b->map_size) {
            render_display_unregister(b->mem);
            pmem_free(b->mem);
        }
        b->bytes = bpp * width * height;
        b->map_size = page_round(b->bytes ? b->bytes : 1);
        b->mem = pmem_calloc(1, b->map_size);
        if (!b->mem) runtime_fatal("gfx: out of memory");
        render_display_register(b->mem, b->bytes, (int)width, (int)height, fmt_to_pica(format));
        b->format = format;
        b->width = width;
        b->height = height;
        b->area = area;
        b->written = false;
    }
    unlock();
}

/* USA 006005BC  nngxGetDisplaybufferParameteri(pname, out) for the active display's buffer:
 * 0x600 address, 0x601 format, 0x602 width, 0x603 height (as used by 004964F8). Outputs 32-bit.
 * Confidence: high. */
void func_006005BC(uint32_t pname, uint32_t *out) {
    if (!out) return;
    lock();
    DisplayBuffer *b = G.slot_buf[G.active_slot];
    if (b) {
        switch (pname) {
        case 0x600: *out = addr32(b->mem); break;
        case 0x601: *out = b->format; break;
        case 0x602: *out = b->width; break;
        case 0x603: *out = b->height; break;
        default: break;
        }
    }
    unlock();
}

/* USA 005FF580  sead display-buffer method (through a vtable; its code falls through into
 * 005FF5DC and shares its literal pool): ActiveDisplay(0x400); 004964F8(this[0xC4 + 4 *
 * this[0x124]], this + 0xD4, 0, 0, (u64)0x500); ActiveDisplay(0x400); BindDisplaybuffer(same id).
 * Replaced because the decompiled body reads nngx's private state. EU disassembly 005FF558.
 * Confidence: high. */
void func_004964F8(uint32_t dbuf_id, void *render_target, int32_t x, int32_t y, uint64_t aa_flip);
void func_005FF580(void *self) {
    func_005FEF2C(NN_GX_DISPLAY0);
    func_004964F8(OBJ_U32(self, 0xC4 + 4 * OBJ_U32(self, 0x124)), (uint8_t *)self + 0xD4, 0, 0, 0x500);
    func_005FEF2C(NN_GX_DISPLAY0);
    func_005FF5DC(OBJ_U32(self, 0xC4 + 4 * OBJ_U32(self, 0x124)));
}

/* USA 00601658  nngxSwapBuffers(display 0x400 / 0x401 / 0x402 both): for each LCD, the bound
 * display buffer becomes the scanned-out one (003E34D8). Callers: game present 0032259C /
 * 00322BF0, sead pacing 00496238 / 0049ABAC (os.c, several times per frame). Native: each screen
 * shows its bound buffer's frame. As on the 3DS, the swap takes effect at the next vblank and the
 * call does not block: the host present (which swaps and paces to 60 Hz, so it sleeps) is done by
 * the next CheckVSync / WaitVSync on the GL thread. The game's frame (00322460) swaps in
 * vtbl+0x78 while it holds the display lock (*data_00715160) and waits for the vblank in vtbl+0x7C
 * after releasing it; presenting here slept inside the lock, and the prepare thread waiting for
 * that lock never got it (the main thread re-took it first every frame): the title task never
 * loaded, about half the boots (boot stop 85, the stop-73 lock). Confidence: high. */
void func_00601658(uint32_t display) {
    lock();
    G.swaps++;
    for (int s = 0; s < 2; s++)
        if (display == NN_GX_DISPLAY0 + (uint32_t)s || display == NN_GX_DISPLAY_BOTH) screen_show(s, G.slot_buf[s]);
    unlock();
}

/* ================================================================== nngx: vsync */

static uint32_t vcount(void) {
    ensure_init();
    return (uint32_t)(((now_ns() - g_t0) / VBLANK_NS) % VCOUNT_WRAP);
}

static void wait_vblank(void) {
    ensure_init();
    if (on_gl_thread()) present_maybe(); /* a swapped frame is shown at this vblank (SwapBuffers) */
    uint64_t t = now_ns() - g_t0;
    uint64_t next = (t / VBLANK_NS + 1) * VBLANK_NS;
    for (;;) {
        pump(false);
        t = now_ns() - g_t0;
        if (t >= next) return;
        uint64_t left = next - t;
        sleep_ns(left < PUMP_NS ? left : PUMP_NS);
    }
}

/* USA 005FF6E4  nngxCheckVSync(display) -> vblank counter of 0x400 / 0x401 (0x402: the older of
 * the two, with the 0x7FFFFFFE wrap rule); other ids 0. Callers: sead loops (os.c 0049AD58,
 * 004966D4, 00496238, 0049ABAC) and 00322880. Native: one host 60 Hz counter for both LCDs. Also
 * pumps SDL (rate-limited) and, on the GL thread, presents the frame swapped since the last vblank
 * (SwapBuffers does not present itself), since the frame loop spins here. Confidence: high. */
int32_t func_005FF6E4(uint32_t display) {
    if (display < NN_GX_DISPLAY0 || display > NN_GX_DISPLAY_BOTH) return 0;
    if (on_gl_thread()) present_maybe(); /* the frame swapped since the last vblank (keep-alive too) */
    pump(false);
    return (int32_t)vcount();
}

/* USA 0060188C  nngxWaitVSync(display): block until that LCD's vblank counter changes (GSP event
 * wait loop). Caller 0048AE64. Native: sleep to the next host vblank, pumping SDL. Confidence:
 * high (body). */
void func_0060188C(uint32_t display) {
    if (display < NN_GX_DISPLAY0 || display > NN_GX_DISPLAY_BOTH) return;
    wait_vblank();
}

/* ================================================================== native API (typed) */

/* For the GL-owning thread if the game ever presents from another thread (see header): presents
 * a recorded frame and pumps SDL. Safe to call any time on the GL thread. */
void albw_gfx_service(void) {
    ensure_init();
    pump(false);
    present_maybe();
}

/* typed views of the bound command buffer, for retyped (64-bit clean) callers */
void *albw_gfx_cmdbuf_current(void) { return G.cur; }
void albw_gfx_cmdbuf_set(void *p) { lock(); G.cur = p; unlock(); }

/* ================================================================== save states (savestate.c)
 * The command lists, their buffers and requests and the display buffers are in the fixed arena
 * (runtime/pmem.c), saved as a whole by savestate.c; here only the state struct G (which points
 * into the arena). After a load the renderer learns the display buffers again. */
void albw_gfx_vram(uint8_t **base, size_t *size) { ensure_init(); *base = g_vram; *size = VRAM_SIZE; }

void albw_gfx_ss_write(SsBuf *b) {
    lock();
    ss_section(b, 0x31584647u, &G, sizeof G);   /* "GFX1" */
    unlock();
}

int albw_gfx_ss_check(SsBuf *b, char *why, size_t whylen) {
    size_t n;
    if (!ss_get_section(b, 0x31584647u, &n) || n != sizeof G) { snprintf(why, whylen, "damaged state (graphics)"); return -1; }
    return 0;
}

void albw_gfx_ss_load(SsBuf *b) {
    size_t n;
    const void *g = ss_get_section(b, 0x31584647u, &n);
    lock();
    memcpy(&G, g, sizeof G);
    G.dirty = true;
    for (size_t i = 0; i < G.ndbufs; i++) {
        DisplayBuffer *d = G.dbufs[i];
        if (d->mem) render_display_register(d->mem, d->bytes, (int)d->width, (int)d->height, fmt_to_pica(d->format));
    }
    unlock();
}
