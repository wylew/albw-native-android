/* HUD overlay (included by platform.c after selector.c): the bottom screen's item buttons drawn on
 * the main screen while the bottom screen is not visible (single-screen mode showing the top
 * screen). Debug > HUD overlay, layout.cfg hud_overlay (default off: work in progress).
 *
 * Pixels, not game data: the regions are copied from the bottom screen's image (screens[
 * SCREEN_BOTTOM], drawn by the game every frame even when not shown). They are only where the
 * code expects them on the world map page, so the page is checked every 10 frames from a few
 * pixels (the orange map scroll top left; not Gear / Items / a dungeon map: as selector.c), and
 * the regions are copied (glBlitFramebuffer) into a small cache texture only then; on other pages
 * the cache keeps the last good copy. Nothing is drawn until the first copy.
 *
 * Regions in 3DS bottom-screen pixels (320 x 240, origin top left), from the game's layouts
 * (Lyt_Game.arc: Gm_MainSystemItemB places the Y slot part Gm_SelectItemRootBtn at layout (129, -21),
 * the X slot at (129, 43); in the part the item (32 x 32) sits at (0, 3), the button badge (16 x 16)
 * at (-24, 27); screen = (160 + x, 120 - y)):
 *   Y item: 36 x 36 around the icon's centre (289, 138); Y badge (257, 106, 16 x 16)
 *   X item: 36 x 36 around (289, 74); X badge (257, 42, 16 x 16) (not drawn yet: HUD_X_ON)
 * Not shown: the rupee counter and the B button (the sword).
 *
 * Drawn at the top screen's scale (k = its rect height / 240), in top-screen pixels, stacked on the
 * left above the power meter (meter 8..32 x 150..222 on the 400 x 240 screen; the bottom right is
 * kept free for the game's button prompts): each button a circle of HUD_ITEM_D pixels, the item
 * icon (the copied disc, the slot's dark button behind it) in the middle HUD_RING, a light ring
 * with thin dark rims around it, the button's badge at its upper left as on the bottom screen.
 * Y is the lowest (its circle ends at HUD_STACK_BOTTOM), X above it.
 *
 * Fades with the scene: every frame the top screen is shrunk to 8 x 8 samples (a blit, on the GPU,
 * no read back) and the shader counts the samples that are not black; with few of them (a loading
 * screen: black but for the Triforce icon; the game's fades to black) the overlay fades out. */
typedef struct { int x, y, w, h; } HudRect;
enum { HUD_Y_ITEM, HUD_Y_BADGE, HUD_X_ITEM, HUD_X_BADGE, HUD_N };
static const HudRect HUD_SRC[HUD_N] = {{271, 120, 36, 36}, {257, 106, 16, 16}, {271, 56, 36, 36}, {257, 42, 16, 16}};
static const int HUD_ATLAS_X[HUD_N] = {0, 36, 52, 88};
#define HUD_ATLAS_W (36 + 16 + 36 + 16)
#define HUD_ATLAS_H 36
#define HUD_X_ON 0              /* the X button's item: when something is mapped to X */
#define HUD_ITEM_D 28.0f        /* item circle diameter, ring included (top-screen pixels) */
#define HUD_RING 0.8f           /* the copied 36 px disc fills this much of the diameter (icon at ~0.62) */
#define HUD_BADGE_D 11.0f       /* badge diameter */
#define HUD_STACK_X 22.0f       /* circle centre x: over the power meter (8..32) */
#define HUD_STACK_BOTTOM 146.0f /* lowest circle's bottom edge: just above the meter (top 150) */
#define HUD_STACK_GAP 6.0f      /* between two circles (room for the upper one's badge row) */
#define HUD_LUM_N 8             /* top screen samples per side for the fade */

static GLuint hud_prog, hud_tex, hud_fbo, hud_lum_tex, hud_lum_fbo;
static GLint hud_loc_rect, hud_loc_uvr, hud_loc_ring, hud_loc_mode;
static int hud_scale_built, hud_have, hud_page_map;

/* mode 1: item circle: the texture inside, from `ring` of the radius a light ring with dark rims;
 * mode 2: badge: the texture in a circle. Both times the scene fade: samples (of 64) with any
 * channel above 3 % : fully shown from 10, gone at 3 or fewer. */
static const char *HUD_VS =
    "#version 330 core\n"
    "uniform vec4 rect; uniform vec4 uvr; out vec2 uv; out vec2 lp;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); lp = p; uv = uvr.xy + p * uvr.zw;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *HUD_FS =
    "#version 330 core\n"
    "in vec2 uv; in vec2 lp; uniform sampler2D tex; uniform sampler2D lum; uniform float ring; uniform int mode;\n"
    "out vec4 o;\n"
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
        glUseProgram(hud_prog);
        glUniform1i(glGetUniformLocation(hud_prog, "tex"), 0);
        glUniform1i(glGetUniformLocation(hud_prog, "lum"), 1);
        if (!hud_target_tex(&hud_lum_fbo, &hud_lum_tex, HUD_LUM_N, HUD_LUM_N, GL_NEAREST)) return 0;
    }
    if (hud_scale_built == sc && hud_tex) return 1;
    int ok = hud_target_tex(&hud_fbo, &hud_tex, HUD_ATLAS_W * sc, HUD_ATLAS_H * sc, GL_LINEAR);
    hud_scale_built = sc;
    hud_have = 0;   /* new size: wait for a copy */
    return ok;
}

/* the world map page: orange scroll top left, not Gear (red at 300,60) / Items (tan at 160,120) */
static void hud_check_page(const Target *bt) {
    int w = bt->w, h = bt->h;
    uint8_t a[4] = {0}, b[4] = {0}, d[4] = {0};
    glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, bt->fbo);
    glReadPixels(300 * w / 320, h - 1 - 60 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
    glReadPixels(160 * w / 320, h - 1 - 120 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
    glReadPixels(5 * w / 320, h - 1 - 5 * h / 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glBindFramebuffer(0x8CA8, 0);
    int gear = a[0] > 90 && a[1] < 45, items = b[0] > 150 && b[1] > 120 && b[2] < 120;
    int scroll = d[0] > 200 && d[1] > 140 && d[2] < 110;
    hud_page_map = scroll && !gear && !items;
}

/* one atlas region at window pixels (x, y, w, h); the region fills the middle `fill` of the quad
 * (mode 1: the ring outside it covers what is sampled beyond the region) */
static void hud_draw(const Layout *l, int i, float x, float y, float w, float h, int mode, float fill) {
    const HudRect *s = &HUD_SRC[i];
    float rect[4] = {x / l->ww * 2 - 1, 1 - (y + h) / l->wh * 2, w / l->ww * 2, h / l->wh * 2};
    float uw = (float)s->w / HUD_ATLAS_W / fill, uh = (float)s->h / HUD_ATLAS_H / fill;
    float uvr[4] = {(HUD_ATLAS_X[i] + s->w * 0.5f) / HUD_ATLAS_W - uw / 2, (HUD_ATLAS_H - s->h * 0.5f) / HUD_ATLAS_H - uh / 2, uw, uh};
    glUniform4fv(hud_loc_rect, 1, rect);
    glUniform4fv(hud_loc_uvr, 1, uvr);
    glUniform1f(hud_loc_ring, HUD_RING);
    glUniform1i(hud_loc_mode, mode);
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
    if (frame++ % 10 == 0) hud_check_page(bt);
    if (hud_page_map) {   /* refresh the cache from the bottom screen (GL rows are bottom first) */
        glBindFramebuffer(0x8CA8 /* READ */, bt->fbo);
        glBindFramebuffer(0x8CA9 /* DRAW */, hud_fbo);
        for (int i = 0; i < HUD_N; i++) {
            const HudRect *s = &HUD_SRC[i];
            int sx0 = s->x * bt->w / 320, sx1 = (s->x + s->w) * bt->w / 320;
            int sy0 = bt->h - (s->y + s->h) * bt->h / 240, sy1 = bt->h - s->y * bt->h / 240;
            int dx0 = HUD_ATLAS_X[i] * sc, dy0 = (HUD_ATLAS_H - s->h) * sc;
            glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx0 + s->w * sc, dy0 + s->h * sc, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        hud_have = 1;
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
    const float d = HUD_ITEM_D * k, bd = HUD_BADGE_D * k, cx = r->x + HUD_STACK_X * k;
    float cy = r->y + (HUD_STACK_BOTTOM - HUD_ITEM_D / 2) * k;
    for (int b = 0; b < 2; b++) {
        int item = b == 0 ? HUD_Y_ITEM : HUD_X_ITEM;
        if (b == 1 && !HUD_X_ON) break;
        hud_draw(l, item, cx - d / 2, cy - d / 2, d, d, 1, HUD_RING);
        /* the badge at the upper left, its centre on the ring's 45-degree line just outside */
        float bx = cx - 0.47f * d, by = cy - 0.47f * d;
        hud_draw(l, item + 1, bx - bd / 2, by - bd / 2, bd, bd, 2, 1);
        cy -= (HUD_ITEM_D + HUD_STACK_GAP) * k;
    }
    glDisable(GL_BLEND);
}
