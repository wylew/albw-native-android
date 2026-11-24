/* HUD overlay (included by platform.c after selector.c): the bottom screen's rupee counter and Y / B
 * item buttons drawn on the main screen while the bottom screen is not visible (single-screen
 * mode showing the top screen). Debug > HUD overlay, layout.cfg hud_overlay (default off: work in
 * progress).
 *
 * Pixels, not game data: the regions are copied from the bottom screen's image (screens[
 * SCREEN_BOTTOM], drawn by the game every frame even when not shown). They are only where the
 * code expects them on the world map page, so the page is checked every 10 frames from a few
 * pixels (the orange map scroll top left; not Gear / Items / a dungeon map: as selector.c), and
 * the regions are copied (glBlitFramebuffer) into a small cache texture only then; on other pages
 * the cache keeps the last good copy. Nothing is drawn until the first copy.
 * Regions in 3DS bottom-screen pixels (320 x 240, origin top left), measured on the map page:
 *   rupees: icon + 4 digits (254, 3, 65 x 24)   Y: badge + slot (256, 106, 60 x 58)
 *   B: badge + slot (280, 172, 39 x 34)
 * Drawn at the top screen's scale (its rect height / 240): rupees in the top right corner, B and Y
 * side by side in the bottom right, with soft edges (the wood background fades out). */
typedef struct { int x, y, w, h; } HudRect;
static const HudRect HUD_SRC[3] = {{254, 3, 65, 24}, {256, 106, 60, 58}, {280, 172, 39, 34}};
enum { HUD_RUPEE, HUD_Y, HUD_B };
#define HUD_ATLAS_W (65 + 60 + 39)
#define HUD_ATLAS_H 58

static GLuint hud_prog, hud_tex, hud_fbo;
static GLint hud_loc_rect, hud_loc_uvr, hud_loc_feather;
static int hud_scale_built, hud_have, hud_page_map;

static const char *HUD_VS =
    "#version 330 core\n"
    "uniform vec4 rect; uniform vec4 uvr; out vec2 uv; out vec2 lp;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); lp = p; uv = uvr.xy + p * uvr.zw;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *HUD_FS =
    "#version 330 core\n"
    "in vec2 uv; in vec2 lp; uniform sampler2D tex; uniform vec2 feather; out vec4 o;\n"
    "void main(){ vec2 e = min(lp, 1.0 - lp);\n"
    "  float a = smoothstep(0.0, feather.x, e.x) * smoothstep(0.0, feather.y, e.y);\n"
    "  o = vec4(texture(tex, uv).rgb, a); }\n";

static int hud_atlas_x(int i) { return i == HUD_RUPEE ? 0 : i == HUD_Y ? 65 : 65 + 60; }

/* (re)build the cache at the current render scale */
static int hud_init(int sc) {
    if (!hud_prog) {
        hud_prog = gl_program(HUD_VS, HUD_FS);
        hud_loc_rect = glGetUniformLocation(hud_prog, "rect");
        hud_loc_uvr = glGetUniformLocation(hud_prog, "uvr");
        hud_loc_feather = glGetUniformLocation(hud_prog, "feather");
        glUseProgram(hud_prog);
        glUniform1i(glGetUniformLocation(hud_prog, "tex"), 0);
    }
    if (hud_scale_built == sc && hud_tex) return 1;
    if (!hud_tex) glGenTextures(1, &hud_tex);
    glBindTexture(GL_TEXTURE_2D, hud_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, HUD_ATLAS_W * sc, HUD_ATLAS_H * sc, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!hud_fbo) glGenFramebuffers(1, &hud_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, hud_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hud_tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

static void hud_composite(const Layout *l) {
    if (!g_layout_cfg.hud_overlay || !l->visible[SCREEN_TOP] || l->visible[SCREEN_BOTTOM]) return;
    const Target *bt = &screens[SCREEN_BOTTOM];
    if (!bt->fbo || bt->w <= 0 || bt->h <= 0) return;
    int sc = bt->h / 240;
    if (sc < 1) sc = 1;
    if (!hud_init(sc)) return;
    static unsigned frame;
    if (frame++ % 10 == 0) hud_check_page(bt);
    if (hud_page_map) {   /* refresh the cache from the bottom screen (GL rows are bottom first) */
        glBindFramebuffer(0x8CA8 /* READ */, bt->fbo);
        glBindFramebuffer(0x8CA9 /* DRAW */, hud_fbo);
        for (int i = 0; i < 3; i++) {
            const HudRect *s = &HUD_SRC[i];
            int sx0 = s->x * bt->w / 320, sx1 = (s->x + s->w) * bt->w / 320;
            int sy0 = bt->h - (s->y + s->h) * bt->h / 240, sy1 = bt->h - s->y * bt->h / 240;
            int dx0 = hud_atlas_x(i) * sc, dy0 = (HUD_ATLAS_H - s->h) * sc;
            glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx0 + s->w * sc, dy0 + s->h * sc, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        hud_have = 1;
    }
    if (!hud_have) return;
    /* the top screen's rect in the window (pixels): 1 3DS pixel = rh / 240 window pixels */
    const LayoutRect *r = &l->r[SCREEN_TOP];
    float k = r->h / 240.0f, m = 5 * k;
    float pos[3][2] = {
        {r->x + r->w - m - HUD_SRC[HUD_RUPEE].w * k, r->y + m},                                                   /* top right */
        {r->x + r->w - m - HUD_SRC[HUD_Y].w * k, r->y + r->h - m - HUD_SRC[HUD_Y].h * k},                           /* bottom right */
        {r->x + r->w - m - (HUD_SRC[HUD_Y].w + 2) * k - HUD_SRC[HUD_B].w * k, r->y + r->h - m - HUD_SRC[HUD_B].h * k}, /* left of Y */
    };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, l->ww, l->wh);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(hud_prog);
    glBindVertexArray(blit_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hud_tex);
    for (int i = 0; i < 3; i++) {
        const HudRect *s = &HUD_SRC[i];
        float w = s->w * k, h = s->h * k;
        float rect[4] = {pos[i][0] / l->ww * 2 - 1, 1 - (pos[i][1] + h) / l->wh * 2, w / l->ww * 2, h / l->wh * 2};
        float uvr[4] = {(float)hud_atlas_x(i) / HUD_ATLAS_W, (float)(HUD_ATLAS_H - s->h) / HUD_ATLAS_H,
                        (float)s->w / HUD_ATLAS_W, (float)s->h / HUD_ATLAS_H};
        glUniform4fv(hud_loc_rect, 1, rect);
        glUniform4fv(hud_loc_uvr, 1, uvr);
        glUniform2f(hud_loc_feather, 6.0f / s->w, 6.0f / s->h);   /* ~6 3DS pixels of fade */
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
}
