/* Single-screen bottom-screen selector (included by platform.c; BOOT_LOG 160ab).
 *
 * In single-screen mode the bottom screen (map, Items, Gear) is shown in place of the top one and
 * there is no mouse on a controller: the D-pad (or a circle-pad flick) moves a selector between the
 * page's touch targets and A taps the one under it. The selector is the user's grid-selector
 * graphic (native/platform/assets/grid-selector.png, embedded by selector_img.h) drawn as an
 * overlay on the bottom screen's image, not a pointer. The page is recognised from pixels of the
 * bottom screen: Gear (red frame), Items (tan grid), a dungeon map (blue-grey frame), the world map
 * (orange scroll top left; with the bell button bottom left once Link has the bell: its yellow
 * icon), else another page with
 * controls of its own (the bell's warp menu), where the D-pad and A go to the game. Targets are in 3DS bottom
 * screen pixels (320 x 240, origin top left), measured on the game's pages. */
#include "selector_img.h"

/* the platform blit with a texture sub-rectangle (uvr = u0, v0, du, dv) */
static const char *SEL_VS =
    GLSL_HDR
    "uniform vec4 rect; uniform vec4 uvr; out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = uvr.xy + p * uvr.zw;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";

typedef struct { int16_t x, y, w, h; uint8_t native; } SelTarget;   /* centre, size; native: the game draws its own cursor there */

static const SelTarget SEL_MAP[] = {
    {22, 20, 40, 40},                        /* + (pins) */
    {22, 142, 36, 40},                       /* pin counter */
    {288, 135, 46, 46},                      /* Y item slot */
    {80, 226, 156, 26}, {240, 226, 156, 26}, /* Gear / Items tabs */
};
/* the map page once Link has the bell (Irene's weather-vane warp): a bell button bottom left, the
   Gear / Items tabs move right and get narrower */
static const SelTarget SEL_MAP_BELL[] = {
    {22, 20, 40, 40},                        /* + (pins) */
    {22, 142, 36, 40},                       /* pin counter */
    {288, 135, 46, 46},                      /* Y item slot */
    {28, 226, 52, 26},                       /* bell */
    {124, 226, 124, 26}, {255, 226, 128, 26}, /* Gear / Items tabs */
};
/* a dungeon's map: floor buttons on the left instead of the pins; wide Gear / Items tabs */
static const SelTarget SEL_DUNGEON[] = {
    {35, 54, 62, 34}, {35, 97, 62, 34}, {35, 139, 62, 34},   /* floors */
    {288, 138, 46, 46},                                       /* Y item slot */
    {80, 226, 156, 26}, {240, 226, 156, 26},                  /* Gear / Items tabs */
};
static const SelTarget SEL_ITEMS[] = {
    {33, 27, 46, 44, 1}, {80, 27, 46, 44, 1}, {127, 27, 46, 44, 1}, {174, 27, 46, 44, 1}, {221, 27, 46, 44, 1},
    {33, 72, 46, 44, 1}, {80, 72, 46, 44, 1}, {127, 72, 46, 44, 1}, {174, 72, 46, 44, 1}, {221, 72, 46, 44, 1},
    {33, 117, 46, 44, 1}, {80, 117, 46, 44, 1}, {127, 117, 46, 44, 1}, {174, 117, 46, 44, 1}, {221, 117, 46, 44, 1},
    {33, 162, 46, 44, 1}, {80, 162, 46, 44, 1}, {127, 162, 46, 44, 1}, {174, 162, 46, 44, 1}, {221, 162, 46, 44, 1},
    {285, 135, 50, 50},                      /* Y item slot */
    {160, 226, 300, 26},                     /* Close */
};
static const SelTarget SEL_GEAR[] = {
    {37, 40, 40, 40}, {82, 40, 40, 40}, {125, 40, 40, 40},                       /* sword, tunic, - */
    {37, 92, 40, 40}, {80, 92, 40, 40}, {122, 92, 40, 40},                       /* rings */
    {37, 133, 40, 40}, {80, 133, 40, 40}, {122, 133, 40, 40},
    {232, 26, 30, 32}, {185, 40, 30, 32}, {285, 40, 30, 32}, {232, 63, 30, 30},  /* pendants etc. */
    {169, 95, 28, 32}, {208, 101, 30, 30}, {256, 101, 30, 30}, {298, 95, 28, 32},
    {202, 140, 30, 32}, {262, 140, 30, 32},
    {33, 178, 40, 36},                                                            /* heart */
    {85, 178, 40, 24}, {127, 178, 40, 24}, {165, 178, 40, 24}, {205, 178, 40, 24}, {245, 178, 40, 24}, {285, 178, 40, 24},
    {160, 226, 300, 26},                                                          /* Close */
};
enum { SEL_PAGE_MAP, SEL_PAGE_ITEMS, SEL_PAGE_GEAR, SEL_PAGE_MAP_BELL, SEL_PAGE_DUNGEON,
       SEL_PAGE_OTHER /* a page with controls of its own (the bell's warp menu): input passes through */ };
static const struct { const SelTarget *t; int n; } SEL_PAGES[5] = {
    {SEL_MAP, (int)(sizeof SEL_MAP / sizeof *SEL_MAP)},
    {SEL_ITEMS, (int)(sizeof SEL_ITEMS / sizeof *SEL_ITEMS)},
    {SEL_GEAR, (int)(sizeof SEL_GEAR / sizeof *SEL_GEAR)},
    {SEL_MAP_BELL, (int)(sizeof SEL_MAP_BELL / sizeof *SEL_MAP_BELL)},
    {SEL_DUNGEON, (int)(sizeof SEL_DUNGEON / sizeof *SEL_DUNGEON)},
};

static int g_sel_page, g_sel_idx, g_sel_on;
static int g_sel_x = 160, g_sel_y = 120;   /* last selected centre (kept across pages) */
static int g_sel_touch;                    /* A held: touch at the target */
static int g_sel_autotap;                  /* frames of a tap when moving onto a native-cursor target */
static uint32_t g_sel_prev;
static GLuint g_sel_tex;

static int sel_active(void) { return g_layout_cfg.single_screen && g_layout_cfg.show_bottom && !g_ss_front; }

static int sel_nearest(int page, int x, int y) {
    const SelTarget *t = SEL_PAGES[page].t;
    int best = 0;
    long bd = -1;
    for (int i = 0; i < SEL_PAGES[page].n; i++) {
        long dx = t[i].x - x, dy = t[i].y - y, d = dx * dx + dy * dy;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

/* the target nearest in direction (dx, dy): ahead along that axis, the side offset counted double */
static int sel_step(int page, int cur, int dx, int dy) {
    const SelTarget *t = SEL_PAGES[page].t;
    int best = cur;
    long bs = -1;
    for (int i = 0; i < SEL_PAGES[page].n; i++) {
        if (i == cur) continue;
        long ax = t[i].x - t[cur].x, ay = t[i].y - t[cur].y;
        long along = ax * dx + ay * dy, side = dx ? ay : ax;
        if (along <= 4 || along * 17 < (side < 0 ? -side : side) * 10) continue;   /* ~60 degree cone */
        long s = along + 2 * (side < 0 ? -side : side);
        if (bs < 0 || s < bs) { bs = s; best = i; }
    }
    return best;
}

/* plat_poll: D-pad / circle-pad flicks move, A taps (both taken from the game while active) */
static void sel_input(PlatInput *pin) {
    if (!sel_active()) {
        g_sel_on = 0;
        g_sel_touch = 0;
        g_sel_prev = pin->held;
        return;
    }
    if (g_sel_page == SEL_PAGE_OTHER) {   /* the game's own menu (bell warp list): D-pad and A are its */
        g_sel_on = 0;
        g_sel_touch = 0;
        g_sel_prev = pin->held;
        return;
    }
    if (!g_sel_on) { g_sel_on = 1; g_sel_idx = sel_nearest(g_sel_page, g_sel_x, g_sel_y); }
    const uint32_t up = KEY_DUP | KEY_CPAD_UP, down = KEY_DDOWN | KEY_CPAD_DOWN;
    const uint32_t left = KEY_DLEFT | KEY_CPAD_LEFT, right = KEY_DRIGHT | KEY_CPAD_RIGHT;
    uint32_t held = pin->held, edge = held & ~g_sel_prev;
    g_sel_prev = held;
    int n = SEL_PAGES[g_sel_page].n;
    if (g_sel_idx >= n) g_sel_idx = n - 1;
    int dx = (edge & right) ? 1 : (edge & left) ? -1 : 0;
    int dy = (edge & down) ? 1 : (edge & up) ? -1 : 0;
    int was = g_sel_idx;
    if (dx) g_sel_idx = sel_step(g_sel_page, g_sel_idx, dx, 0);
    if (dy) g_sel_idx = sel_step(g_sel_page, g_sel_idx, 0, dy);
    /* the game draws its own cursor on these (Items grid): select by a short tap so it follows */
    if (g_sel_idx != was && SEL_PAGES[g_sel_page].t[g_sel_idx].native) g_sel_autotap = 4;
    const SelTarget *t = &SEL_PAGES[g_sel_page].t[g_sel_idx];
    g_sel_x = t->x;
    g_sel_y = t->y;
    {   /* ALBW_SEL_TRACE=1: page / target changes and taps */
        static int lp = -1, li = -1, la;
        int tap = (held & KEY_A) != 0;
        if ((lp != g_sel_page || li != g_sel_idx || la != tap) && getenv("ALBW_SEL_TRACE"))
            fprintf(stderr, "selector: page %s target %d at %d,%d%s\n", g_sel_page == SEL_PAGE_GEAR ? "gear" : g_sel_page == SEL_PAGE_ITEMS ? "items" : g_sel_page == SEL_PAGE_DUNGEON ? "dungeon" : g_sel_page == SEL_PAGE_MAP_BELL ? "map+bell" : "map",
                    g_sel_idx, t->x, t->y, tap ? " TAP" : "");
        lp = g_sel_page; li = g_sel_idx; la = tap;
    }
    g_sel_touch = (held & KEY_A) != 0;
    if (g_sel_autotap > 0) g_sel_autotap--;
    if (g_sel_touch || g_sel_autotap > 0) {
        pin->touch = 1;
        pin->touch_x = t->x;
        pin->touch_y = t->y;
        held |= KEY_TOUCH;
    }
    pin->held = held & ~(up | down | left | right | KEY_A);
    pin->cpad_x = pin->cpad_y = 0;
}

/* plat_composite (the screens were just drawn into the window): recognise the page now and then
 * from the bottom screen's target, draw the selector over the bottom screen's place in the window.
 * Not in render_scanout / plat_end_frame: save states resume inside those (their code must stay). */
static __attribute__((noinline)) void sel_composite(const Layout *l) {
    if (!sel_active() || !l->visible[SCREEN_BOTTOM]) return;
    const Target *bt = &screens[SCREEN_BOTTOM];
    int w = bt->w, h = bt->h;
    if (w <= 0 || h <= 0) return;
    static unsigned frame;
    if (frame++ % 10 == 0) {
        glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, bt->fbo);
        uint8_t a[4] = {0}, b[4] = {0}, c[4] = {0}, d[4] = {0};   /* d: top-left corner */
        glReadPixels(300 * w / 320, h - 1 - 60 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
        glReadPixels(160 * w / 320, h - 1 - 120 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
        glReadPixels(28 * w / 320, h - 1 - 226 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);   /* bell icon: pure yellow */
        glReadPixels(5 * w / 320, h - 1 - 5 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, d);   /* map frame colour */
        glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
        int page = (a[0] > 90 && a[1] < 45) ? SEL_PAGE_GEAR
                 : (b[0] > 150 && b[1] > 120 && b[2] < 120) ? SEL_PAGE_ITEMS
                 : (d[2] > 140 && d[2] > d[0] + 10) ? SEL_PAGE_DUNGEON          /* blue-grey dungeon frame */
                 : !(d[0] > 200 && d[1] > 140 && d[2] < 110) ? SEL_PAGE_OTHER   /* no orange map scroll: warp menu */
                 : (c[0] > 230 && c[1] > 190 && c[2] < 60) ? SEL_PAGE_MAP_BELL : SEL_PAGE_MAP;
        if (page != g_sel_page && page == SEL_PAGE_OTHER) {
            g_sel_page = page;
        } else if (page != g_sel_page) {
            g_sel_page = page;
            g_sel_idx = sel_nearest(page, g_sel_x, g_sel_y);
            const SelTarget *t = &SEL_PAGES[page].t[g_sel_idx];
            g_sel_x = t->x;
            g_sel_y = t->y;
        }
    }
    if (g_sel_page == SEL_PAGE_OTHER) return;   /* the game's own menu: no selector */
    if (!g_sel_tex) {
        glGenTextures(1, &g_sel_tex);
        glBindTexture(GL_TEXTURE_2D, g_sel_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SELECTOR_W, SELECTOR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, SELECTOR_RGBA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    const SelTarget *t = &SEL_PAGES[g_sel_page].t[g_sel_idx < SEL_PAGES[g_sel_page].n ? g_sel_idx : 0];
    if (t->native) {
        /* the game's bracket marks the selected Items cell (only cells holding an item): its
         * bottom-right corner is light just outside the cell (x + 27, y + 19). Shown -> no
         * selector of ours; an empty cell -> ours */
        uint8_t c[4] = {0};
        glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, bt->fbo);
        glReadPixels((t->x + 27) * w / 320, h - 1 - (t->y + 19) * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
        if (c[0] > 210 && c[2] > 155) return;
    }
    static GLuint prog;
    if (!prog) prog = gl_program(SEL_VS, BLIT_FS);
    float pad = g_sel_touch ? 2.0f : 5.0f;   /* closes in a little while A is held */
    float x0 = t->x - t->w * 0.5f - pad, y0 = t->y - t->h * 0.5f - pad;
    float rw = t->w + 2 * pad, rh = t->h + 2 * pad;
    /* four corners of the graphic at a fixed size (not stretched with the target's shape) */
    float cs = 14.0f;
    if (cs > rw * 0.5f) cs = rw * 0.5f;
    if (cs > rh * 0.5f) cs = rh * 0.5f;
    float sr[4];
    layout_rect_ndc(l, SCREEN_BOTTOM, sr);   /* x, y (bottom left), w, h of the bottom screen */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    GLint lr = glGetUniformLocation(prog, "rect"), lu = glGetUniformLocation(prog, "uvr");
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_sel_tex);
    glBindVertexArray(blit_vao);
    for (int qy = 0; qy < 2; qy++)          /* 0 bottom, 1 top (window up) */
        for (int qx = 0; qx < 2; qx++) {
            float px = qx ? x0 + rw - cs : x0;                 /* 3DS pixels, y down */
            float py = qy ? y0 : y0 + rh - cs;
            const float r4[4] = {sr[0] + px / 320.0f * sr[2], sr[1] + (1.0f - (py + cs) / 240.0f) * sr[3],
                                 cs / 320.0f * sr[2], cs / 240.0f * sr[3]};
            /* image quadrant (texture rows top first): u from qx, v from bottom (1) or top (0.5) */
            const float uv[4] = {qx * 0.5f, qy ? 0.5f : 1.0f, 0.5f, -0.5f};
            glUniform4fv(lr, 1, r4);
            glUniform4fv(lu, 1, uv);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    glDisable(GL_BLEND);
}
