/* HUD overlay (included by platform.c after selector.c): the bottom screen's item buttons drawn on
 * the main screen while the bottom screen is not visible (single-screen mode showing the top
 * screen). Debug > HUD overlay, layout.cfg hud_overlay (default off: work in progress).
 *
 * Pixels, not game data: the regions are copied from the bottom screen's image (screens[
 * SCREEN_BOTTOM], drawn by the game every frame even when not shown). The Y slot sits in the same
 * place on every page that shows it (world map, dungeon map), so the slot itself is checked every
 * 10 frames from six pixels (hud_check_slot: its dark round button and the wood around it, held
 * still), and the item regions are copied (glBlitFramebuffer) into a small cache texture only while
 * it is there; elsewhere (loading, fades, menus) the cache keeps the last good copy. Nothing is
 * drawn until the first copy. ALBW_HUD_LOG=1 logs when the slot appears / goes.
 * The button badges never change, so they are drawn by the shader (no copy): the game's look, a
 * brown disc (~103, 69, 21) with the letter in the ring's light tan (reads better than the game's dark
 * letter over the wood); the dungeon map does
 * not even show the Y badge.
 *
 * Regions in 3DS bottom-screen pixels (320 x 240, origin top left), from the game's layouts
 * (Lyt_Game.arc: Gm_MainSystemItemB places the Y slot part Gm_SelectItemRootBtn at layout (129, -21),
 * the X slot at (129, 43); in the part the item (32 x 32) sits at (0, 3), the button badge (16 x 16)
 * at (-24, 27); screen = (160 + x, 120 - y)):
 *   Y item: 36 x 36 around the icon's centre (289, 138)
 *   X item: 36 x 36 around (289, 74) (only once the game has opened the X button: hud_check_slot)
 * Not shown: the rupee counter and the B button (the sword).
 *
 * Drawn at the top screen's scale (k = its rect height / 240), in top-screen pixels, stacked on the
 * left above the power meter, centred on it (meter 8..32 x 150..222 on the 400 x 240 screen, further
 * right by half the extra width in widescreen; the bottom right is
 * kept free for the game's button prompts): each button a circle of HUD_ITEM_D pixels, the item
 * icon (the copied disc, the slot's dark button behind it) in the middle HUD_RING, a light ring
 * with thin dark rims around it, the button's badge at its upper left as on the bottom screen.
 * Y is the lowest (its circle ends at HUD_STACK_BOTTOM), X above it.
 *
 * Fades with the scene: every frame the top screen is shrunk to 8 x 8 samples (a blit, on the GPU,
 * no read back) and the shader counts the samples that are not black; with few of them (a loading
 * screen: black but for the Triforce icon; the game's fades to black) the overlay fades out. */
typedef struct { int x, y, w, h; } HudRect;
enum { HUD_Y_ITEM, HUD_X_ITEM, HUD_N };
static const HudRect HUD_SRC[HUD_N] = {{271, 120, 36, 36}, {271, 56, 36, 36}};
static const int HUD_ATLAS_X[HUD_N] = {0, 36};
#define HUD_ATLAS_W (36 + 36)
#define HUD_ATLAS_H 36
#define HUD_ITEM_D 28.0f        /* item circle diameter, ring included (top-screen pixels) */
#define HUD_RING 0.8f           /* the copied 36 px disc fills this much of the diameter (icon at ~0.62) */
#define HUD_BADGE_D 11.0f       /* badge diameter */
#define HUD_STACK_X 20.0f       /* circle centre x: the power meter's centre (its bar 12..27 on the 400-wide screen) */
#define HUD_STACK_BOTTOM 146.0f /* lowest circle's bottom edge: just above the meter (top 150) */
#define HUD_STACK_GAP 6.0f      /* between two circles (room for the upper one's badge row) */
#define HUD_LUM_N 8             /* top screen samples per side for the fade */

static GLuint hud_prog, hud_tex, hud_fbo, hud_lum_tex, hud_lum_fbo;
static GLint hud_loc_rect, hud_loc_uvr, hud_loc_ring, hud_loc_mode, hud_loc_letter;
static int hud_scale_built, hud_have, hud_slot_seen, hud_have_x, hud_x_open;

/* mode 1: item circle: the texture inside, from `ring` of the radius a light ring with dark rims;
 * mode 2: badge: a brown disc with `letter` (0 Y, 1 X) in light tan strokes. Both times the scene fade: samples (of 64) with any
 * channel above 3 % : fully shown from 10, gone at 3 or fewer. */
static const char *HUD_VS =
    "#version 330 core\n"
    "uniform vec4 rect; uniform vec4 uvr; out vec2 uv; out vec2 lp;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); lp = p; uv = uvr.xy + p * uvr.zw;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *HUD_FS =
    "#version 330 core\n"
    "in vec2 uv; in vec2 lp; uniform sampler2D tex; uniform sampler2D lum; uniform float ring; uniform int mode;\n"
    "uniform int letter; out vec4 o;\n"
    "float seg(vec2 p, vec2 a, vec2 b) { vec2 pa = p - a, ba = b - a;\n"
    "  return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0)); }\n"
    "void main(){\n"
    "  float lit = 0.0;\n"
    "  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {\n"
    "    vec3 s = texelFetch(lum, ivec2(x, y), 0).rgb; lit += step(0.03, max(s.r, max(s.g, s.b))); }\n"
    "  float show = smoothstep(3.0, 10.0, lit);\n"
    "  vec3 c = texture(tex, uv).rgb;\n"
    "  float d = length(lp - 0.5) * 2.0, aa = max(fwidth(d), 1e-4);\n"
    "  float a = 1.0 - smoothstep(1.0 - aa, 1.0, d);\n"
    "  if (mode == 1) {\n"
    "    float in_ring = smoothstep(ring - aa, ring, d);\n"
    "    float rim = max(smoothstep(0.94 - aa, 0.94, d), 1.0 - smoothstep(ring + 0.03, ring + 0.03 + aa, d));\n"
    "    c = mix(c, mix(vec3(0.96, 0.92, 0.78), vec3(0.18, 0.12, 0.06), rim), in_ring);\n"
    "  }\n"
    "  if (mode == 2) {   /* q: -1..1, y down */\n"
    "    vec2 q = vec2(lp.x - 0.5, 0.5 - lp.y) * 2.0;\n"
    "    vec3 base = vec3(0.40, 0.27, 0.08);\n"
    "    c = mix(base * 1.12, base * 0.78, d * d);\n"
    "    float s = letter == 0\n"
    "      ? min(min(seg(q, vec2(-0.40, -0.50), vec2(0.0, 0.02)), seg(q, vec2(0.40, -0.50), vec2(0.0, 0.02))),\n"
    "            seg(q, vec2(0.0, 0.02), vec2(0.0, 0.55)))\n"
    "      : min(seg(q, vec2(-0.40, -0.45), vec2(0.40, 0.45)), seg(q, vec2(0.40, -0.45), vec2(-0.40, 0.45)));\n"
    "    float qa = max(fwidth(q.x), 1e-4);\n"
    "    c = mix(c, vec3(0.96, 0.92, 0.78), 1.0 - smoothstep(0.15 - qa, 0.15 + qa, s));   /* the ring's light tan */\n"
    "  }\n"
    "  o = vec4(c, a * show); }\n";

static GLuint hud_target_tex(GLuint *fbo, GLuint *tex, int w, int h, GLint filter) {
    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!*fbo) glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    GLuint ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

/* (re)build the cache at the current render scale */
static int hud_init(int sc) {
    if (!hud_prog) {
        hud_prog = gl_program(HUD_VS, HUD_FS);
        hud_loc_rect = glGetUniformLocation(hud_prog, "rect");
        hud_loc_uvr = glGetUniformLocation(hud_prog, "uvr");
        hud_loc_ring = glGetUniformLocation(hud_prog, "ring");
        hud_loc_mode = glGetUniformLocation(hud_prog, "mode");
        hud_loc_letter = glGetUniformLocation(hud_prog, "letter");
        glUseProgram(hud_prog);
        glUniform1i(glGetUniformLocation(hud_prog, "tex"), 0);
        glUniform1i(glGetUniformLocation(hud_prog, "lum"), 1);
        if (!hud_target_tex(&hud_lum_fbo, &hud_lum_tex, HUD_LUM_N, HUD_LUM_N, GL_NEAREST)) return 0;
    }
    if (hud_scale_built == sc && hud_tex) return 1;
    int ok = hud_target_tex(&hud_fbo, &hud_tex, HUD_ATLAS_W * sc, HUD_ATLAS_H * sc, GL_LINEAR);
    hud_scale_built = sc;
    hud_have = hud_have_x = 0;   /* new size: wait for a copy */
    return ok;
}

/* the item slots on the bottom screen: the item icon (32 x 32 around (289, 138) for Y, 64 higher
 * for X) on a dark round button (~45, 28, 10, radius ~22) on the lighter wood of the right-hand panel
 * (~79, 44, 8), on the world map and the dungeon map alike (the Y badge is not: the dungeon map hides
 * it and frames the slot). Four points on the button 20 px from the centre (outside any icon), two
 * on the wood around it. Copied only when all six held still since the last check: the game fades
 * the bottom screen on door transitions, and a copy taken then shows a dark icon until the next one.
 * X: the game opens the X button only later in the story; until then its place above Y is plain
 * wood. So whenever the Y slot reads cleanly the X place is checked too: a button there shows the X
 * circle (and copies its icon), plain wood hides it (a save from before the unlock, too). */
static const int16_t HUD_SLOT_PT[HUD_N][6][2] = {
    {{289, 158}, {269, 138}, {309, 138}, {289, 118}, {316, 138}, {289, 90}},   /* Y */
    {{289, 94}, {269, 74}, {309, 74}, {289, 54}, {316, 74}, {262, 74}},       /* X (the rupees above it) */
};
static int hud_brownish(const uint8_t *p) { return p[0] > p[1] + 6 && p[1] > p[2] + 4 && p[2] < 30; }
/* 1: the slot's button and wood are there and held still; *still: nothing changed since the last check */
static int hud_check_slot(const Target *bt, int slot, int *still_out) {
    int w = bt->w, h = bt->h;
    uint8_t px[6][4] = {{0}};
    glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, bt->fbo);
    for (int i = 0; i < 6; i++)
        glReadPixels((HUD_SLOT_PT[slot][i][0] * w + w / 2) / 320, h - 1 - (HUD_SLOT_PT[slot][i][1] * h + h / 2) / 240, 1, 1,
                     GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
    glBindFramebuffer(0x8CA8, 0);
    static uint8_t prev[HUD_N][6][4];
    static int last[HUD_N] = {-1, -1};
    int still = 1, button = 1, wood = 1, br = 0;
    for (int i = 0; i < 6; i++)
        for (int c = 0; c < 3; c++)
            if (px[i][c] > prev[slot][i][c] + 3 || prev[slot][i][c] > px[i][c] + 3) still = 0;
    memcpy(prev[slot], px, sizeof px);
    for (int i = 0; i < 4; i++) {
        button &= px[i][0] >= 28 && px[i][0] <= 75 && px[i][1] >= 14 && px[i][1] <= 50 && hud_brownish(px[i]);
        br += px[i][0];
    }
    for (int i = 4; i < 6; i++) wood &= hud_brownish(px[i]) && px[i][0] >= br / 4 + 18;
    int seen = button && wood && still;
    static int lg = -1;
    if (lg < 0) lg = getenv("ALBW_HUD_LOG") != NULL;
    if (lg && seen != last[slot])
        fprintf(stderr, "hud: %c slot %s (button %d,%d,%d wood %d,%d,%d%s%s%s)\n", slot ? 'X' : 'Y', seen ? "seen" : "not seen",
                px[0][0], px[0][1], px[0][2], px[4][0], px[4][1], px[4][2], button ? "" : ", no button", wood ? "" : ", no wood",
                still ? "" : ", changing");
    last[slot] = seen;
    *still_out = still;
    return seen;
}

/* one atlas region at window pixels (x, y, w, h); the region fills the middle `fill` of the quad
 * (mode 1: the ring outside it covers what is sampled beyond the region) */
static void hud_draw(const Layout *l, int i, float x, float y, float w, float h, int mode, float fill, int letter) {
    const HudRect *s = &HUD_SRC[i];
    float rect[4] = {x / l->ww * 2 - 1, 1 - (y + h) / l->wh * 2, w / l->ww * 2, h / l->wh * 2};
    float uw = (float)s->w / HUD_ATLAS_W / fill, uh = (float)s->h / HUD_ATLAS_H / fill;
    float uvr[4] = {(HUD_ATLAS_X[i] + s->w * 0.5f) / HUD_ATLAS_W - uw / 2, (HUD_ATLAS_H - s->h * 0.5f) / HUD_ATLAS_H - uh / 2, uw, uh};
    glUniform4fv(hud_loc_rect, 1, rect);
    glUniform4fv(hud_loc_uvr, 1, uvr);
    glUniform1f(hud_loc_ring, HUD_RING);
    glUniform1i(hud_loc_mode, mode);
    glUniform1i(hud_loc_letter, letter);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void hud_composite(const Layout *l) {
    if (!g_layout_cfg.hud_overlay || !l->visible[SCREEN_TOP] || l->visible[SCREEN_BOTTOM]) return;
    const Target *bt = &screens[SCREEN_BOTTOM], *tt = &screens[SCREEN_TOP];
    if (!bt->fbo || bt->w <= 0 || bt->h <= 0 || !tt->fbo || tt->w <= 0 || tt->h <= 0) return;
    int sc = bt->h / 240;
    if (sc < 1) sc = 1;
    if (!hud_init(sc)) return;
    static unsigned frame;
    int x_seen = 0;
    if (frame++ % 10 == 0) {
        int sy, sx;
        hud_slot_seen = hud_check_slot(bt, HUD_Y_ITEM, &sy);
        x_seen = hud_check_slot(bt, HUD_X_ITEM, &sx);
        /* X open or not: decided only on a clean read of the page (Y there, nothing moving) */
        if (hud_slot_seen && sx) hud_x_open = x_seen;
    }
    if (hud_slot_seen) {   /* refresh the cache from the bottom screen (GL rows are bottom first) */
        glBindFramebuffer(0x8CA8 /* READ */, bt->fbo);
        glBindFramebuffer(0x8CA9 /* DRAW */, hud_fbo);
        for (int i = 0; i < HUD_N; i++) {
            if (i == HUD_X_ITEM && !x_seen) continue;
            const HudRect *s = &HUD_SRC[i];
            int sx0 = s->x * bt->w / 320, sx1 = (s->x + s->w) * bt->w / 320;
            int sy0 = bt->h - (s->y + s->h) * bt->h / 240, sy1 = bt->h - s->y * bt->h / 240;
            int dx0 = HUD_ATLAS_X[i] * sc, dy0 = (HUD_ATLAS_H - s->h) * sc;
            glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx0 + s->w * sc, dy0 + s->h * sc, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        hud_have = 1;
        hud_have_x |= x_seen;
    }
    if (!hud_have) { glBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
    /* the scene samples for the fade (the top screen as the game drew it, before the overlay) */
    glBindFramebuffer(0x8CA8 /* READ */, tt->fbo);
    glBindFramebuffer(0x8CA9 /* DRAW */, hud_lum_fbo);
    glBlitFramebuffer(0, 0, tt->w, tt->h, 0, 0, HUD_LUM_N, HUD_LUM_N, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* the top screen's rect in the window (pixels): 1 3DS pixel = rh / 240 window pixels */
    const LayoutRect *r = &l->r[SCREEN_TOP];
    float k = r->h / 240.0f;
    glViewport(0, 0, l->ww, l->wh);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(hud_prog);
    glBindVertexArray(blit_vao);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, hud_lum_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hud_tex);
    /* item buttons: Y lowest, X above it */
    /* widescreen: the game lays its 2D HUD out for 400 columns, centred (the meter moves right by
     * half the extra width: measured 33.7 at 427), so the stack follows */
    const float d = HUD_ITEM_D * k, bd = HUD_BADGE_D * k, cx = r->x + (HUD_STACK_X + (LAYOUT_TOP_W - 400) * 0.5f) * k;
    float cy = r->y + (HUD_STACK_BOTTOM - HUD_ITEM_D / 2) * k;
    for (int b = 0; b < 2; b++) {
        int item = b == 0 ? HUD_Y_ITEM : HUD_X_ITEM;
        if (b == 1 && !(hud_x_open && hud_have_x)) break;   /* X: only once the game has opened it */
        hud_draw(l, item, cx - d / 2, cy - d / 2, d, d, 1, HUD_RING, 0);
        /* the badge at the upper left, its centre on the ring's 45-degree line just outside */
        float bx = cx - 0.47f * d, by = cy - 0.47f * d;
        hud_draw(l, item, bx - bd / 2, by - bd / 2, bd, bd, 2, 1, b);   /* letter 0 Y, 1 X */
        cy -= (HUD_ITEM_D + HUD_STACK_GAP) * k;
    }
    glDisable(GL_BLEND);
}
