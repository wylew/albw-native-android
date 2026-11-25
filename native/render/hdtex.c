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
 * thread). Disk cache (README: HD textures): every decoded image LZ4-compressed in
 * $ALBW_HDTEX_CACHE_DIR / $XDG_CACHE_HOME/albw-port/hdtex / ~/.cache/albw-port/hdtex, read instead of
 * the PNG (~5x faster), filled for the whole pack by a low-priority builder thread when idle.
 * PNG: 8-bit grey / grey+alpha / RGB / RGBA / palette, not interlaced; rows are stored
 * top first and handed to GL bottom first. ALBW_HDTEX_LOG=1 logs every decode. ALBW_HDTEX=0: off; the Esc menu switch: hdtex_set_enabled. */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
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

/* ------------------------------------------------------------------ LZ4 block format (disk cache) */
static size_t lz4_bound(size_t n) { return n + n / 255 + 16; }
static uint8_t *lz4_len(uint8_t *o, size_t n) { for (; n >= 255; n -= 255) *o++ = 255; *o++ = (uint8_t)n; return o; }
/* greedy, 64 KiB window, 4-byte matches through a 16-bit-hash table; returns the compressed size */
static size_t lz4_compress(const uint8_t *src, size_t n, uint8_t *dst) {
    enum { HB = 16 };
    uint32_t *tab = calloc(1u << HB, sizeof *tab);   /* position + 1 */
    if (!tab) return 0;
    const uint8_t *ip = src, *anchor = src, *end = src + n, *mlimit = n > 12 ? end - 12 : src;
    uint8_t *op = dst;
    while (ip < mlimit) {
        uint32_t seq = f32(ip), hsh = (seq * 2654435761u) >> (32 - HB);
        uint32_t cand = tab[hsh];
        tab[hsh] = (uint32_t)(ip - src) + 1;
        const uint8_t *m = cand ? src + cand - 1 : NULL;
        if (!m || ip - m > 65535 || f32(m) != seq) { ip++; continue; }
        while (ip > anchor && m > src && ip[-1] == m[-1]) { ip--; m--; }
        const uint8_t *p = ip + 4, *q = m + 4, *lim = end - 5;   /* the last 5 bytes are literals */
        while (p < lim && *p == *q) { p++; q++; }
        size_t lit = (size_t)(ip - anchor), ml = (size_t)(p - ip) - 4;
        uint8_t *tok = op++;
        *tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4 | (ml >= 15 ? 15 : ml));
        if (lit >= 15) op = lz4_len(op, lit - 15);
        memcpy(op, anchor, lit); op += lit;
        uint16_t off = (uint16_t)(ip - m);
        *op++ = (uint8_t)off; *op++ = (uint8_t)(off >> 8);
        if (ml >= 15) op = lz4_len(op, ml - 15);
        ip = anchor = p;
    }
    size_t lit = (size_t)(end - anchor);
    *op++ = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) op = lz4_len(op, lit - 15);
    memcpy(op, anchor, lit); op += lit;
    free(tab);
    return (size_t)(op - dst);
}
/* 1 when exactly n bytes came out */
static int lz4_decompress(const uint8_t *s, size_t sn, uint8_t *d, size_t n) {
    const uint8_t *se = s + sn;
    uint8_t *o = d, *de = d + n;
    while (s < se) {
        unsigned tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { unsigned b; do { if (s >= se) return 0; b = *s++; lit += b; } while (b == 255); }
        if (lit > (size_t)(se - s) || lit > (size_t)(de - o)) return 0;
        memcpy(o, s, lit); o += lit; s += lit;
        if (s >= se) break;   /* the last sequence has no match */
        if (se - s < 2) return 0;
        size_t off = s[0] | (size_t)s[1] << 8;
        s += 2;
        size_t ml = (tok & 15);
        if (ml == 15) { unsigned b; do { if (s >= se) return 0; b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > (size_t)(o - d) || ml > (size_t)(de - o)) return 0;
        const uint8_t *m = o - off;
        if (off >= 8) while (ml >= 8) { memcpy(o, m, 8); o += 8; m += 8; ml -= 8; }
        while (ml--) *o++ = *m++;
    }
    return o == de;
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
    int8_t disk;         /* disk cache: 0 not checked / missing, 1 valid file, -1 cannot be built */
    uint8_t pin;         /* px being written to the disk cache: not evicted */
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
static int g_enabled = -1, g_ready_gen, g_seen_gen;
static size_t g_bytes;
static uint32_t g_frame;
static char g_root[1024], g_status[160];
#define HD_CACHE_MAX ((size_t)384 << 20)

/* disk cache (see the top): g_pre is the background build's next entry, g_checked its first pass
 * (stale files removed, valid ones counted) done since the last scan */
static volatile int g_cache_on = 1;
static int g_checked, g_cache_done_logged;
static uint32_t g_pre, g_cache_n;
static uint64_t g_cache_bytes;
static char g_cdir[1024], g_cache_status[96];
static double g_build_t0;
static int g_log = -1;
static void log_init(void) { if (g_log < 0) g_log = getenv("ALBW_HDTEX_LOG") != NULL; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

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

/* ------------------------------------------------------------------ disk cache files */
/* <cache dir>/<hash>_<w>x<h>_<fmt>.lz4: header, then the decoded image (RGBA, bottom row first)
 * LZ4-compressed. The source PNG's size and modification time are in the header: a changed or
 * replaced pack file makes the entry stale (decoded again, rewritten). */
typedef struct {
    char magic[8];
    uint64_t hash, src_size, zhash;   /* zhash: CityHash64 of the compressed data (corruption) */
    int64_t src_mtime;
    uint32_t pw, ph, zlen;
    uint16_t w, h;
    uint8_t fmt, pad[7];
} CacheHdr;
typedef struct { uint64_t hash; int w, h, fmt; char *path; } Job;
#define CACHE_MAGIC "ALBWHDC2"

static void cache_dir_init(void) {
    if (g_cdir[0]) return;
    const char *d = getenv("ALBW_HDTEX_CACHE_DIR"), *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (d && *d) snprintf(g_cdir, sizeof g_cdir, "%s", d);
    else if (x && *x) snprintf(g_cdir, sizeof g_cdir, "%s/albw-port/hdtex", x);
    else snprintf(g_cdir, sizeof g_cdir, "%s/.cache/albw-port/hdtex", h && *h ? h : ".");
    for (char *q = g_cdir + 1; *q; q++)   /* mkdir -p */
        if (*q == '/') { *q = 0; mkdir(g_cdir, 0755); *q = '/'; }
    mkdir(g_cdir, 0755);
}
static void cache_path(const Job *j, char *buf, size_t n) {
    snprintf(buf, n, "%s/%016llx_%dx%d_%d.lz4", g_cdir, (unsigned long long)j->hash, j->w, j->h, j->fmt);
}
static int hdr_ok(const CacheHdr *c, const Job *j, const struct stat *src) {
    return !memcmp(c->magic, CACHE_MAGIC, 8) && c->hash == j->hash && c->w == j->w && c->h == j->h && c->fmt == j->fmt &&
           c->src_size == (uint64_t)src->st_size && c->src_mtime == (int64_t)src->st_mtime &&
           c->pw && c->ph && c->pw <= 8192 && c->ph <= 8192 && c->zlen;
}
/* the file's size when it is valid for this source, else 0 */
static uint64_t cache_valid(const Job *j, const struct stat *src) {
    char p[1200];
    cache_path(j, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    CacheHdr c;
    struct stat st;
    int ok = fread(&c, sizeof c, 1, f) == 1 && hdr_ok(&c, j, src) && fstat(fileno(f), &st) == 0 &&
             (uint64_t)st.st_size == sizeof c + (uint64_t)c.zlen;
    fclose(f);
    return ok ? sizeof c + (uint64_t)c.zlen : 0;
}
/* *fsize: the file's size; *bad: there was a file but it was stale or damaged (deleted) */
static uint8_t *cache_read(const Job *j, const struct stat *src, int *pw, int *ph, uint64_t *fsize, int *bad) {
    char p[1200];
    cache_path(j, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    CacheHdr c;
    uint8_t *z = NULL, *px = NULL;
    struct stat st;
    *fsize = fstat(fileno(f), &st) == 0 ? (uint64_t)st.st_size : 0;
    if (fread(&c, sizeof c, 1, f) == 1 && hdr_ok(&c, j, src) && (z = malloc(c.zlen)) && fread(z, 1, c.zlen, f) == c.zlen &&
        hdtex_cityhash64(z, c.zlen) == c.zhash) {
        size_t n = (size_t)c.pw * c.ph * 4;
        px = malloc(n);
        if (px && !lz4_decompress(z, c.zlen, px, n)) { free(px); px = NULL; }
        if (px) { *pw = (int)c.pw; *ph = (int)c.ph; }
    }
    free(z);
    fclose(f);
    if (!px) { remove(p); *bad = 1; }
    return px;
}
/* bytes written, 0 on failure */
static uint64_t cache_write(const Job *j, const struct stat *src, const uint8_t *px, int pw, int ph, int tag) {
    size_t n = (size_t)pw * ph * 4;
    uint8_t *z = malloc(lz4_bound(n));
    if (!z) return 0;
    size_t zn = lz4_compress(px, n, z);
    char p[1200], tmp[1300];
    cache_path(j, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", p, tag);   /* one per thread */
    uint64_t r = 0;
    FILE *f = zn ? fopen(tmp, "wb") : NULL;
    if (f) {
        CacheHdr c = {.hash = j->hash, .src_size = (uint64_t)src->st_size, .src_mtime = (int64_t)src->st_mtime,
                      .pw = (uint32_t)pw, .ph = (uint32_t)ph, .zlen = (uint32_t)zn, .zhash = hdtex_cityhash64(z, zn),
                      .w = (uint16_t)j->w, .h = (uint16_t)j->h, .fmt = (uint8_t)j->fmt};
        memcpy(c.magic, CACHE_MAGIC, 8);
        int ok = fwrite(&c, sizeof c, 1, f) == 1 && fwrite(z, 1, zn, f) == zn;
        ok &= fclose(f) == 0;
        if (ok && g_cache_on && rename(tmp, p) == 0) r = sizeof c + zn;
        else remove(tmp);
    }
    free(z);
    return r;
}
static uint8_t *png_file(const char *path, int *w, int *h) {
    uint8_t *px = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) px = png_decode(buf, (size_t)n, w, h);
    free(buf);
    fclose(f);
    return px;
}
static void cache_status_update(void) {   /* g_mu held */
    if (!g_cache_on) snprintf(g_cache_status, sizeof g_cache_status, "off");
    else if (!g_n) snprintf(g_cache_status, sizeof g_cache_status, "empty");
    else snprintf(g_cache_status, sizeof g_cache_status, "%u/%u, %llu MB%s", g_cache_n, g_n,
                  (unsigned long long)(g_cache_bytes >> 20), g_checked && g_pre >= g_cap ? "" : ", building");
}

/* first pass of the background build, on the worker: removes cache files no pack texture has
 * (a pack removed / replaced), marks and counts the valid ones */
static void cache_check(void) {
    DIR *d = opendir(g_cdir);
    struct dirent *de;
    char p[1400];
    while (d && (de = readdir(d))) {
        unsigned long long hash;
        int w, h, fmt;
        if (de->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", g_cdir, de->d_name);
        if (sscanf(de->d_name, "%16llx_%dx%d_%d.lz4", &hash, &w, &h, &fmt) != 4 || strstr(de->d_name, ".tmp")) {
            struct stat ts;   /* a leftover temporary file (not one being written now) */
            if (stat(p, &ts) == 0 && time(NULL) - ts.st_mtime > 60) remove(p);
            continue;
        }
        pthread_mutex_lock(&g_mu);
        HdEnt *e = find(hash);
        Job j = {hash, w, h, fmt, e && e->w == w && e->h == h && e->fmt == fmt ? strdup(e->path) : NULL};
        pthread_mutex_unlock(&g_mu);
        struct stat src;
        uint64_t sz = j.path && stat(j.path, &src) == 0 ? cache_valid(&j, &src) : 0;
        free(j.path);
        if (!sz) { remove(p); continue; }
        pthread_mutex_lock(&g_mu);
        if ((e = find(hash)) && e->disk == 0) { e->disk = 1; g_cache_n++; g_cache_bytes += sz; }
        pthread_mutex_unlock(&g_mu);
    }
    if (d) closedir(d);
}

/* two threads: the loader serves the textures the game asks for (the queue: disk cache, else the
 * PNG); the builder (lower priority) fills the disk cache when there is a pack and nothing else
 * to do, so a request never waits behind a build. g_gen changes at every scan: a thread whose
 * entry table went away meanwhile drops its result (rescan never waits for them). */
static int g_gen;
static void start_workers(void);
static void *worker(void *arg) {
    const int builder = arg != NULL;
    log_init();
#if defined(__linux__)
    if (builder) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    for (;;) {
        pthread_mutex_lock(&g_mu);
        uint32_t i = 0;
        int check = 0;
        for (;;) {
            if (!builder) {
                if (g_qhead != g_qtail) {
                    i = g_queue[g_qhead++ % g_qcap];
                    if (g_ent && i < g_cap && g_ent[i].path && g_ent[i].state == 1) break;
                    continue;
                }
            } else if (g_cache_on && g_enabled > 0 && g_n) {
                if (!g_checked) { check = 1; break; }
                /* not one the loader has (state 1): it writes the cache itself */
                while (g_pre < g_cap && !(g_ent[g_pre].path && g_ent[g_pre].disk == 0 && (g_ent[g_pre].state == 0 || g_ent[g_pre].state == 2))) g_pre++;
                if (g_pre < g_cap) { i = g_pre++; break; }
                if (!g_cache_done_logged) {
                    g_cache_done_logged = 1;
                    cache_status_update();
                    fprintf(stderr, "hdtex: disk cache complete: %s in %.1f s\n", g_cache_status, now_s() - g_build_t0);
                }
            }
            pthread_cond_wait(&g_cv, &g_mu);
        }
        const int gen = g_gen;
        if (check) {
            g_build_t0 = now_s();
            pthread_mutex_unlock(&g_mu);
            cache_check();
            pthread_mutex_lock(&g_mu);
            if (gen == g_gen) {
                g_checked = 1;
                g_pre = 0;
                cache_status_update();
                fprintf(stderr, "hdtex: disk cache %s (%s)\n", g_cache_status, g_cdir);
            }
            pthread_mutex_unlock(&g_mu);
            continue;
        }
        HdEnt *e = &g_ent[i];
        Job j = {e->hash, e->w, e->h, e->fmt, strdup(e->path)};
        int cache_on = g_cache_on;
        pthread_mutex_unlock(&g_mu);
        struct stat src;
        int have_src = j.path && stat(j.path, &src) == 0;
        uint8_t *px = NULL;
        int w = 0, h = 0, from_disk = 0, bad = 0;
        uint64_t fsize = 0;
        double t0 = now_s();
        if (!builder && have_src && cache_on && (px = cache_read(&j, &src, &w, &h, &fsize, &bad))) from_disk = 1;
        if (!px && have_src) px = png_file(j.path, &w, &h);
        if (px) hdtex_fix_apply(j.hash, j.w, j.h, j.fmt, px, w, h);   /* known pack mistakes (hdtex_fix.c) */
        double t1 = now_s();
        int write = px && !from_disk && cache_on, ok = px != NULL;
        uint8_t *own = px;      /* freed here unless the entry takes it */
        pthread_mutex_lock(&g_mu);
        if (gen != g_gen) write = 0;   /* rescanned meanwhile: drop it */
        else {
            e = &g_ent[i];
            if (bad && e->disk == 1) {   /* counted as cached, but deleted: written again below */
                e->disk = 0; g_cache_n--; g_cache_bytes -= fsize < g_cache_bytes ? fsize : g_cache_bytes;
            }
            if (e->disk != 0 && !from_disk) write = 0;   /* the other thread wrote it meanwhile */
            if (px && (e->state == 1 || (!builder && e->state != 2))) {   /* wanted: publish */
                e->px = px; e->pw = w; e->ph = h; e->state = 2; g_bytes += (size_t)w * h * 4;
                e->pin = (uint8_t)write;   /* not evicted while written below */
                g_ready_gen++;
                own = NULL;
            } else if (!px && !builder) {
                e->state = -1;
                g_ready_gen++;
            }
            if (from_disk && e->disk == 0) { e->disk = 1; g_cache_n++; g_cache_bytes += fsize; }
            if (builder && !px && e->disk == 0) e->disk = -1;   /* cannot decode: not again this session */
        }
        pthread_mutex_unlock(&g_mu);
        if (write) {
            uint64_t sz = cache_write(&j, &src, px, w, h, builder);
            pthread_mutex_lock(&g_mu);
            if (gen == g_gen) {
                e = &g_ent[i];
                if (!own) e->pin = 0;
                if (e->disk == 0) { e->disk = sz ? 1 : -1; if (sz) { g_cache_n++; g_cache_bytes += sz; } }
            } else if (!own) own = px;   /* rescan left the pinned image to this thread */
            pthread_mutex_unlock(&g_mu);
        }
        free(own);
        const char *nm = j.path && strrchr(j.path, '/') ? strrchr(j.path, '/') + 1 : j.path;
        if (!ok) fprintf(stderr, "hdtex: cannot decode %s\n", j.path ? j.path : "?");
        else if (g_log)
            fprintf(stderr, "hdtex: %s %dx%d in %.0f ms (+%.0f ms cache write) %s\n", from_disk ? "cache" : builder ? "build" : "decoded",
                    w, h, (t1 - t0) * 1e3, write ? (now_s() - t1) * 1e3 : 0.0, nm);
        free(j.path);
    }
    return NULL;
}
static void start_workers(void) {   /* g_mu held */
    static int started[2];
    for (int k = 0; k < 2; k++) {
        if (started[k] || (k == 1 && !(g_cache_on && g_enabled > 0 && g_n))) continue;
        pthread_t t;
        if (pthread_create(&t, NULL, worker, k ? (void *)1 : NULL) == 0) { pthread_detach(t); started[k] = 1; }
    }
}

static void root_dir(char *out, size_t n) {
    const char *t = getenv("ALBW_TEXTURES_DIR"), *d = getenv("ALBW_DATA_DIR"), *h = getenv("HOME");
    if (t && *t) snprintf(out, n, "%s", t);
    else if (d && *d) snprintf(out, n, "%s/textures", d);
    else snprintf(out, n, "%s/textures", h && *h ? h : ".");
}

void hdtex_rescan(void) {
    pthread_mutex_lock(&g_mu);
    g_qhead = g_qtail;   /* drop queued decodes; the ones in progress see g_gen change */
    g_gen++;
    for (uint32_t k = 0; k < g_cap; k++) { free(g_ent[k].path); if (!g_ent[k].pin) free(g_ent[k].px); }   /* pinned: its thread frees it */
    free(g_ent);
    g_ent = NULL; g_cap = g_n = 0; g_bytes = 0;
    g_checked = g_cache_done_logged = 0; g_pre = g_cache_n = 0; g_cache_bytes = 0;
    root_dir(g_root, sizeof g_root);
    mkdir(g_root, 0755);
    cache_dir_init();
    scan(g_root, 0);
    if (g_n) snprintf(g_status, sizeof g_status, "%u HD textures", g_n);
    else snprintf(g_status, sizeof g_status, "no pack installed");
    cache_status_update();
    g_ready_gen++;
    if (g_n && g_cache_on && g_enabled > 0) start_workers();   /* builds the disk cache when idle */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "hdtex: %s (%s)\n", g_status, g_root);
}

static int g_cfg_enabled = 1;
static void init_once(void) {   /* ALBW_HDTEX=0 / ALBW_HDTEX_CACHE=0 override the settings */
    if (g_enabled >= 0) return;
    const char *e = getenv("ALBW_HDTEX");
    g_enabled = g_cfg_enabled && !(e && !strcmp(e, "0"));
    e = getenv("ALBW_HDTEX_CACHE");
    if (e && !strcmp(e, "0")) g_cache_on = 0;
    hdtex_rescan();
}
/* the settings at start (before the first scan, so nothing starts that the settings turn off) */
void hdtex_configure(int enabled, int cache) {
    if (g_enabled >= 0) { hdtex_set_cache(cache); hdtex_set_enabled(enabled); return; }
    g_cfg_enabled = enabled != 0;
    g_cache_on = cache != 0;
    init_once();
}

int hdtex_active(void) { init_once(); return g_enabled && g_n > 0; }
int hdtex_enabled(void) { init_once(); return g_enabled; }
void hdtex_set_enabled(int on) {
    init_once();
    pthread_mutex_lock(&g_mu);
    g_enabled = on != 0;
    if (g_enabled && g_n && g_cache_on) start_workers();
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
const char *hdtex_status(void) { init_once(); return g_status; }
const char *hdtex_root(void) { init_once(); return g_root; }

/* off: no building, no reading, and every cache file deleted (the space back) */
void hdtex_set_cache(int on) {
    init_once();
    pthread_mutex_lock(&g_mu);
    on = on != 0;
    int was = g_cache_on;
    g_cache_on = on;
    if (!on && was) {
        DIR *d = opendir(g_cdir);
        struct dirent *de;
        char p[1400];
        while (d && (de = readdir(d)))
            if (de->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", g_cdir, de->d_name); remove(p); }
        if (d) closedir(d);
        for (uint32_t k = 0; k < g_cap; k++) g_ent[k].disk = 0;
        g_cache_n = 0; g_cache_bytes = 0;
    }
    if (on && !was) { g_checked = g_cache_done_logged = 0; g_pre = 0; }
    if (on && g_n && g_enabled > 0) start_workers();
    cache_status_update();
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
const char *hdtex_cache_status(void) {
    init_once();
    pthread_mutex_lock(&g_mu);
    cache_status_update();
    pthread_mutex_unlock(&g_mu);
    return g_cache_status;
}

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
                start_workers();
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
                    pthread_cond_broadcast(&g_cv);
                }
            }
            r = 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* GL thread, once per frame: 1 when images became ready since the last call; evicts the least
 * recently used decoded images over the cap (never one asked for this frame, nor one being
 * written to the disk cache); they are read back from the disk cache when asked for again */
int hdtex_poll(void) {
    if (g_enabled <= 0) return 0;
    pthread_mutex_lock(&g_mu);
    g_frame++;
    while (g_bytes > HD_CACHE_MAX) {
        HdEnt *old = NULL;
        for (uint32_t k = 0; k < g_cap; k++)
            if (g_ent[k].state == 2 && !g_ent[k].pin && g_ent[k].used != g_frame && (!old || g_ent[k].used < old->used)) old = &g_ent[k];
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
