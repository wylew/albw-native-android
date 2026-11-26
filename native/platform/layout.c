/* Screen layout engine (see layout.h). Plain C, no libm, no GL/SDL/Qt.
 * Compiled into the SDL backend by platform.c (#include, so the existing build scripts need no
 * change) and as its own translation unit by the Qt frontend. */
#include "layout.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SNAP_PX 10.0f
#define MIN_PX 32.0f

static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }
static float floorf_(float v) {
    float i = (float)(long long)v;
    return v < i ? i - 1 : i;
}
static float roundf_(float v) { return floorf_(v + 0.5f); }

static int g_top_w = 400;
int layout_top_w(void) { return g_top_w; }
void layout_set_top_w(int w) { g_top_w = w < 400 ? 400 : w; }

static int g_bottom_w = LAYOUT_BOTTOM_GAME_W;
int layout_bottom_w(void) { return g_bottom_w; }
void layout_set_bottom_w(int w) { g_bottom_w = w < LAYOUT_BOTTOM_GAME_W ? LAYOUT_BOTTOM_GAME_W : w; }

void layout_game_rect(const Layout *l, int screen, LayoutRect *out) {
    *out = l->r[screen];
    if (screen == LAYOUT_SCREEN_BOTTOM && LAYOUT_BOTTOM_W > LAYOUT_BOTTOM_GAME_W) {
        float w = out->w * LAYOUT_BOTTOM_GAME_W / LAYOUT_BOTTOM_W;
        out->x += (out->w - w) * 0.5f;
        out->w = w;
    }
}

void layout_native_size(int screen, int *w, int *h) {
    *w = screen == LAYOUT_SCREEN_TOP ? LAYOUT_TOP_W : LAYOUT_BOTTOM_W;
    *h = LAYOUT_SCREEN_H;
}

static const char *const NAMES[LAYOUT_PRESET_COUNT] = {
    "Default (top above bottom)", "Side by side", "Large top + small bottom",
    "Picture in picture", "Top screen only", "Bottom screen only", "Custom"};
static const char *const KEYS[LAYOUT_PRESET_COUNT] = {
    "default", "side_by_side", "large_top", "pip", "top_only", "bottom_only", "custom"};

const char *layout_preset_name(int p) { return p >= 0 && p < LAYOUT_PRESET_COUNT ? NAMES[p] : "?"; }
const char *layout_preset_key(int p) { return p >= 0 && p < LAYOUT_PRESET_COUNT ? KEYS[p] : "default"; }
int layout_preset_from_key(const char *k) {
    for (int i = 0; i < LAYOUT_PRESET_COUNT; i++)
        if (!strcmp(k, KEYS[i])) return i;
    return -1;
}

void layout_defaults(LayoutConfig *c) {
    memset(c, 0, sizeof *c);
    c->separate = 1;   /* one window per screen unless layout.cfg says "windows joined" */
    c->preset = LAYOUT_DEFAULT;
    c->linear_filter = 1;
    c->render_scale = 1;
    c->large_ratio = 2.5f;
    c->keep_aspect = 1;
    c->snap = 1;
    c->custom_front = LAYOUT_SCREEN_BOTTOM;
    c->confirm_load = 1;
    c->hd_textures = 1;
    c->hd_cache = 1;
    c->hud_overlay = 0;   /* off by default: work in progress (README: HUD overlay) */
#if defined(__ANDROID__)
    c->menu_centered = 1;   /* a controller-sized panel, not a bar across the top */
#endif
    /* custom starts as the default layout of a 400x480 window */
    c->custom[0] = (LayoutRect){0.0f, 0.0f, 1.0f, 0.5f};
    c->custom[1] = (LayoutRect){0.1f, 0.5f, 0.8f, 0.5f};
    c->custom_visible[0] = c->custom_visible[1] = 1;
}

static float clamp_ratio(float r) { return r < 1.5f ? 1.5f : r > 6.0f ? 6.0f : r; }

/* Preset arrangement in 3DS pixels: rect per screen (canvas units), canvas size, visibility, order. */
static void preset_canvas(const LayoutConfig *c, LayoutRect r[2], int vis[2], int order[2], float *cw, float *ch) {
    int p = c->preset;
    int a = c->swap ? LAYOUT_SCREEN_BOTTOM : LAYOUT_SCREEN_TOP, b = 1 - a;  /* a = primary/first */
    int aw, bw, h;
    layout_native_size(a, &aw, &h);
    layout_native_size(b, &bw, &h);
    float k = clamp_ratio(c->large_ratio);
    vis[0] = vis[1] = 1;
    order[0] = LAYOUT_SCREEN_TOP;
    order[1] = LAYOUT_SCREEN_BOTTOM;
    switch (p) {
    default:
    case LAYOUT_DEFAULT: {
        float w = (float)(aw > bw ? aw : bw);
        r[a] = (LayoutRect){(w - aw) / 2, 0, (float)aw, (float)h};
        r[b] = (LayoutRect){(w - bw) / 2, (float)h, (float)bw, (float)h};
        *cw = w;
        *ch = 2.0f * h;
        break;
    }
    case LAYOUT_SIDE_BY_SIDE:
        r[a] = (LayoutRect){0, 0, (float)aw, (float)h};
        r[b] = (LayoutRect){(float)aw, 0, (float)bw, (float)h};
        *cw = (float)(aw + bw);
        *ch = (float)h;
        break;
    case LAYOUT_LARGE_TOP:
        r[a] = (LayoutRect){0, 0, aw * k, h * k};
        r[b] = (LayoutRect){aw * k, h * k - h, (float)bw, (float)h};
        *cw = aw * k + bw;
        *ch = h * k;
        break;
    case LAYOUT_PIP: {
        float sw = bw / k * 1.25f, sh = h / k * 1.25f;
        r[a] = (LayoutRect){0, 0, (float)aw, (float)h};
        r[b] = (LayoutRect){aw - sw, h - sh, sw, sh};
        *cw = (float)aw;
        *ch = (float)h;
        order[0] = a;
        order[1] = b;
        break;
    }
    case LAYOUT_TOP_ONLY:
    case LAYOUT_BOTTOM_ONLY: {
        int s = p == LAYOUT_TOP_ONLY ? LAYOUT_SCREEN_TOP : LAYOUT_SCREEN_BOTTOM, w;
        layout_native_size(s, &w, &h);
        r[s] = (LayoutRect){0, 0, (float)w, (float)h};
        r[1 - s] = (LayoutRect){0, 0, 0, 0};
        vis[1 - s] = 0;
        *cw = (float)w;
        *ch = (float)h;
        break;
    }
    }
}

/* single-screen mode: the one-screen preset of the screen that is up */
static LayoutConfig single_as_preset(const LayoutConfig *c) {
    LayoutConfig d = *c;
    d.single_screen = 0;
    d.preset = c->show_bottom ? LAYOUT_BOTTOM_ONLY : LAYOUT_TOP_ONLY;
    d.swap = 0;
    return d;
}

void layout_canvas_size(const LayoutConfig *c, int *w, int *h) {
    LayoutRect r[2];
    int vis[2], order[2];
    float cw, ch;
    LayoutConfig d = c->single_screen ? single_as_preset(c) : *c;
    if (d.preset == LAYOUT_CUSTOM) d.preset = LAYOUT_DEFAULT;
    preset_canvas(&d, r, vis, order, &cw, &ch);
    *w = (int)roundf_(cw);
    *h = (int)roundf_(ch);
}

static LayoutRect pixel_align(LayoutRect r) {
    float x0 = roundf_(r.x), y0 = roundf_(r.y), x1 = roundf_(r.x + r.w), y1 = roundf_(r.y + r.h);
    return (LayoutRect){x0, y0, x1 - x0, y1 - y0};
}

void layout_compute(const LayoutConfig *c, int ww, int wh, Layout *l) {
    if (c->single_screen) {
        LayoutConfig d = single_as_preset(c);
        layout_compute(&d, ww, wh, l);
        return;
    }
    memset(l, 0, sizeof *l);
    l->ww = ww > 1 ? ww : 1;
    l->wh = wh > 1 ? wh : 1;
    if (c->preset == LAYOUT_CUSTOM) {
        int vis0 = c->custom_visible[0], vis1 = c->custom_visible[1];
        if (!vis0 && !vis1) vis0 = vis1 = 1;
        l->visible[0] = vis0;
        l->visible[1] = vis1;
        for (int s = 0; s < 2; s++) {
            const LayoutRect *n = &c->custom[s];
            LayoutRect r = {n->x * l->ww, n->y * l->wh, n->w * l->ww, n->h * l->wh};
            if (c->keep_aspect) {  /* letterbox the native aspect inside the box */
                int nw, nh;
                layout_native_size(s, &nw, &nh);
                float sc = fminf_(r.w / nw, r.h / nh);
                float w = nw * sc, h = nh * sc;
                r = (LayoutRect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
            }
            l->r[s] = pixel_align(r);
        }
        l->order[1] = c->custom_front ? 1 : 0;
        l->order[0] = 1 - l->order[1];
        return;
    }
    LayoutRect r[2];
    float cw, ch;
    preset_canvas(c, r, l->visible, l->order, &cw, &ch);
    float s = fminf_(l->ww / cw, l->wh / ch);
    if (c->integer_scale && s >= 1.0f) s = floorf_(s);
    float ox = (l->ww - cw * s) / 2, oy = (l->wh - ch * s) / 2;
    for (int i = 0; i < 2; i++)
        l->r[i] = l->visible[i] ? pixel_align((LayoutRect){ox + r[i].x * s, oy + r[i].y * s, r[i].w * s, r[i].h * s})
                                : (LayoutRect){0, 0, 0, 0};
}

void layout_compute_single(const LayoutConfig *c, int screen, int ww, int wh, Layout *out) {
    /* the one-screen presets already letterbox a single screen: reuse them */
    LayoutConfig d = *c;
    d.preset = screen == LAYOUT_SCREEN_TOP ? LAYOUT_TOP_ONLY : LAYOUT_BOTTOM_ONLY;
    d.swap = 0;
    d.single_screen = 0;
    layout_compute(&d, ww, wh, out);
}

int layout_separate_visible(const LayoutConfig *c, int screen) {
    if (c->single_screen) return screen == (c->show_bottom ? LAYOUT_SCREEN_BOTTOM : LAYOUT_SCREEN_TOP);
    if (c->preset == LAYOUT_TOP_ONLY) return screen == LAYOUT_SCREEN_TOP;
    if (c->preset == LAYOUT_BOTTOM_ONLY) return screen == LAYOUT_SCREEN_BOTTOM;
    return 1;
}

int layout_windows_from_key(const char *k) {
    if (!k) return -1;
    if (!strcmp(k, "separate")) return 1;
    if (!strcmp(k, "joined")) return 0;
    return -1;
}

int layout_hit(const Layout *l, float x, float y) {
    for (int k = 1; k >= 0; k--) {
        int s = l->order[k];
        const LayoutRect *r = &l->r[s];
        if (l->visible[s] && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return s;
    }
    return -1;
}

int layout_to_screen(const Layout *l, int screen, float x, float y, int *sx, int *sy) {
    int nw, nh;
    layout_native_size(screen, &nw, &nh);
    if (screen == LAYOUT_SCREEN_BOTTOM) nw = LAYOUT_BOTTOM_GAME_W;   /* the game's image, not the frame */
    LayoutRect gr;
    layout_game_rect(l, screen, &gr);
    const LayoutRect *r = &gr;
    if (!l->visible[screen] || r->w <= 0 || r->h <= 0) {
        *sx = *sy = 0;
        return 0;
    }
    int inside = x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
    int px = (int)floorf_((x - r->x) * nw / r->w), py = (int)floorf_((y - r->y) * nh / r->h);
    *sx = px < 0 ? 0 : px >= nw ? nw - 1 : px;
    *sy = py < 0 ? 0 : py >= nh ? nh - 1 : py;
    return inside;
}

void layout_rect_ndc(const Layout *l, int s, float ndc[4]) {
    LayoutRect gr;
    layout_game_rect(l, s, &gr);
    const LayoutRect *r = &gr;
    ndc[0] = r->x / l->ww * 2 - 1;
    ndc[1] = 1 - (r->y + r->h) / l->wh * 2;
    ndc[2] = r->w / l->ww * 2;
    ndc[3] = r->h / l->wh * 2;
}

static LayoutRect normalize(const Layout *l, LayoutRect r) {
    return (LayoutRect){r.x / l->ww, r.y / l->wh, r.w / l->ww, r.h / l->wh};
}

void layout_make_custom(LayoutConfig *c, const Layout *l) {
    if (c->preset == LAYOUT_CUSTOM) return;
    Layout def;
    LayoutConfig d = *c;
    d.preset = LAYOUT_DEFAULT;
    layout_compute(&d, l->ww, l->wh, &def);
    for (int s = 0; s < 2; s++) {
        c->custom_visible[s] = l->visible[s];
        c->custom[s] = normalize(l, l->visible[s] ? l->r[s] : def.r[s]);
    }
    c->custom_front = l->order[1];
    c->preset = LAYOUT_CUSTOM;
}

/* ------------------------------------------------------------------ editing */
static void corner_pos(const LayoutRect *r, int corner, float *x, float *y) {
    *x = corner & 1 ? r->x + r->w : r->x;
    *y = corner & 2 ? r->y + r->h : r->y;
}

int layout_edit_hit(const Layout *l, float x, float y, float handle, int *screen, int *corner) {
    for (int k = 1; k >= 0; k--) {
        int s = l->order[k];
        if (!l->visible[s]) continue;
        const LayoutRect *r = &l->r[s];
        for (int cn = 0; cn < 4; cn++) {
            float cx, cy;
            corner_pos(r, cn, &cx, &cy);
            if (fabsf_(x - cx) <= handle && fabsf_(y - cy) <= handle) {
                *screen = s;
                *corner = cn;
                return LAYOUT_EDIT_RESIZE;
            }
        }
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            *screen = s;
            *corner = (x > r->x + r->w / 2 ? 1 : 0) | (y > r->y + r->h / 2 ? 2 : 0);  /* nearest */
            return LAYOUT_EDIT_MOVE;
        }
    }
    *screen = -1;
    *corner = 0;
    return LAYOUT_EDIT_NONE;
}

int layout_edit_begin(LayoutEdit *e, LayoutConfig *c, const Layout *l, float x, float y, float handle,
                      int force_resize) {
    int s, cn, m = layout_edit_hit(l, x, y, handle, &s, &cn);
    memset(e, 0, sizeof *e);
    e->guide_x = e->guide_y = -1;
    if (m == LAYOUT_EDIT_NONE) return 0;
    if (force_resize) m = LAYOUT_EDIT_RESIZE;
    layout_make_custom(c, l);
    e->mode = m;
    e->screen = s;
    e->corner = cn;
    e->mx0 = x;
    e->my0 = y;
    e->r0 = l->r[s];
    c->custom[s] = normalize(l, e->r0);  /* box = what is drawn (drops any letterbox slack) */
    c->custom_front = s;                 /* the screen being edited comes to the front */
    return 1;
}

/* snap value v to the closest candidate within SNAP_PX; returns the guide line or -1 */
typedef struct { float value, guide; } Cand;
static float snap1(float *v, const Cand *c, int n) {
    float best = SNAP_PX + 1, g = -1, nv = *v;
    for (int i = 0; i < n; i++) {
        float d = fabsf_(*v - c[i].value);
        if (d <= SNAP_PX && d < best) {
            best = d;
            nv = c[i].value;
            g = c[i].guide;
        }
    }
    *v = nv;
    return g;
}

void layout_edit_update(LayoutEdit *e, LayoutConfig *c, const Layout *l, float x, float y, int free_aspect,
                        int no_snap) {
    if (e->mode == LAYOUT_EDIT_NONE) return;
    int s = e->screen, o = 1 - s;
    int snap = c->snap && !no_snap;
    int keep = c->keep_aspect ^ (free_aspect ? 1 : 0);
    float ww = (float)l->ww, wh = (float)l->wh;
    const LayoutRect *orc = &l->r[o];
    int ov = l->visible[o];
    float dx = x - e->mx0, dy = y - e->my0;
    LayoutRect r = e->r0;
    e->guide_x = e->guide_y = -1;
    if (e->mode == LAYOUT_EDIT_MOVE) {
        r.x += dx;
        r.y += dy;
        if (snap) {
            Cand cx[8] = {{0, 0}, {ww - r.w, ww}, {(ww - r.w) / 2, ww / 2}};
            int n = 3;
            if (ov) {
                cx[n++] = (Cand){orc->x + orc->w, orc->x + orc->w};          /* right of the other */
                cx[n++] = (Cand){orc->x - r.w, orc->x};                      /* left of the other */
                cx[n++] = (Cand){orc->x, orc->x};                            /* left edges aligned */
                cx[n++] = (Cand){orc->x + orc->w - r.w, orc->x + orc->w};    /* right edges aligned */
                cx[n++] = (Cand){orc->x + (orc->w - r.w) / 2, orc->x + orc->w / 2}; /* centres */
            }
            e->guide_x = snap1(&r.x, cx, n);
            Cand cy[8] = {{0, 0}, {wh - r.h, wh}, {(wh - r.h) / 2, wh / 2}};
            n = 3;
            if (ov) {
                cy[n++] = (Cand){orc->y + orc->h, orc->y + orc->h};
                cy[n++] = (Cand){orc->y - r.h, orc->y};
                cy[n++] = (Cand){orc->y, orc->y};
                cy[n++] = (Cand){orc->y + orc->h - r.h, orc->y + orc->h};
                cy[n++] = (Cand){orc->y + (orc->h - r.h) / 2, orc->y + orc->h / 2};
            }
            e->guide_y = snap1(&r.y, cy, n);
        }
        /* keep at least MIN_PX of the screen inside the window */
        r.x = fmaxf_(MIN_PX - r.w, fminf_(r.x, ww - MIN_PX));
        r.y = fmaxf_(MIN_PX - r.h, fminf_(r.y, wh - MIN_PX));
    } else {
        int nw, nh;
        layout_native_size(s, &nw, &nh);
        int right = e->corner & 1, bottom = (e->corner & 2) != 0;
        float ax = right ? e->r0.x : e->r0.x + e->r0.w, ay = bottom ? e->r0.y : e->r0.y + e->r0.h;
        float px, py;
        corner_pos(&e->r0, e->corner, &px, &py);
        px += dx;
        py += dy;
        /* keep aspect: the axis that changed most (relative to the start size) drives the size,
         * and only that axis snaps */
        float rw = fmaxf_(MIN_PX, right ? px - ax : ax - px), rh = fmaxf_(MIN_PX, bottom ? py - ay : ay - py);
        int use_x = fabsf_(rw / e->r0.w - 1) >= fabsf_(rh / e->r0.h - 1);
        if (snap) {  /* the dragged corner snaps to window edges/centre and the other screen's edges */
            Cand cx[6] = {{0, 0}, {ww, ww}, {ww / 2, ww / 2}};
            Cand cy[6] = {{0, 0}, {wh, wh}, {wh / 2, wh / 2}};
            int n = 3;
            if (ov) {
                cx[n] = (Cand){orc->x, orc->x};
                cy[n++] = (Cand){orc->y, orc->y};
                cx[n] = (Cand){orc->x + orc->w, orc->x + orc->w};
                cy[n++] = (Cand){orc->y + orc->h, orc->y + orc->h};
            }
            if (!keep || use_x) e->guide_x = snap1(&px, cx, n);
            if (!keep || !use_x) e->guide_y = snap1(&py, cy, n);
        }
        float w = fmaxf_(MIN_PX, right ? px - ax : ax - px);
        float h = fmaxf_(MIN_PX, bottom ? py - ay : ay - py);
        if (keep) {
            float sc = use_x ? w / nw : h / nh;
            if (snap && e->guide_x < 0 && e->guide_y < 0) {  /* whole multiples of the native size */
                float k = roundf_(sc);
                if (k >= 1 && fabsf_(sc - k) * nw <= SNAP_PX) sc = k;
            }
            w = nw * sc;
            h = nh * sc;
        } else if (snap) {
            float kx = roundf_(w / nw), ky = roundf_(h / nh);
            if (e->guide_x < 0 && kx >= 1 && fabsf_(w - kx * nw) <= SNAP_PX) w = kx * nw;
            if (e->guide_y < 0 && ky >= 1 && fabsf_(h - ky * nh) <= SNAP_PX) h = ky * nh;
        }
        r = (LayoutRect){right ? ax : ax - w, bottom ? ay : ay - h, w, h};
    }
    c->custom[s] = normalize(l, r);
}

void layout_edit_end(LayoutEdit *e) {
    e->mode = LAYOUT_EDIT_NONE;
    e->guide_x = e->guide_y = -1;
}

static void set_rect_keep_centre(LayoutConfig *c, const Layout *l, int s, float w, float h) {
    const LayoutRect *r = &l->r[s];
    float cx = r->x + r->w / 2, cy = r->y + r->h / 2;
    LayoutRect n = {cx - w / 2, cy - h / 2, w, h};
    /* keep it inside the window when it fits */
    if (n.w <= l->ww) n.x = fmaxf_(0, fminf_(n.x, l->ww - n.w));
    if (n.h <= l->wh) n.y = fmaxf_(0, fminf_(n.y, l->wh - n.h));
    c->custom[s] = normalize(l, pixel_align(n));
}

void layout_edit_scale(LayoutConfig *c, const Layout *l, int s, float f) {
    layout_make_custom(c, l);
    const LayoutRect *r = &l->r[s];
    float w = fmaxf_(MIN_PX, r->w * f), h = fmaxf_(MIN_PX * r->h / (r->w > 0 ? r->w : 1), r->h * f);
    set_rect_keep_centre(c, l, s, w, h);
}

void layout_edit_set_size(LayoutConfig *c, const Layout *l, int s, float n) {
    layout_make_custom(c, l);
    int nw, nh;
    layout_native_size(s, &nw, &nh);
    if (n <= 0) n = fminf_((float)l->ww / nw, (float)l->wh / nh);
    set_rect_keep_centre(c, l, s, nw * n, nh * n);
    c->custom_visible[s] = 1;
}

/* ------------------------------------------------------------------ config file */
const char *layout_default_path(void) {
    static char buf[1024];
    const char *e = getenv("ALBW_LAYOUT_CFG");
    if (e && *e) return e;
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(buf, sizeof buf, "%s/albw-port/layout.cfg", x);
    else snprintf(buf, sizeof buf, "%s/.config/albw-port/layout.cfg", h ? h : ".");
    return buf;
}

void layout_fmt_float(char *buf, int n, float v) {
    long long q = (long long)((v < 0 ? -v : v) * 100000.0f + 0.5f);
    snprintf(buf, (size_t)n, "%s%lld.%05lld", v < 0 && q ? "-" : "", q / 100000, q % 100000);
}

float layout_parse_float(const char *s, const char **end) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    double v = 0, f = 0.1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',') {
        s++;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f *= 0.1; }
    }
    if (end) *end = s;
    return (float)(neg ? -v : v);
}

static int mkdirs_for(const char *path) {
    char d[1024];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(d, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    return 0;
}

int layout_load(LayoutConfig *c, const char *path) {
    layout_defaults(c);
    if (!path) path = layout_default_path();
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256], key[64], arg[64];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        int n = 0;
        if (sscanf(line, "%63s %63s %n", key, arg, &n) < 2) continue;
        const char *rest = line + n;
        int iv = atoi(arg);
        if (!strcmp(key, "windows")) { int w = layout_windows_from_key(arg); if (w >= 0) c->separate = w; }
        else if (!strcmp(key, "window_size") || !strcmp(key, "window_pos")) {
            int s = !strcmp(arg, "top") ? 0 : !strcmp(arg, "bottom") ? 1 : -1, a, b;
            if (s < 0 || sscanf(rest, "%d %d", &a, &b) != 2) continue;
            LayoutWindowGeom *g = &c->window[s];
            if (key[7] == 's') {
                if (a >= 32 && b >= 32 && a <= 16384 && b <= 16384) g->w = a, g->h = b, g->has_size = 1;
            } else {
                g->x = a, g->y = b, g->has_pos = 1;
            }
        }
        else if (!strcmp(key, "preset")) { int p = layout_preset_from_key(arg); if (p >= 0) c->preset = p; }
        else if (!strcmp(key, "swap")) c->swap = iv != 0;
        else if (!strcmp(key, "integer_scale")) c->integer_scale = iv != 0;
        else if (!strcmp(key, "linear_filter")) c->linear_filter = iv != 0;
        else if (!strcmp(key, "large_ratio")) c->large_ratio = clamp_ratio(layout_parse_float(arg, NULL));
        else if (!strcmp(key, "keep_aspect")) c->keep_aspect = iv != 0;
        else if (!strcmp(key, "snap")) c->snap = iv != 0;
        else if (!strcmp(key, "render_scale")) { if (iv >= 1 && iv <= 8) c->render_scale = iv; }
        else if (!strcmp(key, "menu_style")) c->menu_centered = !strcmp(arg, "centered");
        else if (!strcmp(key, "confirm_load")) c->confirm_load = iv != 0;
        else if (!strcmp(key, "hud_overlay")) c->hud_overlay = iv != 0;
        else if (!strcmp(key, "hd_textures")) c->hd_textures = iv != 0;
        else if (!strcmp(key, "hd_cache")) c->hd_cache = iv != 0;
        else if (!strcmp(key, "custom_front")) c->custom_front = !strcmp(arg, "top") ? 0 : 1;
        else if (!strcmp(key, "single_screen")) c->single_screen = iv != 0;
        else if (!strcmp(key, "widescreen")) c->widescreen = iv != 0;
        else if (!strcmp(key, "custom")) {
            int s = !strcmp(arg, "top") ? 0 : !strcmp(arg, "bottom") ? 1 : -1;
            if (s < 0) continue;
            float v[4];
            for (int i = 0; i < 4; i++) v[i] = layout_parse_float(rest, &rest);
            if (v[2] > 0.001f && v[3] > 0.001f) c->custom[s] = (LayoutRect){v[0], v[1], v[2], v[3]};
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest >= '0' && *rest <= '9') c->custom_visible[s] = atoi(rest) != 0;
        }
    }
    fclose(f);
    return 0;
}

int layout_save(const LayoutConfig *c, const char *path) {
    if (!path) path = layout_default_path();
    if (mkdirs_for(path)) return -1;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    char r[32];
    layout_fmt_float(r, sizeof r, clamp_ratio(c->large_ratio));
    fprintf(f, "# ALBW native port: screen layout (shared by the SDL and Qt frontends; see native/platform/layout.h)\n"
               "windows %s\npreset %s\nswap %d\ninteger_scale %d\nlinear_filter %d\nlarge_ratio %s\nkeep_aspect %d\nsnap %d\n"
               "render_scale %d\nmenu_style %s\ncustom_front %s\nsingle_screen %d\nwidescreen %d\nconfirm_load %d\nhud_overlay %d\nhd_textures %d\nhd_cache %d\n",
            c->separate ? "separate" : "joined", layout_preset_key(c->preset), c->swap, c->integer_scale, c->linear_filter, r, c->keep_aspect, c->snap,
            c->render_scale < 1 ? 1 : c->render_scale, c->menu_centered ? "centered" : "bar", c->custom_front ? "bottom" : "top", c->single_screen, c->widescreen,
            c->confirm_load, c->hud_overlay, c->hd_textures, c->hd_cache);
    for (int s = 0; s < 2; s++) {
        char v[4][32];
        layout_fmt_float(v[0], 32, c->custom[s].x);
        layout_fmt_float(v[1], 32, c->custom[s].y);
        layout_fmt_float(v[2], 32, c->custom[s].w);
        layout_fmt_float(v[3], 32, c->custom[s].h);
        fprintf(f, "custom %s %s %s %s %s %d\n", s ? "bottom" : "top", v[0], v[1], v[2], v[3], c->custom_visible[s]);
    }
    for (int s = 0; s < 2; s++) {
        const LayoutWindowGeom *g = &c->window[s];
        if (g->has_size) fprintf(f, "window_size %s %d %d\n", s ? "bottom" : "top", g->w, g->h);
        if (g->has_pos) fprintf(f, "window_pos %s %d %d\n", s ? "bottom" : "top", g->x, g->y);
    }
    if (fclose(f)) return -1;
    return rename(tmp, path) ? -1 : 0;
}
