/* HD texture packs (README: HD textures): Citra / Azahar custom texture packs, e.g. "ALBW 4K" by
 * Henriko Magnifico: files tex1_<w>x<h>_<hash>_<fmt>_mip0.png anywhere under the textures folder
 *   $ALBW_TEXTURES_DIR, else $ALBW_DATA_DIR/textures, else $HOME/textures
 *   (Android: the app's files folder / textures; the Esc menu's Import copies a pack there).
 * <hash> is CityHash64 (v1.1) of the original texture's raw 3DS bytes, level 0 (checked against
 * the pack: 45 of the 73 textures of a Hyrule Castle town scene; pack.json's use_new_hash does not
 * change that for these packs); <w>x<h> and <fmt> (PICA texture format) must match too.
 * render.c asks at every 2D texture upload (hdtex_get). A hit that is decoded already is uploaded
 * at once (bigger, with a generated mip chain); otherwise the PNG is decoded on a worker thread and
 * the texture keeps its original until hdtex_poll reports it ready (no hitch on the game thread).
 * Decoded images stay in a memory cache (cap 384 MiB, least recently used evicted on the GL
 * thread). PNG: 8-bit grey / grey+alpha / RGB / RGBA / palette, not interlaced; rows are stored
 * top first and handed to GL bottom first. ALBW_HDTEX_LOG=1 logs every decode. ALBW_HDTEX=0: off; the Esc menu switch: hdtex_set_enabled. */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>
#include "hdtex.h"

/* ------------------------------------------------------------------ CityHash64 v1.1 */
static const uint64_t K0 = 0xc3a5c85c97cb3127ull, K1 = 0xb492b66fbe98f273ull, K2 = 0x9ae16a3b2f90404full;
static uint64_t f64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t f32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rot(uint64_t v, int s) { return s == 0 ? v : (v >> s) | (v << (64 - s)); }
static uint64_t smix(uint64_t v) { return v ^ (v >> 47); }
static uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }
static uint64_t hl16m(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul; a ^= a >> 47;
    uint64_t b = (v ^ a) * mul; b ^= b >> 47; return b * mul;
}
static uint64_t hl16(uint64_t u, uint64_t v) { return hl16m(u, v, 0x9ddfea08eb382d69ull); }
static uint64_t hl0to16(const uint8_t *s, size_t len) {
    if (len >= 8) {
        uint64_t mul = K2 + len * 2, a = f64(s) + K2, b = f64(s + len - 8);
        uint64_t c = rot(b, 37) * mul + a, d = (rot(a, 25) + b) * mul;
        return hl16m(c, d, mul);
    }
    if (len >= 4) {
        uint64_t mul = K2 + len * 2, a = f32(s);
        return hl16m(len + (a << 3), f32(s + len - 4), mul);
    }
    if (len > 0) {
        uint8_t a = s[0], b = s[len >> 1], c = s[len - 1];
        uint32_t y = (uint32_t)a + ((uint32_t)b << 8), z = (uint32_t)len + ((uint32_t)c << 2);
        return smix(y * K2 ^ z * K0) * K2;
    }
    return K2;
}
static uint64_t hl17to32(const uint8_t *s, size_t len) {
    uint64_t mul = K2 + len * 2, a = f64(s) * K1, b = f64(s + 8), c = f64(s + len - 8) * mul, d = f64(s + len - 16) * K2;
    return hl16m(rot(a + b, 43) + rot(c, 30) + d, a + rot(b + K2, 18) + c, mul);
}
static uint64_t hl33to64(const uint8_t *s, size_t len) {
    uint64_t mul = K2 + len * 2, a = f64(s) * K2, b = f64(s + 8), c = f64(s + len - 24), d = f64(s + len - 32);
    uint64_t e = f64(s + 16) * K2, f = f64(s + 24) * 9, g = f64(s + len - 8), h = f64(s + len - 16) * mul;
    uint64_t u = rot(a + g, 43) + (rot(b, 30) + c) * 9, v = ((a + g) ^ d) + f + 1;
    uint64_t w = bswap64((u + v) * mul) + h, x = rot(e + f, 42) + c;
    uint64_t y = (bswap64((v + w) * mul) + g) * mul, z = e + f + c;
    a = bswap64((x + z) * mul + y) + b;
    b = smix((z + a) * mul + d + h) * mul;
    return b + x;
}
static void weak32(uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b, uint64_t out[2]) {
    a += w; b = rot(b + a + z, 21);
    uint64_t c = a; a += x; a += y; b += rot(a, 44);
    out[0] = a + z; out[1] = b + c;
}
static void weak32s(const uint8_t *s, uint64_t a, uint64_t b, uint64_t out[2]) {
    weak32(f64(s), f64(s + 8), f64(s + 16), f64(s + 24), a, b, out);
}
uint64_t hdtex_cityhash64(const void *data, size_t len) {
    const uint8_t *s = data;
    if (len <= 32) return len <= 16 ? hl0to16(s, len) : hl17to32(s, len);
    if (len <= 64) return hl33to64(s, len);
    uint64_t x = f64(s + len - 40), y = f64(s + len - 16) + f64(s + len - 56);
    uint64_t z = hl16(f64(s + len - 48) + len, f64(s + len - 24)), v[2], w[2];
    weak32s(s + len - 64, len, z, v);
    weak32s(s + len - 32, y + K1, x, w);
    x = x * K1 + f64(s);
    len = (len - 1) & ~(size_t)63;
    do {
        x = rot(x + y + v[0] + f64(s + 8), 37) * K1;
        y = rot(y + v[1] + f64(s + 48), 42) * K1;
        x ^= w[1];
        y += v[0] + f64(s + 40);
        z = rot(z + w[0], 33) * K1;
        weak32s(s, v[1] * K1, x + w[0], v);
        weak32s(s + 32, z + w[1], y + f64(s + 16), w);
        uint64_t t = z; z = x; x = t;
        s += 64;
        len -= 64;
    } while (len != 0);
    return hl16(hl16(v[0], w[0]) + smix(y) * K1 + z, hl16(v[1], w[1]) + x);
}

/* ------------------------------------------------------------------ PNG (8-bit, not interlaced) */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
/* RGBA, rows bottom first (GL); NULL on anything unsupported */
static uint8_t *png_decode(const uint8_t *d, size_t n, int *ow, int *oh) {
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, lace = 0;
    uint8_t pal[256][4];
    memset(pal, 255, sizeof pal);
    uint8_t *z = NULL;
    size_t zn = 0, zcap = 0;
    for (size_t i = 8; i + 12 <= n;) {
        uint32_t len = be32(d + i);
        const uint8_t *type = d + i + 4, *c = d + i + 8;
        if (i + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; lace = c[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            for (uint32_t k = 0; k < len / 3 && k < 256; k++) { pal[k][0] = c[3 * k]; pal[k][1] = c[3 * k + 1]; pal[k][2] = c[3 * k + 2]; }
        } else if (!memcmp(type, "tRNS", 4) && ctype == 3) {
            for (uint32_t k = 0; k < len && k < 256; k++) pal[k][3] = c[k];
        } else if (!memcmp(type, "IDAT", 4)) {
            if (zn + len > zcap) { zcap = (zn + len) * 2; uint8_t *nz = realloc(z, zcap); if (!nz) { free(z); return NULL; } z = nz; }
            memcpy(z + zn, c, len);
            zn += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        i += 12 + len;
    }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!z || !w || !h || w > 8192 || h > 8192 || depth != 8 || lace || !ch) { free(z); return NULL; }
    size_t stride = (size_t)w * ch, rawn = (stride + 1) * h;
    uint8_t *raw = malloc(rawn), *out = malloc((size_t)w * h * 4);
    uLongf got = (uLongf)rawn;
    if (!raw || !out || uncompress(raw, &got, z, (uLong)zn) != Z_OK || got != rawn) { free(z); free(raw); free(out); return NULL; }
    free(z);
    uint8_t *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + y * (stride + 1), ft = row[0], *p = row + 1;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)ch ? p[x - ch] : 0, b = prev ? prev[x] : 0, cc = prev && x >= (size_t)ch ? prev[x - ch] : 0;
            switch (ft) {
            case 1: p[x] = (uint8_t)(p[x] + a); break;
            case 2: p[x] = (uint8_t)(p[x] + b); break;
            case 3: p[x] = (uint8_t)(p[x] + ((a + b) >> 1)); break;
            case 4: p[x] = (uint8_t)(p[x] + paeth(a, b, cc)); break;
            default: break;
            }
        }
        prev = p;
        uint8_t *o = out + (size_t)(h - 1 - y) * w * 4;   /* bottom row first */
        for (uint32_t x = 0; x < w; x++, o += 4) {
            const uint8_t *s = p + (size_t)x * ch;
            switch (ctype) {
            case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
            case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
            case 3: memcpy(o, pal[s[0]], 4); break;
            case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
            default: memcpy(o, s, 4); break;
            }
        }
    }
    free(raw);
    *ow = (int)w;
    *oh = (int)h;
    return out;
}

/* ------------------------------------------------------------------ pack index + cache */
typedef struct {
    uint64_t hash;
    uint16_t w, h;
    uint8_t fmt;
    int8_t state;        /* 0 not decoded, 1 queued, 2 ready, -1 failed */
    char *path;
    uint8_t *px;         /* RGBA, bottom row first (state 2) */
    int pw, ph;
    uint32_t used;       /* last frame asked for (eviction) */
} HdEnt;
static HdEnt *g_ent;
static uint32_t g_cap, g_n;                 /* open addressing, power of two */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint32_t *g_queue;                   /* entry indices to decode */
static uint32_t g_qhead, g_qtail, g_qcap;
static int g_worker, g_busy, g_enabled = -1, g_ready_gen, g_seen_gen;
static size_t g_bytes;
static uint32_t g_frame;
static char g_root[1024], g_status[160];
#define HD_CACHE_MAX ((size_t)384 << 20)

static HdEnt *find(uint64_t h) {
    if (!g_cap) return NULL;
    for (uint32_t i = (uint32_t)(h ^ h >> 32) & (g_cap - 1);; i = (i + 1) & (g_cap - 1)) {
        if (!g_ent[i].path) return NULL;
        if (g_ent[i].hash == h) return &g_ent[i];
    }
}
static void add(uint64_t h, int w, int hh, int fmt, const char *path) {
    if ((g_n + 1) * 2 > g_cap) {
        uint32_t ncap = g_cap ? g_cap * 2 : 1024;
        HdEnt *ne = calloc(ncap, sizeof *ne);
        if (!ne) return;
        for (uint32_t k = 0; k < g_cap; k++)
            if (g_ent[k].path) {
                uint64_t x = g_ent[k].hash;
                uint32_t i = (uint32_t)(x ^ x >> 32) & (ncap - 1);
                while (ne[i].path) i = (i + 1) & (ncap - 1);
                ne[i] = g_ent[k];
            }
        free(g_ent);
        g_ent = ne;
        g_cap = ncap;
    }
    if (find(h)) return;   /* the first pack with this hash wins */
    uint32_t i = (uint32_t)(h ^ h >> 32) & (g_cap - 1);
    while (g_ent[i].path) i = (i + 1) & (g_cap - 1);
    g_ent[i] = (HdEnt){.hash = h, .w = (uint16_t)w, .h = (uint16_t)hh, .fmt = (uint8_t)fmt, .path = strdup(path)};
    g_n++;
}
static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1400];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unsigned w, h, fmt;
        unsigned long long hash;
        if (sscanf(e->d_name, "tex1_%ux%u_%16llx_%u", &w, &h, &hash, &fmt) == 4 && strstr(e->d_name, ".png")) {
            add(hash, (int)w, (int)h, (int)fmt, p);
            continue;
        }
        struct stat st;
        if (depth < 8 && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) scan(p, depth + 1);
    }
    closedir(d);
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_qhead == g_qtail) pthread_cond_wait(&g_cv, &g_mu);
        uint32_t i = g_queue[g_qhead++ % g_qcap];
        char *path = g_ent && i < g_cap && g_ent[i].path ? strdup(g_ent[i].path) : NULL;
        g_busy = path != NULL;   /* hdtex_rescan waits: g_ent[i] is written below */
        pthread_mutex_unlock(&g_mu);
        if (!path) continue;
        uint8_t *px = NULL;
        int w = 0, h = 0;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) px = png_decode(buf, (size_t)n, &w, &h);
            free(buf);
            fclose(f);
        }
        pthread_mutex_lock(&g_mu);
        HdEnt *e = &g_ent[i];
        if (px) { e->px = px; e->pw = w; e->ph = h; e->state = 2; g_bytes += (size_t)w * h * 4; }
        else e->state = -1;
        g_ready_gen++;
        g_busy = 0;
        pthread_mutex_unlock(&g_mu);
        if (!px) fprintf(stderr, "hdtex: cannot decode %s\n", path);
        else {
            static int lg = -1;
            if (lg < 0) lg = getenv("ALBW_HDTEX_LOG") != NULL;
            if (lg) fprintf(stderr, "hdtex: decoded %dx%d %s\n", w, h, strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        }
        free(path);
    }
    return NULL;
}

static void root_dir(char *out, size_t n) {
    const char *t = getenv("ALBW_TEXTURES_DIR"), *d = getenv("ALBW_DATA_DIR"), *h = getenv("HOME");
    if (t && *t) snprintf(out, n, "%s", t);
    else if (d && *d) snprintf(out, n, "%s/textures", d);
    else snprintf(out, n, "%s/textures", h && *h ? h : ".");
}

void hdtex_rescan(void) {
    pthread_mutex_lock(&g_mu);
    g_qhead = g_qtail;   /* drop queued decodes; wait for the one in progress (it writes g_ent) */
    while (g_busy) { pthread_mutex_unlock(&g_mu); struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); pthread_mutex_lock(&g_mu); }
    for (uint32_t k = 0; k < g_cap; k++) { free(g_ent[k].path); free(g_ent[k].px); }
    free(g_ent);
    g_ent = NULL; g_cap = g_n = 0; g_bytes = 0;
    root_dir(g_root, sizeof g_root);
    mkdir(g_root, 0755);
    scan(g_root, 0);
    if (g_n) snprintf(g_status, sizeof g_status, "%u HD textures", g_n);
    else snprintf(g_status, sizeof g_status, "no pack installed");
    g_ready_gen++;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "hdtex: %s (%s)\n", g_status, g_root);
}

static void init_once(void) {
    if (g_enabled >= 0) return;
    const char *e = getenv("ALBW_HDTEX");
    g_enabled = !(e && !strcmp(e, "0"));
    hdtex_rescan();
}

int hdtex_active(void) { init_once(); return g_enabled && g_n > 0; }
int hdtex_enabled(void) { init_once(); return g_enabled; }
void hdtex_set_enabled(int on) { init_once(); g_enabled = on != 0; }
const char *hdtex_status(void) { init_once(); return g_status; }
const char *hdtex_root(void) { init_once(); return g_root; }

int hdtex_get(uint64_t hash, int w, int h, int fmt, const uint8_t **px, int *pw, int *ph) {
    if (!hdtex_active()) return -1;
    pthread_mutex_lock(&g_mu);
    HdEnt *e = find(hash);
    int r = -1;
    if (e && e->w == w && e->h == h && e->fmt == fmt) {
        e->used = g_frame;
        if (e->state == 2) { *px = e->px; *pw = e->pw; *ph = e->ph; r = 1; }
        else if (e->state == 0 || e->state == 1) {
            if (e->state == 0) {
                if (!g_worker) {
                    pthread_t t;
                    if (pthread_create(&t, NULL, worker, NULL) == 0) { pthread_detach(t); g_worker = 1; }
                }
                if (g_qtail - g_qhead >= g_qcap) {   /* grow the ring */
                    uint32_t ncap = g_qcap ? g_qcap * 2 : 256, *nq = malloc(ncap * sizeof *nq);
                    if (nq) {
                        for (uint32_t k = 0; k < g_qtail - g_qhead; k++) nq[k] = g_queue[(g_qhead + k) % g_qcap];
                        g_qtail -= g_qhead; g_qhead = 0;
                        free(g_queue); g_queue = nq; g_qcap = ncap;
                    }
                }
                if (g_qtail - g_qhead < g_qcap) {
                    g_queue[g_qtail++ % g_qcap] = (uint32_t)(e - g_ent);
                    e->state = 1;
                    pthread_cond_signal(&g_cv);
                }
            }
            r = 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* GL thread, once per frame: 1 when images became ready since the last call; evicts the least
 * recently used decoded images over the cap (never one asked for this frame) */
int hdtex_poll(void) {
    if (g_enabled <= 0) return 0;
    pthread_mutex_lock(&g_mu);
    g_frame++;
    while (g_bytes > HD_CACHE_MAX) {
        HdEnt *old = NULL;
        for (uint32_t k = 0; k < g_cap; k++)
            if (g_ent[k].state == 2 && g_ent[k].used != g_frame && (!old || g_ent[k].used < old->used)) old = &g_ent[k];
        if (!old) break;
        g_bytes -= (size_t)old->pw * old->ph * 4;
        free(old->px);
        old->px = NULL;
        old->state = 0;
    }
    int r = g_ready_gen != g_seen_gen;
    g_seen_gen = g_ready_gen;
    pthread_mutex_unlock(&g_mu);
    return r;
}
