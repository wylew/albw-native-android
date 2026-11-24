/* Save-state user interface (#included by platform.c after menu.c): on-screen messages, slot
 * thumbnails, controller shortcuts.
 *
 * Toasts: a panel at the top of the game for ~2.5 s after a save, load, slot change or error, with
 * the slot's picture. The outcome comes from savestate.c (albw_ss_note); the title bar still gets
 * it too (desktop).
 * Thumbnails: when a save succeeds, the top screen (screens[SCREEN_TOP], the last frame) is
 * shrunk to 240x144 and written next to the state as <slotN.state>.thumb ("ALBWTHM1", u32 width,
 * u32 height, RGBA rows top first). Shown in toasts and in the Esc menu's States tab.
 * Controller (any platform): hold L3 (left stick click; the 3DS circle pad has none, so nothing
 * of the game is lost) and press L = save, R = load, D-pad left / right = slot. While L3 is held
 * L, R and the D-pad do not reach the game (they used to: L3+L also pressed L in the game).
 * Keyboard: F6 save, F7 load, F8 / Shift+F8 slot.
 * Confirm load (States > Confirm load, layout.cfg confirm_load, default on): the first quick
 * load shows the slot, its save time and picture and asks for the same press again within 3 s;
 * the menu's Load item loads at once (it is already a deliberate choice). */
#define THUMB_W 240
#define THUMB_H 144
#define TOAST_NS 2500000000ull
#define CONFIRM_NS 3000000000ull

__attribute__((weak)) void albw_ss_slot_path(int slot, char *buf, size_t n);

static struct {
    char line1[96], line2[96];
    int slot;           /* picture of this slot, -1 none */
    uint64_t t0, len;
} g_toast;
static GLuint g_thumb_tex[SS_SLOTS_UI];
static int g_thumb_state[SS_SLOTS_UI];   /* 0 not looked up, 1 texture, -1 none */
static int g_thumb_pending = -1;          /* slot whose save was requested: picture after it succeeds */
static int g_confirm_slot = -1;
static uint64_t g_confirm_ns;

static void ssui_toast(int slot, uint64_t len, const char *l1, const char *l2) {
    snprintf(g_toast.line1, sizeof g_toast.line1, "%s", l1 ? l1 : "");
    snprintf(g_toast.line2, sizeof g_toast.line2, "%s", l2 ? l2 : "");
    g_toast.slot = slot;
    g_toast.t0 = plat_ticks_ns();
    g_toast.len = len;
}

static int thumb_file(int slot, char *buf, size_t n) {
    if (!albw_ss_slot_path || slot < 0 || slot >= SS_SLOTS_UI) return 0;
    char p[4096];
    albw_ss_slot_path(slot, p, sizeof p);
    snprintf(buf, n, "%s.thumb", p);
    return 1;
}

/* the last frame's top screen, shrunk (box filter) and written next to the slot's state */
static void thumb_save(int slot) {
    char path[4200];
    Target *t = &screens[SCREEN_TOP];
    if (!thumb_file(slot, path, sizeof path) || !t->fbo || t->w <= 0 || t->h <= 0) return;
    uint8_t *src = malloc((size_t)t->w * t->h * 4), *dst = malloc(THUMB_W * THUMB_H * 4);
    if (!src || !dst) { free(src); free(dst); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, src);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < THUMB_H; y++) {
        int sy0 = y * t->h / THUMB_H, sy1 = (y + 1) * t->h / THUMB_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < THUMB_W; x++) {
            int sx0 = x * t->w / THUMB_W, sx1 = (x + 1) * t->w / THUMB_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned acc[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t *p = src + ((size_t)sy * t->w + sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++;
                }
            uint8_t *q = dst + ((size_t)(THUMB_H - 1 - y) * THUMB_W + x) * 4;   /* GL rows are bottom first */
            q[0] = (uint8_t)(acc[0] / n); q[1] = (uint8_t)(acc[1] / n); q[2] = (uint8_t)(acc[2] / n); q[3] = 255;
        }
    }
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        uint32_t wh[2] = {THUMB_W, THUMB_H};
        int ok = fwrite("ALBWTHM1", 1, 8, f) == 8 && fwrite(wh, 4, 2, f) == 2 &&
                 fwrite(dst, 1, THUMB_W * THUMB_H * 4, f) == THUMB_W * THUMB_H * 4;
        ok &= fclose(f) == 0;
        if (ok) rename(tmp, path); else remove(tmp);
    }
    free(src);
    free(dst);
    g_thumb_state[slot] = 0;   /* reload */
}

static GLuint ssui_thumb(int slot) {
    if (slot < 0 || slot >= SS_SLOTS_UI) return 0;
    if (g_thumb_state[slot] == 1) return g_thumb_tex[slot];
    if (g_thumb_state[slot] == -1) return 0;
    g_thumb_state[slot] = -1;
    char path[4200];
    if (!thumb_file(slot, path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    uint32_t wh[2];
    uint8_t *px = NULL;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "ALBWTHM1", 8) && fread(wh, 4, 2, f) == 2 &&
        wh[0] && wh[1] && wh[0] <= 1024 && wh[1] <= 1024 && (px = malloc((size_t)wh[0] * wh[1] * 4)) &&
        fread(px, 1, (size_t)wh[0] * wh[1] * 4, f) == (size_t)wh[0] * wh[1] * 4) {
        if (!g_thumb_tex[slot]) glGenTextures(1, &g_thumb_tex[slot]);
        glBindTexture(GL_TEXTURE_2D, g_thumb_tex[slot]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_thumb_state[slot] = 1;
    }
    free(px);
    fclose(f);
    return g_thumb_state[slot] == 1 ? g_thumb_tex[slot] : 0;
}

static void ssui_save_requested(int slot) { g_thumb_pending = slot; }

static void slot_when(int slot, char *buf, size_t n) {
    if (!albw_ss_slot_info || !albw_ss_slot_info(slot, buf, n)) snprintf(buf, n, "empty");
}

/* ---- actions shared by the controller, the keyboard and the menu */
static void ssui_save(int slot) {
    if (!albw_ss_request) return;
    g_thumb_pending = slot;
    albw_ss_request(1, slot);
}
static void ssui_quick_load(int slot, const char *again) {
    if (!albw_ss_request) return;
    char when[64], l1[96], l2[96];
    slot_when(slot, when, sizeof when);
    if (!strcmp(when, "empty")) {
        snprintf(l1, sizeof l1, "Slot %d is empty", slot + 1);
        ssui_toast(slot, TOAST_NS, l1, "Save with L3 + L first");
        return;
    }
    uint64_t now = plat_ticks_ns();
    if (g_layout_cfg.confirm_load && !(g_confirm_slot == slot && now - g_confirm_ns < CONFIRM_NS)) {
        g_confirm_slot = slot;
        g_confirm_ns = now;
        snprintf(l1, sizeof l1, "Load slot %d?  (saved %s)", slot + 1, when);
        snprintf(l2, sizeof l2, "Press %s again to load", again);
        ssui_toast(slot, CONFIRM_NS, l1, l2);
        return;
    }
    g_confirm_slot = -1;
    albw_ss_request(2, slot);
}
static void ssui_slot_step(int d) {
    g_ss_slot = (g_ss_slot + (d < 0 ? SS_SLOTS_UI - 1 : 1)) % SS_SLOTS_UI;
    g_confirm_slot = -1;
    char when[64], l1[64];
    slot_when(g_ss_slot, when, sizeof when);
    snprintf(l1, sizeof l1, "Slot %d", g_ss_slot + 1);
    ssui_toast(g_ss_slot, TOAST_NS, l1, when);
    fprintf(stderr, "savestate: slot %d\n", g_ss_slot + 1);
}

/* L3 on any connected controller (read directly: the input layer has no binding for it) */
static int ssui_l3_held(void) {
    int n = 0, l3 = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK)) l3 = 1;
    }
    SDL_free(ids);
    return l3;
}

/* once per frame (plat_end_frame): L3 + L / R / D-pad left / right */
static __attribute__((noinline)) void ssui_pad_shortcuts(void) {
    static int prev[4];
    int now[4] = {0, 0, 0, 0}, l3 = 0, n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        l3 |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        now[0] |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        now[1] |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        now[2] |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        now[3] |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    }
    SDL_free(ids);
    if (l3) {
        if (now[0] && !prev[0]) { fprintf(stderr, "savestate: save to slot %d (L3+L)\n", g_ss_slot + 1); ssui_save(g_ss_slot); }
        if (now[1] && !prev[1]) { fprintf(stderr, "savestate: load slot %d (L3+R)\n", g_ss_slot + 1); ssui_quick_load(g_ss_slot, "L3 + R"); }
        if (now[2] && !prev[2]) ssui_slot_step(-1);
        if (now[3] && !prev[3]) ssui_slot_step(1);
    }
    memcpy(prev, now, sizeof prev);
}

/* after albw_ss_frame_point: a new outcome becomes a toast; a successful save gets its picture */
static __attribute__((noinline)) void ssui_after_frame_point(void) {
    static char last[200];
    const char *msg = albw_ss_note ? albw_ss_note() : NULL;
    if (!msg || !msg[0] || !strcmp(msg, last)) return;
    snprintf(last, sizeof last, "%s", msg);
    int slot = -1;
    const char *p = strstr(msg, "state ");
    if (p) slot = atoi(p + 6) - 1;
    if (slot < 0 || slot >= SS_SLOTS_UI) slot = -1;
    int saved = !strncmp(msg, "Saved state", 11), loaded = !strncmp(msg, "Loaded state", 12);
    if (saved && slot >= 0 && g_thumb_pending == slot) thumb_save(slot);
    if (saved) g_thumb_pending = -1;
    char l1[96];
    if (saved) snprintf(l1, sizeof l1, "Saved to slot %d", slot + 1);
    else if (loaded) snprintf(l1, sizeof l1, "Loaded slot %d", slot + 1);
    else snprintf(l1, sizeof l1, "%s", msg);
    char when[64] = "";
    if ((saved || loaded) && slot >= 0) slot_when(slot, when, sizeof when);
    ssui_toast(saved || loaded ? slot : -1, TOAST_NS, l1, when);
}

/* the toast, over the composited frame (plat_composite) */
static void ssui_draw(int ww, int wh) {
    if (!g_toast.t0) return;
    uint64_t age = plat_ticks_ns() - g_toast.t0;
    if (age >= g_toast.len) { g_toast.t0 = 0; return; }
    float a = 1;
    if (g_toast.len - age < 400000000ull) a = (float)(g_toast.len - age) / 400000000.0f;   /* fade out */
    menu_gl_init();
    float s = wh / 540.0f;
    if (s < 0.6f) s = 0.6f;
    if (s > 2.0f) s = 2.0f;
    float cw = MENU_FONT_W * s, th = MENU_FONT_H * s, pad = 8 * s;
    GLuint tex = g_toast.slot >= 0 ? ssui_thumb(g_toast.slot) : 0;
    float ph = tex ? th * 3.6f : 0, pw = ph * 5 / 3;
    size_t n1 = strlen(g_toast.line1), n2 = strlen(g_toast.line2);
    float tw = (n1 > n2 * 0.8f ? n1 : n2 * 0.8f) * cw;
    float bw = pad * 3 + pw + tw, bh = pad * 2 + (ph > th * 2.6f ? ph : th * 2.6f);
    float bx = (ww - bw) * 0.5f, by = pad * 2;
    menu_rect(bx, by, bw, bh, 0.08f, 0.10f, 0.12f, 0.92f * a);
    menu_rect(bx, by + bh - 2 * s, bw, 2 * s, 0.85f, 0.70f, 0.25f, a);   /* gold rule */
    float tx = bx + pad * 2 + pw;
    menu_text(tx, by + pad + th * 0.2f, s, g_toast.line1, 1, 1, 1, a);
    if (n2) menu_text(tx, by + pad + th * 1.45f, s * 0.8f, g_toast.line2, 0.75f, 0.75f, 0.75f, a);
    menu_flush(ww, wh, menu_tex, 0);
    if (tex) {
        menu_image(bx + pad, by + pad, pw, ph, a);
        menu_flush(ww, wh, tex, 1);
    }
}
