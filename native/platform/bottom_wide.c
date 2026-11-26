/* 16:9 bottom screen (included by platform.c before plat_composite): in widescreen the bottom
 * screen's frame is LAYOUT_WIDE_BOTTOM_W (427) columns while the game still draws its 320, centred
 * (layout.c layout_game_rect); the two side strips (~53 columns each) are filled here with the
 * game's own wood: Archive/Lyt_Game.arc timg/Wood_01.bflim (256 x 256 ETC1), the dark wood of the
 * bottom screen's right-hand panel, read from the ROM at the first use (sdk/fs.c albw_romfs_read),
 * drawn at 1 texel per 3DS pixel. The image is stored rotated (the BFLIM's swizzle 8: the grain
 * runs along the stored columns), so it is sampled turned 90 degrees: the grain runs horizontally as
 * on the panel. The game draws it nearly untinted: x1.07 matches the panel (HD: strip ~71,38,9 x1.07
 * against the panel's ~76,41,9 on the RP5). With an HD texture pack holding it (ALBW 4K does) the HD image is used, as
 * on the game's own panel. Touches on the strips go nowhere (layout_to_screen). */
void *albw_romfs_read(const char *path, size_t *size);   /* sdk/fs.c */

static GLuint bw_prog, bw_tex;
static GLint bw_loc_rect, bw_loc_uvr;
static int bw_state;              /* 0 not loaded, 1 loaded (original), 2 HD, -1 unavailable */
static uint64_t bw_hash;          /* CityHash64 of the original texture (HD pack lookup) */

static const char *BW_VS =
    "#version 330 core\n"
    "uniform vec4 rect; uniform vec4 uvr; out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = uvr.xy + p * uvr.zw;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *BW_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o; uniform sampler2D tex;\n"
    "void main(){ o = vec4(texture(tex, uv.yx).rgb * 1.07, 1.0); }\n";   /* stored rotated; the panel's brightness */

/* a file of a SARC archive by the end of its name ("Wood_01.bflim"); NULL if missing */
static const uint8_t *bw_sarc_find(const uint8_t *d, size_t n, const char *suffix, size_t *len) {
    if (n < 0x20 || memcmp(d, "SARC", 4)) return NULL;
    uint32_t doff, cnt;
    memcpy(&doff, d + 0x0C, 4);
    cnt = (uint32_t)d[0x1A] | (uint32_t)d[0x1B] << 8;
    size_t sfnt = 0x20 + (size_t)cnt * 16 + 8, sl = strlen(suffix);
    for (uint32_t i = 0; i < cnt && 0x20 + (size_t)i * 16 + 16 <= n; i++) {
        uint32_t attr, s, e;
        memcpy(&attr, d + 0x20 + i * 16 + 4, 4);
        memcpy(&s, d + 0x20 + i * 16 + 8, 4);
        memcpy(&e, d + 0x20 + i * 16 + 12, 4);
        size_t no = sfnt + (size_t)(attr & 0xFFFF) * 4;
        if (!(attr >> 24) || no >= n) continue;
        const char *nm = (const char *)d + no;
        size_t nl = strnlen(nm, n - no);
        if (nl >= sl && !memcmp(nm + nl - sl, suffix, sl) && doff + (size_t)e <= n && s <= e) {
            *len = e - s;
            return d + doff + s;
        }
    }
    return NULL;
}

static void bw_upload(const uint8_t *px, int w, int h) {   /* RGBA, bottom row first */
    if (!bw_tex) glGenTextures(1, &bw_tex);
    glBindTexture(GL_TEXTURE_2D, bw_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

/* the original texture from the ROM: 1 on success */
static int bw_load(void) {
    size_t n = 0, fl = 0;
    uint8_t *arc = albw_romfs_read("Archive/Lyt_Game.arc", &n);
    const uint8_t *f = arc ? bw_sarc_find(arc, n, "/Wood_01.bflim", &fl) : NULL;
    int ok = 0;
    /* BFLIM: the image, then a 0x28-byte footer: "FLIM" header (0x14), "imag" block: size,
     * width, height, alignment, format (10 = ETC1), swizzle, image size */
    if (f && fl > 0x28 && !memcmp(f + fl - 0x28, "FLIM", 4)) {
        const uint8_t *im = f + fl - 0x28 + 0x14;
        int w = im[8] | im[9] << 8, h = im[10] | im[11] << 8, fmt = im[14];
        size_t need = texfmt_size(12, w, h);
        uint8_t *px = w == 256 && h == 256 && fmt == 10 && fl - 0x28 >= need ? malloc((size_t)w * h * 4) : NULL;
        if (px && tex_decode(12, f, w, h, px) == 0) {
            bw_upload(px, w, h);
            bw_hash = hdtex_cityhash64(f, need);
            ok = 1;
        }
        free(px);
    }
    free(arc);
    fprintf(stderr, "bottom_wide: wood %s\n", ok ? "loaded (Lyt_Game.arc Wood_01)" : "not found: side strips stay black");
    return ok;
}

/* the side strips of the bottom screen's frame l->r[SCREEN_BOTTOM] (current GL state: plat_composite) */
static void bw_fill(const Layout *l) {
    if (LAYOUT_BOTTOM_W <= LAYOUT_BOTTOM_GAME_W || !l->visible[SCREEN_BOTTOM] || bw_state < 0) return;
    if (!bw_state) bw_state = bw_load() ? 1 : -1;
    if (bw_state < 0) return;
    {   /* the HD pack's version while HD textures are on, the original otherwise */
        const uint8_t *px;
        int pw, ph;
        int want_hd = hdtex_active() && hdtex_get(bw_hash, 256, 256, 12, &px, &pw, &ph) == 1;
        if (want_hd && bw_state != 2) { bw_upload(px, pw, ph); bw_state = 2; }
        else if (!want_hd && bw_state == 2 && !hdtex_active()) { bw_state = 0; bw_fill(l); return; }   /* reload the original */
    }
    if (!bw_prog) {
        bw_prog = gl_program(BW_VS, BW_FS);
        bw_loc_rect = glGetUniformLocation(bw_prog, "rect");
        bw_loc_uvr = glGetUniformLocation(bw_prog, "uvr");
        glUseProgram(bw_prog);
        glUniform1i(glGetUniformLocation(bw_prog, "tex"), 0);
    }
    const LayoutRect *r = &l->r[SCREEN_BOTTOM];
    LayoutRect g;
    layout_game_rect(l, SCREEN_BOTTOM, &g);
    float k = r->h / 240.0f;   /* window pixels per 3DS pixel */
    if (k <= 0) return;
    glUseProgram(bw_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bw_tex);
    for (int side = 0; side < 2; side++) {
        float x = side ? g.x + g.w : r->x, w = side ? r->x + r->w - (g.x + g.w) : g.x - r->x;
        if (w <= 0) continue;
        /* 1 texel per 3DS pixel; the right strip carries on from the panel's wood at 320 */
        float u0 = side ? 64.0f / 256 : 128.0f / 256, uw = w / k / 256, vh = 240.0f / 256;
        float rect[4] = {x / l->ww * 2 - 1, 1 - (r->y + r->h) / l->wh * 2, w / l->ww * 2, r->h / l->wh * 2};
        float uvr[4] = {u0, 0, uw, vh};
        glUniform4fv(bw_loc_rect, 1, rect);
        glUniform4fv(bw_loc_uvr, 1, uvr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glUseProgram(blit_prog);
}
