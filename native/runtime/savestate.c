/* Save states of the native port (design: native/SAVESTATES.md).
 *
 * A state is the whole game at the end of a frame, written to <state dir>/slot<N>.state and loadable
 * by any later run of the same binary:
 *  - game memory: the heap and device-memory regions, VRAM, the game's data / bss and the static
 *    data of every native module that holds game state (<binary>.ssmap from the link map, plus
 *    ranges modules register: the sound voices);
 *  - the fixed arena (runtime/pmem.c): kernel objects, scheduler records, command lists and
 *    buffers, display buffers, model vertex layouts and blob ops, decoder tables;
 *  - by value: the kernel tables and every game thread's registers, stack and thread-locals
 *    (sdk/os.c), the nngx state (sdk/gfx.c), the file tables (sdk/fs.c), the renderer's op pool,
 *    model-blob registry, lighting LUT / material registries and light side table;
 *  - the main thread's registers and stack, taken here at the frame end.
 * Everything a state points into is at a fixed address in every run (non-PIE binary, VRAM at its
 * 3DS address, pmem areas), so loading is: check everything, write it all back, let every thread
 * jump back into its saved frame. The renderer re-reads textures; host state (window, GL objects,
 * audio device, input) is never part of a state.
 *
 * Requests (F6 / F7 / F8, Esc menu File, ALBW_SS_AT) run at the next frame end on the main thread
 * (albw_ss_frame_point, from plat_end_frame). A state from another build is converted on load
 * (ss_convert.c: symbols, ranges and code recorded in a trailer at save time). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "runtime.h"
#include "savestate.h"
#include "ss_convert.h"

/* the parts (sdk/os.c, sdk/gfx.c, sdk/fs.c, sdk/memory.c, sdk/light.c, render/) */
int albw_os_ss_park_all(int timeout_ms, char *why, size_t whylen);
void albw_os_ss_release_all(void);
void albw_os_ss_write(SsBuf *b);
int albw_os_ss_check(SsBuf *b, char *why, size_t whylen);
void albw_os_ss_load(SsBuf *b);
int albw_os_ss_persistent(void);
void albw_os_ss_main_tls(void *buf, size_t n, int restore);
size_t albw_os_ss_main_tls_size(void);
void albw_gfx_ss_write(SsBuf *b);
void albw_os_ss_thr_layout(uint32_t *size, uint32_t *len_off, uint32_t *sp_off);
int albw_gfx_ss_check(SsBuf *b, char *why, size_t whylen);
void albw_gfx_ss_load(SsBuf *b);
void albw_gfx_vram(uint8_t **base, size_t *size);
void albw_fs_ss_write(SsBuf *b);
int albw_fs_ss_check(SsBuf *b, char *why, size_t whylen);
void albw_fs_ss_load(SsBuf *b);
int albw_mem_ss_region(int i, uint8_t **base, uint32_t *committed);
int albw_mem_ss_resize(int i, uint32_t committed);
size_t light_ss_export(void **out);
int light_ss_import(const void *in, size_t n);
size_t render_pool_export(void **out);
int render_pool_import(const void *in, size_t n);
size_t fraglight_export(void **out);
int fraglight_import(const void *in, size_t n);
size_t h3d_ss_state(void *buf, size_t n, int restore);
void render_ss_loaded(void);

/* ------------------------------------------------------------------ ranges */
typedef struct { const char *name; uint8_t *p; size_t n; } Range;
static Range g_ranges[256];
static int g_nranges, g_ranges_ok = -1;
static char g_ranges_err[200];
static pthread_mutex_t g_reg_m = PTHREAD_MUTEX_INITIALIZER;

void albw_ss_add_range(const char *name, void *p, size_t n) {
    pthread_mutex_lock(&g_reg_m);
    if (g_nranges < (int)(sizeof g_ranges / sizeof g_ranges[0])) g_ranges[g_nranges++] = (Range){name, p, n};
    pthread_mutex_unlock(&g_reg_m);
}

extern char albw_data[];
static char g_exe[4096];
static char *g_ssmap_text;   /* <binary>.ssmap as read at startup (ss_preload) */
static size_t g_ssmap_len;
static int load_ssmap(void) {
    char path[4200];
#if defined(__ANDROID__)
    /* set by albw_ss_android_init (runtime/android.c): the game library and the map from the APK */
    if (!g_exe[0]) { snprintf(g_ranges_err, sizeof g_ranges_err, "save states not set up (no map)"); return -1; }
#else
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    if (n <= 0) { snprintf(g_ranges_err, sizeof g_ranges_err, "cannot find the binary"); return -1; }
    g_exe[n] = 0;
#endif
    snprintf(path, sizeof path, "%s.ssmap", g_exe);
    /* the copy read at startup (ss_preload): the file may be gone by now (relinked meanwhile) */
    FILE *f = g_ssmap_text ? fmemopen(g_ssmap_text, g_ssmap_len, "r") : fopen(path, "r");
    if (!f) { snprintf(g_ranges_err, sizeof g_ranges_err, "no %s (relink with tools/link.sh)", path); return -1; }
    char line[600], name[512];
    unsigned long a, s;
    int ok = 0, nr = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "albw_data %lx", &a) == 1) { ok = a == (unsigned long)(uintptr_t)albw_data; continue; }
        if (sscanf(line, "%lx %lx %511s", &a, &s, name) == 3 && s) {
            albw_ss_add_range("static", (void *)(uintptr_t)a, s);
            nr++;
        }
    }
    fclose(f);
    if (!ok) { snprintf(g_ranges_err, sizeof g_ranges_err, "%s is from another build (relink)", path); return -1; }
    if (!nr) { snprintf(g_ranges_err, sizeof g_ranges_err, "%s lists no ranges", path); return -1; }
    return 0;
}

/* the binary file (ss_convert.c reads its symbols): Android: libalbw.so, else /proc/self/exe */
const char *albw_ss_exe_path(void) {
#if defined(__ANDROID__)
    return g_exe[0] ? g_exe : "/proc/self/exe";
#else
    return "/proc/self/exe";
#endif
}

/* the binary's identity: FNV-1a 64 of its file (a state only fits the build that wrote it) */
static uint64_t g_build;
static uint64_t build_hash(void) {
    if (g_build) return g_build;
    FILE *f = fopen(g_exe, "rb");
    uint64_t h = 1469598103934665603ull;
    if (f) {
        static uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
        fclose(f);
    }
    return g_build = h | 1;
}
/* Read <binary>.ssmap and hash the binary at startup. Both used to be read at the first save /
 * load: a relink while the game ran replaced build/albw, the running binary's path became
 * "build/albw (deleted)" and save states stopped with "no ... (deleted).ssmap" (user report).
 * Only the file is read here; load_ssmap still parses it at the first save / load (same range
 * order as before), and albw_ss_frame_point is unchanged: states are saved from inside it. */
#if defined(__ANDROID__)
/* Android (README: save states): /proc/self/exe is the Java runtime; the game library (libalbw.so,
 * loaded at a fixed address by the loader libmain.so) is the binary, and its .ssmap comes from the
 * APK assets. Called from albw_android_main before the game starts. */
uint32_t albw_ss_bias;   /* load address of the library (its ELF addresses start at 0): ss_convert.c */
void albw_ss_android_init(const char *lib, char *map, size_t len) {
    snprintf(g_exe, sizeof g_exe, "%s", lib);
    Dl_info di;
    if (dladdr((void *)albw_ss_android_init, &di) && di.dli_fbase) albw_ss_bias = (uint32_t)(uintptr_t)di.dli_fbase;
    g_ssmap_text = map;
    g_ssmap_len = map ? len : 0;
    build_hash();
}
#else
__attribute__((constructor)) static void ss_preload(void) {
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    if (n <= 0) return;
    g_exe[n] = 0;
    char path[4200];
    snprintf(path, sizeof path, "%s.ssmap", g_exe);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *t = len > 0 ? malloc((size_t)len) : NULL;
        if (t && fread(t, 1, (size_t)len, f) == (size_t)len) { g_ssmap_text = t; g_ssmap_len = (size_t)len; }
        else free(t);
        fclose(f);
    }
    build_hash();
}
#endif
static uint64_t g_session;   /* states of a run without fixed addresses load only in that run */

/* ------------------------------------------------------------------ host threads (sound) */
static pthread_mutex_t g_host_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_host_cv = PTHREAD_COND_INITIALIZER;
static int g_host_busy, g_host_hold;

void albw_ss_host_enter(void) {
    pthread_mutex_lock(&g_host_m);
    while (g_host_hold) pthread_cond_wait(&g_host_cv, &g_host_m);
    g_host_busy++;
    pthread_mutex_unlock(&g_host_m);
}
void albw_ss_host_leave(void) {
    pthread_mutex_lock(&g_host_m);
    g_host_busy--;
    pthread_cond_broadcast(&g_host_cv);
    pthread_mutex_unlock(&g_host_m);
}
static void host_hold(int on) {
    pthread_mutex_lock(&g_host_m);
    g_host_hold = on;
    if (on) while (g_host_busy) pthread_cond_wait(&g_host_cv, &g_host_m);
    pthread_cond_broadcast(&g_host_cv);
    pthread_mutex_unlock(&g_host_m);
}

/* ------------------------------------------------------------------ files */
typedef struct {
    char magic[8];             /* "ALBWSTA1" */
    uint64_t build, session;   /* session 0: loadable by any run of the build */
    int64_t when;
    int64_t frame;
    uint64_t raw, packed;
} FileHdr;

static void state_dir(char *out, size_t n) {
    const char *e = getenv("ALBW_STATE_DIR");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/albw-port/states", x);
    else snprintf(out, n, "%s/.local/share/albw-port/states", h ? h : ".");
}
static void slot_path(int slot, char *out, size_t n) {
    char d[3800];
    state_dir(d, sizeof d);
    snprintf(out, n, "%s/slot%d.state", d, slot + 1);
}
static int mkdirs(const char *p) {
    char t[4096];
    snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    return mkdir(t, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ------------------------------------------------------------------ state */
static int g_req, g_req_slot;
static char g_note[240];
static uintptr_t g_main_base;
static uint8_t *g_main_alt;
#define MAIN_ALT (512u << 10)
#define REDZONE 256u
extern long g_plat_frames __attribute__((weak));

void albw_ss_set_main_base(uintptr_t base) { g_main_base = base; }

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, sizeof g_note, fmt, ap);
    va_end(ap);
    fprintf(stderr, "savestate: %s\n", g_note);
}

/* ------------------------------------------------------------------ old states: effect textures
 * The effect library builds a texture command block for each emitter of JackAll.ptcl when the
 * game boots (003F8674, from the file's texture records):
 *   {0x11001 -> 0x80} {0 -> 0x81} {size -> 0x82} {sampler -> 0x83} {0 -> 0x84} {address >> 3 -> 0x85} ...
 * States saved by builds before 160aw hold blocks whose address points at the wrong data (not a
 * texture), so glows were drawn as solid squares (Yuga's aura, the arrow flash) for as long as
 * one played from such a state - re-saving keeps them, because the blocks are only built at boot.
 * The blocks sit at the same addresses in every boot, so a load repairs them: before the state's
 * memory is written back, the current (freshly booted) blocks pointing into JackAll's texture
 * section are recorded; afterwards, a block at the same place with the same layout, size, sampler
 * and format but another address gets the recorded address back. */
extern uint32_t g_eft_tex_lo, g_eft_tex_hi __attribute__((weak));
typedef struct { uint32_t off, addr, size, samp, fmt; } EftTex;
static int eft_block_at(const uint32_t *w, size_t i, size_t n) {
    return i >= 11 && i + 3 < n && w[i + 1] == 0x000F0085u && w[i - 1] == 0x000F0084u && w[i - 3] == 0x000F0083u &&
           w[i - 5] == 0x000F0082u && w[i - 7] == 0x000F0081u && w[i - 9] == 0x000F0080u && w[i + 3] == 0x000F008Eu;
}
static EftTex *eft_texture_scan(const uint8_t *base, size_t bytes, size_t *count) {
    *count = 0;
    if (!&g_eft_tex_lo || !g_eft_tex_lo) return NULL;
    const uint32_t *w = (const uint32_t *)base;
    size_t n = bytes / 4, cap = 0;
    EftTex *out = NULL;
    for (size_t i = 11; i + 3 < n; i++) {
        if (w[i + 1] != 0x000F0085u || !eft_block_at(w, i, n)) continue;
        uint32_t a = w[i] << 3;
        if (a < g_eft_tex_lo || a >= g_eft_tex_hi) continue;
        if (*count == cap) { cap = cap ? cap * 2 : 256; EftTex *t = realloc(out, cap * sizeof *out); if (!t) break; out = t; }
        out[(*count)++] = (EftTex){(uint32_t)(i * 4), w[i], w[i - 6], w[i - 4], w[i + 2]};
    }
    return out;
}
static int eft_texture_repair(uint8_t *base, size_t bytes, const EftTex *t, size_t count) {
    uint32_t *w = (uint32_t *)base;
    size_t n = bytes / 4;
    int fixed = 0;
    for (size_t k = 0; k < count; k++) {
        size_t i = t[k].off / 4;
        if (!eft_block_at(w, i, n) || w[i] == t[k].addr) continue;
        if (w[i - 6] != t[k].size || w[i - 4] != t[k].samp || w[i + 2] != t[k].fmt) continue;
        w[i] = t[k].addr;
        fixed++;
    }
    return fixed;
}

typedef struct { AlbwCtx ctx; uintptr_t sp, base; uint32_t len; } MainHdr;

/* for ss_convert.c: the records' layout and the saved ranges */
static void ss_layout(SsLayout *l) {
    l->mh_size = sizeof(MainHdr);
    l->mh_len_off = offsetof(MainHdr, len);
    l->mh_sp_off = offsetof(MainHdr, sp);
    albw_os_ss_thr_layout(&l->th_size, &l->th_len_off, &l->th_sp_off);
}
static SsRangeInfo *ss_range_info(void) {
    SsRangeInfo *r = calloc(g_nranges ? g_nranges : 1, sizeof *r);
    for (int i = 0; r && i < g_nranges; i++)
        r[i] = (SsRangeInfo){(uint32_t)(uintptr_t)g_ranges[i].p, (uint32_t)g_ranges[i].n, g_ranges[i].name};
    return r;
}
#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static void put_blob(SsBuf *b, uint32_t tag, void *p, size_t n) { ss_section(b, tag, p, n); free(p); }

/* everything except the main thread (MAIN, written first by do_save) */
static void write_body(SsBuf *b) {
    ss_put_u32(b, TAG('R', 'N', 'G', 'S'));
    ss_put_u32(b, (uint32_t)g_nranges);
    for (int i = 0; i < g_nranges; i++) { ss_put_u32(b, (uint32_t)g_ranges[i].n); ss_put(b, g_ranges[i].p, g_ranges[i].n); }
    for (int i = 0; i < 2; i++) {
        uint8_t *base; uint32_t n;
        albw_mem_ss_region(i, &base, &n);
        ss_section(b, TAG('M', 'E', 'M', '0' + i), base, n);
    }
    uint8_t *vb; size_t vn;
    albw_gfx_vram(&vb, &vn);
    ss_section(b, TAG('V', 'R', 'A', 'M'), vb, vn);
    size_t an = pmem_arena_used();
    size_t asz;
    ss_section(b, TAG('A', 'R', 'N', 'A'), pmem_area(PMEM_ARENA, &asz), an);
    size_t hn = h3d_ss_state(NULL, 0, 0);
    void *hs = malloc(hn);
    h3d_ss_state(hs, hn, 0);
    put_blob(b, TAG('H', '3', 'D', 'R'), hs, hn);
    void *p;
    size_t n = render_pool_export(&p);
    put_blob(b, TAG('P', 'O', 'O', 'L'), p, n);
    n = fraglight_export(&p);
    put_blob(b, TAG('F', 'L', 'R', 'G'), p, n);
    n = light_ss_export(&p);
    put_blob(b, TAG('L', 'S', 'I', 'D'), p, n);
    albw_fs_ss_write(b);
    albw_gfx_ss_write(b);
    albw_os_ss_write(b);
}

static __attribute__((noinline)) void do_save(int slot) {
    MainHdr mh;
    size_t tls_n = albw_os_ss_main_tls_size();
    if (albw_ctx_save(&mh.ctx) != 0) {   /* here again through a load of this state */
        render_ss_loaded();
        note("Loaded state %d", slot + 1);
        return;
    }
    mh.sp = albw_ctx_sp(&mh.ctx) - REDZONE;
    mh.base = g_main_base;
    mh.len = (uint32_t)(mh.base - mh.sp);
    SsBuf b = {0};
    ss_put_u32(&b, TAG('M', 'A', 'I', 'N'));
    ss_put(&b, &mh, sizeof mh);
    ss_put(&b, (const void *)mh.sp, mh.len);
    void *tls = calloc(1, tls_n);
    albw_os_ss_main_tls(tls, tls_n, 0);
    put_blob(&b, TAG('M', 'T', 'L', 'S'), tls, tls_n);
    write_body(&b);
    {   /* what a later build needs to convert this state */
        SsLayout lay;
        ss_layout(&lay);
        SsRangeInfo *ri = ss_range_info();
        if (ri) ss_convert_write_trailer(&b, &lay, ri, g_nranges);
        free(ri);
    }
    if (b.err) { free(b.p); note("Save state %d failed: out of memory", slot + 1); return; }
    uLongf packed = compressBound(b.n);
    uint8_t *z = malloc(packed);
    if (!z || compress2(z, &packed, b.p, b.n, 1) != Z_OK) {
        free(z); free(b.p);
        note("Save state %d failed: compression", slot + 1);
        return;
    }
    FileHdr h = {"ALBWSTA", build_hash(), pmem_fixed() && albw_os_ss_persistent() ? 0 : g_session, (int64_t)time(NULL),
                 &g_plat_frames ? g_plat_frames : 0, b.n, packed};
    memcpy(h.magic, "ALBWSTA1", 8);
    char path[4200], tmp[4300], dir[3800];
    state_dir(dir, sizeof dir);
    mkdirs(dir);
    slot_path(slot, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(z, 1, packed, f) == packed;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = rename(tmp, path) == 0;
    else unlink(tmp);
    if (ok) note("Saved state %d (%.0f MB)%s", slot + 1, packed / 1048576.0, h.session ? " - this run only" : "");
    else note("Save state %d failed: cannot write %s (%s)", slot + 1, path, strerror(errno));
    free(z);
    free(b.p);
}

/* ---- load */
static SsBuf g_lb;            /* the state being loaded (alive while threads restore from it) */
static const MainHdr *g_lmh;
static const uint8_t *g_lmain;

uintptr_t albw_os_ss_canary(void);
static void main_restore_tramp(void *p) {
    (void)p;
    albw_canary_set(albw_os_ss_canary());   /* the restored frames carry the saving run's canary */
    memcpy((void *)g_lmh->sp, g_lmain, g_lmh->len);
    albw_ctx_jump(&g_lmh->ctx, 1);
}

static int read_state(int slot, SsBuf *b, char *why, size_t whylen, int *other_build) {
    char path[4200];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, whylen, "empty"); return -1; }
    FileHdr h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "ALBWSTA1", 8)) { fclose(f); snprintf(why, whylen, "not a state file"); return -1; }
    *other_build = h.build != build_hash();
    if (h.session && h.session != g_session) { fclose(f); snprintf(why, whylen, "made by an earlier run that had no fixed addresses"); return -1; }
    uint8_t *z = malloc(h.packed ? h.packed : 1);
    b->p = malloc(h.raw ? h.raw : 1);
    if (!z || !b->p || fread(z, 1, h.packed, f) != h.packed) {
        fclose(f); free(z); free(b->p); b->p = NULL;
        snprintf(why, whylen, "cannot read the file");
        return -1;
    }
    fclose(f);
    uLongf raw = h.raw;
    int zr = uncompress(b->p, &raw, z, h.packed);
    free(z);
    if (zr != Z_OK || raw != h.raw) { free(b->p); b->p = NULL; snprintf(why, whylen, "damaged file"); return -1; }
    b->n = b->cap = raw;
    b->rd = 0;
    b->err = 0;
    return 0;
}

static void do_load(int slot) {
    char why[200];
    free(g_lb.p);
    memset(&g_lb, 0, sizeof g_lb);
    int other = 0;
    if (read_state(slot, &g_lb, why, sizeof why, &other)) { note("Cannot load state %d: %s", slot + 1, why); return; }
    if (other) {   /* another build: rewrite it for this one (ss_convert.c) */
        SsLayout lay;
        ss_layout(&lay);
        SsRangeInfo *ri = ss_range_info();
        int bad = !ri || ss_convert(&g_lb, &lay, ri, g_nranges, why, sizeof why);
        free(ri);
        if (bad) {
            note("Cannot load state %d: %s", slot + 1, why);
            free(g_lb.p);
            memset(&g_lb, 0, sizeof g_lb);
            return;
        }
    }
    SsBuf *b = &g_lb;
    /* 1. check every part before anything changes */
    size_t n;
#define FAIL(...) do { snprintf(why, sizeof why, __VA_ARGS__); goto fail; } while (0)
    if (ss_get_u32(b) != TAG('M', 'A', 'I', 'N')) FAIL("damaged state");
    g_lmh = ss_peek(b, sizeof(MainHdr));
    if (!g_lmh || !(g_lmain = ss_peek(b, g_lmh->len))) FAIL("damaged state (main thread)");
    if (g_lmh->base != g_main_base) FAIL("the main thread's stack differs (another build?)");
    const void *mtls = ss_get_section(b, TAG('M', 'T', 'L', 'S'), &n);
    if (!mtls || n != albw_os_ss_main_tls_size()) FAIL("damaged state (main thread)");
    if (ss_get_u32(b) != TAG('R', 'N', 'G', 'S') || ss_get_u32(b) != (uint32_t)g_nranges) FAIL("the saved data ranges differ");
    size_t rng_at = b->rd;
    for (int i = 0; i < g_nranges; i++)
        if (ss_get_u32(b) != g_ranges[i].n || !ss_peek(b, g_ranges[i].n)) FAIL("the saved data ranges differ");
    const void *mem[2];
    size_t memn[2];
    for (int i = 0; i < 2; i++)
        if (!(mem[i] = ss_get_section(b, TAG('M', 'E', 'M', '0' + i), &memn[i]))) FAIL("damaged state (memory)");
    size_t vn;
    const void *vram = ss_get_section(b, TAG('V', 'R', 'A', 'M'), &vn);
    size_t an;
    const void *arena = ss_get_section(b, TAG('A', 'R', 'N', 'A'), &an);
    size_t hn;
    const void *h3 = ss_get_section(b, TAG('H', '3', 'D', 'R'), &hn);
    size_t pn, fn, ln;
    const void *pool = ss_get_section(b, TAG('P', 'O', 'O', 'L'), &pn);
    const void *flr = ss_get_section(b, TAG('F', 'L', 'R', 'G'), &fn);
    const void *lsd = ss_get_section(b, TAG('L', 'S', 'I', 'D'), &ln);
    if (!vram || !arena || !h3 || !pool || !flr || !lsd || b->err) FAIL("damaged state");
    if (hn != h3d_ss_state(NULL, 0, 0)) FAIL("damaged state (models)");
    size_t fs_at = b->rd;
    if (albw_fs_ss_check(b, why, sizeof why) || albw_gfx_ss_check(b, why, sizeof why)) goto fail;
    size_t os_at = b->rd;
    if (albw_os_ss_check(b, why, sizeof why)) goto fail;
    for (int i = 0; i < 2; i++)
        if (albw_mem_ss_resize(i, (uint32_t)memn[i])) FAIL("memory resize failed");
    /* the freshly booted effect texture blocks, for eft_texture_repair below */
    EftTex *eft[2];
    size_t neft[2];
    for (int i = 0; i < 2; i++) {
        uint8_t *base; uint32_t c;
        albw_mem_ss_region(i, &base, &c);
        eft[i] = eft_texture_scan(base, c < memn[i] ? c : memn[i], &neft[i]);
    }
    /* 2. write back (no way back from here) */
    int eft_fixed = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t *base; uint32_t c;
        albw_mem_ss_region(i, &base, &c);
        memcpy(base, mem[i], memn[i]);
        eft_fixed += eft_texture_repair(base, memn[i], eft[i], neft[i]);
        free(eft[i]);
    }
    if (eft_fixed) fprintf(stderr, "savestate: %d effect texture blocks from an older build repaired\n", eft_fixed);
    uint8_t *vb; size_t vsz;
    albw_gfx_vram(&vb, &vsz);
    memcpy(vb, vram, vn < vsz ? vn : vsz);
    b->rd = rng_at;
    uint32_t eft_lo = &g_eft_tex_lo ? g_eft_tex_lo : 0, eft_hi = &g_eft_tex_hi ? g_eft_tex_hi : 0;
    for (int i = 0; i < g_nranges; i++) { ss_get_u32(b); memcpy(g_ranges[i].p, ss_peek(b, g_ranges[i].n), g_ranges[i].n); }
    /* the effect texture section is this boot's (states from older builds hold 0 here): keep it for
       the next load's repair */
    if (&g_eft_tex_lo && eft_lo) { g_eft_tex_lo = eft_lo; g_eft_tex_hi = eft_hi; }
    pmem_arena_restore(arena, an);
    h3d_ss_state((void *)h3, hn, 1);
    if (render_pool_import(pool, pn) || fraglight_import(flr, fn) || light_ss_import(lsd, ln))
        runtime_fatal("savestate: renderer state of slot %d is damaged (the game state is already replaced)", slot + 1);
    b->rd = fs_at;
    albw_fs_ss_load(b);
    albw_gfx_ss_load(b);
    b->rd = os_at;
    albw_os_ss_load(b);   /* the other threads are back in their saved waits (still parked) */
    albw_os_ss_main_tls((void *)mtls, albw_os_ss_main_tls_size(), 1);
    albw_ctx_run_on(g_main_alt + MAIN_ALT, main_restore_tramp, NULL);   /* -> do_save's second return */
fail:
    note("Cannot load state %d: %s", slot + 1, why);
    free(g_lb.p);
    memset(&g_lb, 0, sizeof g_lb);
#undef FAIL
}

/* main game thread, end of a frame (plat_end_frame), holding the app core */
void albw_ss_frame_point(void) {
    if (!g_req) return;
    int op = g_req, slot = g_req_slot;
    g_req = 0;
    if (g_ranges_ok < 0) {
        g_ranges_ok = load_ssmap() == 0;
        while (!g_session) g_session = ((uint64_t)rand() << 32 ^ (uint64_t)time(NULL) << 16 ^ (uint64_t)getpid()) | 1;
    }
    if (!g_ranges_ok) { note("Save states unavailable: %s", g_ranges_err); return; }
    if (!g_main_base) { note("Save states unavailable: no main stack bounds"); return; }
    if (!g_main_alt && !(g_main_alt = malloc(MAIN_ALT))) { note("Save states unavailable: out of memory"); return; }
    char why[200];
    host_hold(1);
    if (albw_os_ss_park_all(3000, why, sizeof why)) {
        host_hold(0);
        note("Cannot %s state %d now: %s", op == SS_SAVE ? "save" : "load", slot + 1, why);
        return;
    }
    if (op == SS_SAVE) do_save(slot);
    else do_load(slot);
    albw_os_ss_release_all();
    host_hold(0);
}

/* Resuming a state whose albw_ss_frame_point had a different stack frame (states saved by the
 * short-lived build of 160ap's first attempt, 2026-11-19 21:00-21:15, frame 0x209C instead of
 * 0x21DC): ss_convert.c maps the return address after do_save / do_load to albw_ss_fp_tail, which
 * does what albw_ss_frame_point does after that call, then unwinds the OLD frame (size in
 * g_ss_fp_tail_frame, set by the converter; savestate.c data is not part of a state) and returns
 * to its caller (plat_end_frame). Prologue of both versions: push ebp, ebx, edi, esi; sub F, esp. */
uint32_t g_ss_fp_tail_frame;
void albw_ss_fp_tail_work(void) {
    albw_os_ss_release_all();
    host_hold(0);
}
#if defined(__i386__)
__asm__(".text\n.globl albw_ss_fp_tail\n.type albw_ss_fp_tail, @function\nalbw_ss_fp_tail:\n"
        "    call albw_ss_fp_tail_work\n"
        "    addl g_ss_fp_tail_frame, %esp\n"
        "    popl %esi\n    popl %edi\n    popl %ebx\n    popl %ebp\n    ret\n"
        ".size albw_ss_fp_tail, .-albw_ss_fp_tail\n");
#else
/* only the i386 states of that 2026-11-19 build need it */
void albw_ss_fp_tail(void) { runtime_fatal("albw_ss_fp_tail: i386 save states only"); }
#endif

void albw_ss_request(int op, int slot) {
    if (slot < 0 || slot >= SS_SLOTS) return;
    g_req_slot = slot;
    g_req = op;
}

int albw_ss_slot_info(int slot, char *buf, size_t n) {
    char path[4200];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    FileHdr h;
    if (!f || fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "ALBWSTA1", 8)) {
        if (f) fclose(f);
        snprintf(buf, n, "empty");
        return 0;
    }
    fclose(f);
    struct tm tm;
    time_t w = (time_t)h.when;
    localtime_r(&w, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             g_exe[0] && h.build != build_hash() ? " (older build)" : "");
    return 1;
}

const char *albw_ss_note(void) { return g_note; }
