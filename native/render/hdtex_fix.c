/* Corrections for known mistakes in HD texture packs (README: HD textures > Pack fixes).
 *
 * A pack image that matches a texture's hash but was drawn wrong (glyphs in the wrong cells, ...)
 * is repaired in memory right after it is decoded (PNG or disk cache), before the disk cache
 * stores it, so the user's pack files stay untouched and a download straight from the pack's
 * source just works.
 *
 * Adding a fix: one Fix entry in FIXES below.
 *   - hash / w / h / fmt: the ORIGINAL texture (the tex1_<w>x<h>_<hash>_<fmt>_mip0.png name).
 *   - ops: run in order. Every rectangle is in the original texture's texels, top-left origin
 *     (as the image looks in a viewer), fractions allowed; they are scaled to the pack image's
 *     size, so one entry serves every resolution of a pack (1080p, 4K, ...).
 *       EXPECT  rect lo hi       the mean alpha (0..255) of rect must be within lo..hi, else the
 *                                fix is skipped. Put every EXPECT first: together they must
 *                                recognise the broken image only, so a corrected pack, or the
 *                                image after the fix itself (the disk cache), is left alone.
 *       MOVE    rect dx dy       moves rect by (dx, dy) texels; the uncovered rest becomes empty
 *       CLEAR   rect             makes rect fully transparent
 *       COPY    src dst          scales src into dst (bilinear), drawn over what is there
 *   - what: a line for the log ("hdtex: fixed ...").
 * Find the rectangles by looking at the pack PNG next to the original texture (ALBW_TEX_DUMP_DIR
 * dumps the originals). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdtex.h"

enum { OP_END = 0, OP_EXPECT, OP_MOVE, OP_CLEAR, OP_COPY };
typedef struct { float x0, y0, x1, y1; } FRect;
typedef struct {
    int op;
    FRect a;          /* EXPECT / MOVE / CLEAR: the rect; COPY: the source */
    FRect b;          /* COPY: the destination */
    float p, q;       /* EXPECT: lo, hi; MOVE: dx, dy */
} FixOp;
typedef struct {
    uint64_t hash;
    int w, h, fmt;
    const char *what;
    FixOp ops[16];
} Fix;

/* ALBW 4K (Henriko Magnifico) main font sheet, 512x1024 A4: 18x24 glyph cells on a 19x25 grid
 * from (1, 1), 26 per row. Row 26 holds the message icons; the game's order is
 *   Ravio, "er", "e", bow, bomb, wand, d-pad, d-pad, !, ?, ~
 * but the pack left out the superscript "er" (French "1er"), so cells 1..9 hold the next glyph
 * each: the "which symbol" question at Ravio's shop shows wand / d-pad / bomb in place of
 * bomb / wand / bow. The fix moves cells 1..9 back one cell right and draws the "er" from the
 * pack's own superscript "e" and its "r" (row 3, cell 2) at superscript size. */
#define CELL_X(c) (1.0f + 19.0f * (c))
#define CELL_Y(r) (1.0f + 25.0f * (r))
#define CELL(r, c) {CELL_X(c), CELL_Y(r), CELL_X(c) + 18.0f, CELL_Y(r) + 24.0f}
#define NOR {0, 0, 0, 0}   /* no second rect */
#define IN_CELL(r, c, x0, y0, x1, y1) {CELL_X(c) + (x0), CELL_Y(r) + (y0), CELL_X(c) + (x1), CELL_Y(r) + (y1)}

static const Fix FIXES[] = {
    {0xD8373A57FACBA0EAull, 512, 1024, 11, "ALBW 4K font: message icons one cell off (bomb / wand / bow)", {
        {OP_EXPECT, CELL(26, 0), NOR, 8, 255},     /* Ravio */
        {OP_EXPECT, CELL(26, 1), NOR, 8, 255},     /* the "e" where the "er" belongs */
        {OP_EXPECT, CELL(26, 9), NOR, 8, 255},     /* "~" one cell early */
        {OP_EXPECT, CELL(26, 10), NOR, 0, 0.5f},   /* and its own cell empty */
        {OP_MOVE, {CELL_X(1) - 0.5f, CELL_Y(26) - 0.5f, CELL_X(9) + 18.5f, CELL_Y(26) + 24.5f}, NOR, 19, 0},
        /* "er": the superscript "e" (now in cell 2) narrowed into the left part ... */
        {OP_COPY, IN_CELL(26, 2, 0.5f, 2.0f, 12.5f, 14.5f), IN_CELL(26, 1, 0.3f, 2.0f, 9.6f, 14.5f)},
        /* ... and the "r" (row 3, cell 2) scaled to the superscript "e"'s height, bottoms aligned, to its right */
        {OP_COPY, IN_CELL(3, 2, 0.25f, 6.75f, 9.75f, 20.12f), IN_CELL(26, 1, 9.0f, 2.03f, 17.9f, 14.53f)},
    }},
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* texel rect -> pixel bounds of the pack image (top-left origin) */
static void to_px(const FRect *r, float sx, float sy, int pw, int ph, int *x0, int *y0, int *x1, int *y1) {
    *x0 = (int)floorf(clampf(r->x0 * sx, 0, (float)pw));
    *y0 = (int)floorf(clampf(r->y0 * sy, 0, (float)ph));
    *x1 = (int)ceilf(clampf(r->x1 * sx, 0, (float)pw));
    *y1 = (int)ceilf(clampf(r->y1 * sy, 0, (float)ph));
}
/* px is RGBA with the bottom row first (as handed to GL): top-left (x, y) -> pixel */
static uint8_t *pix(uint8_t *px, int pw, int ph, int x, int y) { return px + ((size_t)(ph - 1 - y) * pw + x) * 4; }

static int expect(uint8_t *px, int pw, int ph, float sx, float sy, const FixOp *o) {
    int x0, y0, x1, y1;
    to_px(&o->a, sx, sy, pw, ph, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return 0;
    double sum = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) sum += pix(px, pw, ph, x, y)[3];
    double mean = sum / ((double)(x1 - x0) * (y1 - y0));
    return mean >= o->p && mean <= o->q;
}
static void clear(uint8_t *px, int pw, int ph, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) memset(pix(px, pw, ph, x0, y), 0, (size_t)(x1 - x0) * 4);
}
static int move(uint8_t *px, int pw, int ph, float sx, float sy, const FixOp *o) {
    int x0, y0, x1, y1, dx = (int)lroundf(o->p * sx), dy = (int)lroundf(o->q * sy);
    to_px(&o->a, sx, sy, pw, ph, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return 1;
    uint8_t *tmp = malloc((size_t)w * h * 4);
    if (!tmp) return 0;
    for (int y = 0; y < h; y++) memcpy(tmp + (size_t)y * w * 4, pix(px, pw, ph, x0, y0 + y), (size_t)w * 4);
    clear(px, pw, ph, x0, y0, x1, y1);
    for (int y = 0; y < h; y++) {
        int ty = y0 + dy + y;
        if (ty < 0 || ty >= ph) continue;
        for (int x = 0; x < w; x++) {
            int tx = x0 + dx + x;
            if (tx >= 0 && tx < pw) memcpy(pix(px, pw, ph, tx, ty), tmp + ((size_t)y * w + x) * 4, 4);
        }
    }
    free(tmp);
    return 1;
}
/* bilinear sample of the snapshot (top-left origin) at pixel centre coordinates */
static void sample(const uint8_t *s, int w, int h, float x, float y, float out[4]) {
    x = clampf(x - 0.5f, 0, (float)w - 1); y = clampf(y - 0.5f, 0, (float)h - 1);
    int ix = (int)x, iy = (int)y, jx = ix + 1 < w ? ix + 1 : ix, jy = iy + 1 < h ? iy + 1 : iy;
    float fx = x - ix, fy = y - iy;
    for (int k = 0; k < 4; k++) {
        float a = s[((size_t)iy * w + ix) * 4 + k] * (1 - fx) + s[((size_t)iy * w + jx) * 4 + k] * fx;
        float b = s[((size_t)jy * w + ix) * 4 + k] * (1 - fx) + s[((size_t)jy * w + jx) * 4 + k] * fx;
        out[k] = a * (1 - fy) + b * fy;
    }
}
static int copy(uint8_t *px, int pw, int ph, float sx, float sy, const FixOp *o) {
    int a0, b0, a1, b1, x0, y0, x1, y1;
    to_px(&o->a, sx, sy, pw, ph, &a0, &b0, &a1, &b1);
    to_px(&o->b, sx, sy, pw, ph, &x0, &y0, &x1, &y1);
    int sw = a1 - a0, sh = b1 - b0;
    if (sw <= 0 || sh <= 0 || x1 <= x0 || y1 <= y0) return 1;
    uint8_t *s = malloc((size_t)sw * sh * 4);   /* snapshot: source and destination may overlap */
    if (!s) return 0;
    for (int y = 0; y < sh; y++) memcpy(s + (size_t)y * sw * 4, pix(px, pw, ph, a0, b0 + y), (size_t)sw * 4);
    /* downscaling by more than 2: average a few samples per pixel */
    float kx = (float)sw / (x1 - x0), ky = (float)sh / (y1 - y0);
    int nx = (int)ceilf(kx), ny = (int)ceilf(ky);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float c[4] = {0}, t[4];
            for (int j = 0; j < ny; j++)
                for (int i = 0; i < nx; i++) {
                    sample(s, sw, sh, (x - x0 + (i + 0.5f) / nx) * kx, (y - y0 + (j + 0.5f) / ny) * ky, t);
                    for (int k = 0; k < 4; k++) c[k] += t[k];
                }
            for (int k = 0; k < 4; k++) c[k] /= (float)(nx * ny);
            uint8_t *d = pix(px, pw, ph, x, y);
            float sa = c[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);   /* source over */
            for (int k = 0; k < 3; k++)
                d[k] = (uint8_t)lroundf(oa > 0 ? (c[k] * sa + d[k] * da * (1 - sa)) / oa : 0);
            d[3] = (uint8_t)lroundf(oa * 255.0f);
        }
    free(s);
    return 1;
}

int hdtex_fix_apply(uint64_t hash, int w, int h, int fmt, uint8_t *px, int pw, int ph) {
    int done = 0;
    if (!px || w <= 0 || h <= 0) return 0;
    for (size_t i = 0; i < sizeof FIXES / sizeof *FIXES; i++) {
        const Fix *f = &FIXES[i];
        if (f->hash != hash || f->w != w || f->h != h || f->fmt != fmt) continue;
        float sx = (float)pw / w, sy = (float)ph / h;
        const FixOp *o = f->ops;
        int ok = 1;
        for (; o->op == OP_EXPECT && ok; o++) ok = expect(px, pw, ph, sx, sy, o);
        if (!ok) continue;   /* not the broken image (fixed pack, or fixed already) */
        for (; o < f->ops + sizeof f->ops / sizeof *f->ops && o->op != OP_END; o++) {
            if (o->op == OP_MOVE) move(px, pw, ph, sx, sy, o);
            else if (o->op == OP_CLEAR) {
                int x0, y0, x1, y1;
                to_px(&o->a, sx, sy, pw, ph, &x0, &y0, &x1, &y1);
                if (x1 > x0 && y1 > y0) clear(px, pw, ph, x0, y0, x1, y1);
            } else if (o->op == OP_COPY) copy(px, pw, ph, sx, sy, o);
        }
        fprintf(stderr, "hdtex: fixed %s\n", f->what);
        done++;
    }
    return done;
}
