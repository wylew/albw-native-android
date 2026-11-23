/* Native OpenGL 3.3 core renderer (see render.h for the model: native ops recorded by the
 * replaced GPU-state producers, executed in nngx request order; no PICA word is ever decoded).
 *
 * ---------------------------------------------------------------- conventions
 * Render targets ("surfaces") are GL textures keyed by the game address of the colour / depth
 * buffer. GL row y = PICA window y (the PICA viewport origin is the bottom-left corner, like GL).
 * In 3DS memory a tiled colour buffer stores the TOP window row first, a DisplayTransfer keeps
 * the memory row order (flip = mirror rows), and the LCD shows linear memory rotated: LCD pixel
 * (x from the left, y from the top) = buffer pixel (column 239 - y, row x) for the 240-wide top
 * buffer (3dbrew "Framebuffers"). Display images are therefore kept in the same GL orientation
 * as surfaces (plain blit for a transfer) and the rotation happens only in render_scanout:
 *     texel(X = W-1 - lcd_y, Y = H-1 - lcd_x)        (W = 240, H = 400 or 320)
 * Textures decoded from memory use the same orientation (texdecode.c, row 0 = bottom = t 0), so
 * render-to-texture surfaces and memory textures are sampled identically.
 * PICA clip space has z in [-w, 0] (0 = near); the hand-written vertex programs output
 * gl_Position.z = -(2 z + w) so GL's [-w, w] range maps onto it, then glDepthRange(near, far)
 * applies the depth-map scale/offset the game programmed (ROP_DEPTHMAP).
 * ALBW_SCANOUT=direct|rot|rotflip overrides the scan-out orientation for debugging.
 */
#define _GNU_SOURCE
#include "render.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../platform/platform.h"
#include "fraglight.h"
#include "effects.h"
#include "prim.h"
#include "texdecode.h"

/* ================================================================== GL functions beyond gl.h */

#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR 0x1800
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_TEST 0x0B90
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_ONE 1
#define GL_ZERO 0
#define GL_TRIANGLE_FAN 0x0006
#define GL_BYTE 0x1400
#define GL_SHORT 0x1402
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_INCR 0x1E02
#define GL_DECR 0x1E03
#define GL_INVERT 0x150A
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508
#define GL_NEVER 0x0200
#define GL_EQUAL 0x0202
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_CLEAR 0x1500
#define GL_UNSIGNED_INT 0x1405
#define GL_RG 0x8227
#define GL_RG32F 0x8230

#define RGL_FUNCS(X) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, StencilFunc, (GLenum, GLint, GLuint)) \
    X(void, StencilOp, (GLenum, GLenum, GLenum)) \
    X(void, StencilMask, (GLuint)) \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, BlendEquationSeparate, (GLenum, GLenum)) \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, CullFace, (GLenum)) \
    X(void, FrontFace, (GLenum)) \
    X(void, PolygonOffset, (GLfloat, GLfloat)) \
    X(void, DepthRange, (double, double)) \
    X(void, LogicOp, (GLenum)) \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat *)) \
    X(void, ClearBufferfi, (GLenum, GLint, GLfloat, GLint)) \
    X(void, Uniform1iv, (GLint, GLsizei, const GLint *)) \
    X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, Uniform2i, (GLint, GLint, GLint)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, TexParameterfv, (GLenum, GLenum, const GLfloat *)) \
    X(void, TexParameterf, (GLenum, GLenum, GLfloat)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, BindFragDataLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, DrawBuffers, (GLsizei, const GLenum *))

#define RGL_DEFINE(ret, name, args) static ret(*rgl##name) args;
RGL_FUNCS(RGL_DEFINE)
#undef RGL_DEFINE

/* ================================================================== small helpers */

extern int g_dbg_fog_off __attribute__((weak));   /* platform.c: Debug > Fog off */
static bool g_trace;
static void rlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("render: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define WARN_ONCE(...) do { static bool once_; if (!once_) { once_ = true; rlog(__VA_ARGS__); } } while (0)

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { rlog("out of memory"); abort(); }
    return q;
}

#define VEC_PUSH(arr, n, cap) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 16; \
    (arr) = xrealloc((arr), (cap) * sizeof *(arr)); } } while (0)

static int fmt_bytes(int rfmt) {
    switch (rfmt) {
    case RFMT_RGBA8: return 4;
    case RFMT_RGB8: return 3;
    default: return 2;
    }
}
static int depth_bytes(int dfmt) { return dfmt == RDEPTH_D16 ? 2 : dfmt == RDEPTH_D24 ? 3 : 4; }

/* ================================================================== state */

typedef struct {
    uintptr_t addr;
    size_t bytes;
    int w, h, fmt;       /* logical size (3DS pixels) and RFMT_* / RDEPTH_* */
    bool depth;
    GLuint tex;          /* GL_RGBA8 or GL_DEPTH24_STENCIL8 texture, (w*scale) x (h*scale) */
    GLuint fbo;          /* single-attachment FBO (clears, blits) */
    int ph;              /* physical rows (logical units): h, or wider for top-screen buffers in widescreen */
} Surface;

/* ================================================================== widescreen
 * The top screen is a 240x400 buffer stored rotated (GL rows = the 400-pixel long side). In
 * widescreen every surface of that shape (colour, depth, display image) gets g_ws_rows physical
 * rows instead of 400; viewports and copies that cover the whole 400 rows are stretched onto them,
 * and the game's projections are widened by the same factor (albw_ws_factor, compilefix
 * 0057F6C0 / 001312BC): more of the scene to the sides, nothing stretched. Passes that use a part
 * of such a buffer (the bottom screen draws 240x320 into the shared 0x1F000080 buffer) stay 1:1.
 * A separate static (not a field of R): save states restore static data by symbol. */
static int g_ws_rows = 400;
/* portrait 5:3 = the top screen's shape (240x400, and the 480x800 sources of anti-aliased
 * transfers): every projection with the top screen's aspect is widened (0057F6C0), so every target
 * of that shape is drawn wide too */
static int surf_wide_shape(const Surface *s) { return s->w >= 120 && s->w * 5 == s->h * 3; }
static int sph(const Surface *s) { return s->ph > 0 ? s->ph : s->h; }
/* what each program last received (upload_uniforms skips unchanged uniforms: every GL call costs
 * a TLS lookup in the driver, which glibc makes expensive on i386 - BOOT_LOG 160ad). Not in
 * Program: R is part of save states, its layout must stay. Index: progs, then variants. */
typedef struct {
    uint8_t sh_valid;
    float sh_c[RENDER_VS_FLOATS * 4];
    GLint sh_b, sh_src[24], sh_cmb[24], sh_buf[2], sh_at[4], sh_fog[2], sh_tc[4];
    float sh_k[24], sh_bufcol[4], sh_fogc[3], sh_ws;
} UniShadow;
struct UniShadow_ { UniShadow u; };
static UniShadow *g_ush;   /* RPROG_COUNT + RENDER_PROG_VARIANTS entries, allocated on first use */
static float g_cur_fy = 1.0f;
static GLuint g_glprog_bound;   /* the program bound by the renderer (0 = unknown; use_program) */   /* stretch of the viewport last applied (apply_frag) */
float albw_ws_factor(void) { return (float)g_ws_rows / 400.0f; }

typedef struct { GLuint fbo; Surface *c, *d; } FboEnt;

typedef struct {
    uintptr_t mem;
    size_t bytes;
    int w, h, fmt;
    Surface *img;        /* GL image (a colour surface not in the surface list) */
    bool valid;          /* received a transfer */
} Display;

typedef struct { uintptr_t start, end; uint32_t value; int bits; } PendingFill;

typedef struct {
    uintptr_t addr;
    int w, h, fmt, levels;
    bool cube;
    uintptr_t faces[6];
    GLuint tex;
    bool dirty;
    uint64_t hash;
    uint32_t checked_frame, used_frame;
    int p_wrap_s, p_wrap_t, p_min, p_mag; /* last sampler params set */
    int p_maxl, p_minl;                   /* last per-draw LOD range set (-1 none) */
    int up_levels;                        /* levels tex_upload actually stored (<= levels: stops under 8x8) */
} TexEnt;

typedef struct {
    GLuint prog;
    GLint c, b, tev_src, tev_cmb, tev_const, tev_buf, tev_bufcol, alpha, texcfg, tex[3], texcube, scale;
    GLint ws;   /* u_ws: widescreen stretch of the current viewport (layout shader keeps HUD 1:1) */
    GLint fog, fog_color, fog_lut;
    bool fl;           /* fragment lighting compiled in */
    FLLoc fll;
    char *vs_src;      /* vertex shader source (specialised programs reuse it) */
    struct UniShadow_ *ush;   /* specialised programs: their own uniform shadow (else ush_of's table) */
} Program;

typedef struct { uintptr_t key; int prog, variant; } ProgAlias;

static struct {
    bool ready, failed;
    int scale;                       /* internal resolution multiplier */
    int scanout_mode;                /* 0 auto, 1 direct, 2 rot, 3 rotflip */

    Surface **surf; size_t nsurf, capsurf;
    FboEnt *fbos; size_t nfbo, capfbo;
    Display *disp; size_t ndisp, capdisp;
    PendingFill pend[16]; unsigned pend_next;
    TexEnt *tex; size_t ntex, captex;

    /* current pipeline state */
    Surface *cur_c, *cur_d;
    GLuint cur_fbo;
    int vp[4];
    bool scissor; int sc[4];
    RFragState frag; bool frag_dirty;
    struct { uint8_t mode, flip; float rgb[3]; uint32_t key, tex_key; GLuint tex; } fog;
    RTexUnit unit[3]; int tex_enable, tex2_tc1;
    RTev tev; bool tev_dirty;
    int prog_id;
    float vsf[RENDER_VS_FLOATS * 4]; bool vsf_dirty;
    uint16_t vsb;
    struct { uintptr_t ptr; int type, count, stride; bool array; float k[4]; } attr[RENDER_MAX_ATTRIBS];

    Program progs[RPROG_COUNT];
    Program variants[RENDER_PROG_VARIANTS];  /* RPROG_H3D_DEFAULT variants (extension GLSL) */
    int prog_variant;
    RenderExt ext;
    ProgAlias *alias; size_t nalias, capalias;
    GLuint scan_prog, vao, vbo, ibo;
    size_t vbo_off, ibo_off;
    GLint scan_rot;

    uint32_t frame;
    RStats st;
} R = {.scale = 1};

#define STREAM_BYTES (8u << 20)

/* ================================================================== GLSL */

/* Common fragment program: PICA fixed-function fragment pipeline as uniforms
 * (6 texture-combiner stages + combiner buffer, alpha test). Programs built with FL = 1 (the
 * nw::h3d DefaultShader variants) also evaluate the 3DS fragment lighting (fraglight.c, hand-written
 * GLSL) into the combiner sources fragment primary / secondary; in the others both are 0. */
static const char FS_HEAD[] =
    "in vec4 v_color; in vec4 v_tc0; in vec2 v_tc1; in vec2 v_tc2; in vec3 v_normal; in vec3 v_view;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D tex0, tex1, tex2; uniform samplerCube texcube0;\n"
    "uniform ivec4 tev_src[6]; uniform ivec4 tev_cmb[6]; uniform vec4 tev_const[6];\n"
    "uniform ivec2 tev_buf; uniform vec4 tev_bufcol; uniform ivec4 alpha_test; uniform ivec4 texcfg;\n"
    "uniform ivec2 fog; uniform vec3 fog_color; uniform sampler2D fog_lut;\n";
static const char FS_TEV[] =
    "vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), prev = vec4(0.0), buf = vec4(0.0), k = vec4(0.0);\n"
    "vec4 fprim = vec4(0.0), fsec = vec4(0.0);\n"
    "vec4 src(int s){\n"
    "  if (s == 0) return clamp(v_color, 0.0, 1.0);\n"
    "  if (s == 1) return fprim; if (s == 2) return fsec;\n"
    "  if (s == 3) return t0; if (s == 4) return t1; if (s == 5) return t2;\n"
    "  if (s == 13) return buf; if (s == 14) return k; if (s == 15) return prev;\n"
    "  return vec4(0.0);\n"
    "}\n"
    "vec3 op_rgb(int o, vec4 v){\n"
    "  if (o == 0) return v.rgb; if (o == 1) return 1.0 - v.rgb;\n"
    "  if (o == 2) return vec3(v.a); if (o == 3) return vec3(1.0 - v.a);\n"
    "  if (o == 4) return vec3(v.r); if (o == 5) return vec3(1.0 - v.r);\n"
    "  if (o == 8) return vec3(v.g); if (o == 9) return vec3(1.0 - v.g);\n"
    "  if (o == 12) return vec3(v.b); if (o == 13) return vec3(1.0 - v.b);\n"
    "  return v.rgb;\n"
    "}\n"
    "float op_a(int o, vec4 v){\n"
    "  if (o == 0) return v.a; if (o == 1) return 1.0 - v.a; if (o == 2) return v.r; if (o == 3) return 1.0 - v.r;\n"
    "  if (o == 4) return v.g; if (o == 5) return 1.0 - v.g; if (o == 6) return v.b; if (o == 7) return 1.0 - v.b;\n"
    "  return v.a;\n"
    "}\n"
    "vec3 cmb3(int c, vec3 a, vec3 b, vec3 d){\n"
    "  if (c == 0) return a; if (c == 1) return a * b; if (c == 2) return min(a + b, 1.0);\n"
    "  if (c == 3) return clamp(a + b - 0.5, 0.0, 1.0); if (c == 4) return a * d + b * (1.0 - d);\n"
    "  if (c == 5) return max(a - b, 0.0); if (c == 6 || c == 7) return vec3(clamp(4.0 * dot(a - 0.5, b - 0.5), 0.0, 1.0));\n"
    "  if (c == 8) return min(a * b + d, 1.0); if (c == 9) return min(a + b, 1.0) * d;\n"
    "  return a;\n"
    "}\n"
    "float cmb1(int c, float a, float b, float d){\n"
    "  if (c == 0) return a; if (c == 1) return a * b; if (c == 2) return min(a + b, 1.0);\n"
    "  if (c == 3) return clamp(a + b - 0.5, 0.0, 1.0); if (c == 4) return a * d + b * (1.0 - d);\n"
    "  if (c == 5) return max(a - b, 0.0); if (c == 8) return min(a * b + d, 1.0); if (c == 9) return min(a + b, 1.0) * d;\n"
    "  return a;\n"
    "}\n"
    "bool test(int f, int a, int r){\n"
    "  if (f == 0) return false; if (f == 1) return true; if (f == 2) return a == r; if (f == 3) return a != r;\n"
    "  if (f == 4) return a < r; if (f == 5) return a <= r; if (f == 6) return a > r; return a >= r;\n"
    "}\n"
    "void main(){\n"
    "  t0 = vec4(0.0); t1 = vec4(0.0); t2 = vec4(0.0);\n"
    "  if ((texcfg.x & 1) != 0) {\n"
    "    if (texcfg.z == 1) t0 = texture(texcube0, v_tc0.xyz);\n"
    "    else if (texcfg.z == 2) t0 = textureProj(tex0, v_tc0.xyw);\n"
    "    else t0 = texture(tex0, v_tc0.xy);\n"
    "  }\n"
    "  if ((texcfg.x & 2) != 0) t1 = texture(tex1, v_tc1);\n"
    "  if ((texcfg.x & 4) != 0) t2 = texture(tex2, texcfg.y != 0 ? v_tc1 : v_tc2);\n"
    "#if FL\n"
    "  fl_compute(t0, t1, t2, fprim, fsec);\n"
    "#endif\n"
    "  prev = vec4(0.0); buf = vec4(0.0); vec4 next = tev_bufcol;\n"
    "  for (int i = 0; i < 6; i++) {\n"
    "    ivec4 s = tev_src[i]; ivec4 m = tev_cmb[i]; k = tev_const[i];\n"
    "    vec3 a = op_rgb(s.z & 15, src(s.x & 15)), b = op_rgb(s.z >> 4 & 15, src(s.x >> 4 & 15)),\n"
    "         c = op_rgb(s.z >> 8 & 15, src(s.x >> 8 & 15));\n"
    "    float aa = op_a(s.w & 15, src(s.y & 15)), ab = op_a(s.w >> 4 & 15, src(s.y >> 4 & 15)),\n"
    "          ac = op_a(s.w >> 8 & 15, src(s.y >> 8 & 15));\n"
    "    vec3 rgb = min(cmb3(m.x, a, b, c) * float(1 << m.z), 1.0);\n"
    "    float al = (m.x == 7) ? rgb.r : min(cmb1(m.y, aa, ab, ac) * float(1 << m.w), 1.0);\n"
    "    prev = vec4(rgb, al);\n"
    "    buf = next;\n"
    "    if (i < 4 && ((tev_buf.x >> i) & 1) != 0) next.rgb = prev.rgb;\n"
    "    if (i < 4 && ((tev_buf.y >> i) & 1) != 0) next.a = prev.a;\n"
    "  }\n"
    "  if (alpha_test.x != 0 && !test(alpha_test.y, int(prev.a * 255.0 + 0.5), alpha_test.z)) discard;\n"
    /* PICA fog (register 0xE0 mode 5): factor from the 128-entry LUT indexed by the window depth
       (flipped when 0xE0 bit 16 is set), colour = mix(fog colour, colour, factor); alpha kept */
    "  if (fog.x == 5) {\n"
    "    float z = clamp(fog.y != 0 ? 1.0 - gl_FragCoord.z : gl_FragCoord.z, 0.0, 1.0) * 128.0;\n"
    "    float zi = min(floor(z), 127.0);\n"
    "    vec2 e = texelFetch(fog_lut, ivec2(int(zi), 0), 0).rg;\n"
    "    prev.rgb = mix(fog_color, prev.rgb, clamp(e.x + e.y * (z - zi), 0.0, 1.0));\n"
    "  }\n"
    "  o_color = prev;\n"
    /* debug (ALBW_FS_DEBUG): 1 texture 0, 2 vertex colour, 3 fragment-lighting diffuse */
    "#if DBGOUT == 1 && FL\n  if (texcfg.x != 0) o_color = vec4(t0.rgb, 1.0);\n#elif DBGOUT == 2 && FL\n  if (texcfg.x != 0) o_color = vec4(v_color.rgb, 1.0);\n"
    "#elif DBGOUT == 4 && FL\n  if (texcfg.x != 0) o_color = vec4(fract(v_tc0.xy), 0.0, 1.0);\n#elif DBGOUT == 5 && FL\n  if (texcfg.x != 0) o_color = vec4(t0.aaa, 1.0);\n"
    "#elif DBGOUT == 6 && FL\n  if (texcfg.x != 0) o_color = vec4(fsec.rgb, 1.0);\n"
    "#elif DBGOUT == 7 && FL\n  if (texcfg.x != 0) o_color = vec4(vec3(fract(gl_FragCoord.z * 256.0)), 1.0);\n"
    "#elif DBGOUT == 3 && FL\n  if (texcfg.x != 0) o_color = vec4(fprim.rgb, 1.0);\n#endif\n"
    "}\n";

/* Vertex programs: same interface as the PICA shaders they stand for (see SHADERS.md): float
 * uniform registers c[0..95] (rows of matrices as the game uploads them), bool uniforms b, input
 * registers v0..v11. pica_out() converts PICA clip space (z in [-w, 0]) to GL. */
#define VS_HEAD \
    "#version 330 core\n" \
    "layout(location = 0) in vec4 v0; layout(location = 1) in vec4 v1; layout(location = 2) in vec4 v2;\n" \
    "layout(location = 3) in vec4 v3; layout(location = 4) in vec4 v4; layout(location = 5) in vec4 v5;\n" \
    "layout(location = 6) in vec4 v6; layout(location = 7) in vec4 v7;\n" \
    "uniform vec4 c[96]; uniform int b;\n" \
    "out vec4 v_color; out vec4 v_tc0; out vec2 v_tc1; out vec2 v_tc2; out vec3 v_normal; out vec3 v_view;\n" \
    "vec4 pica_out(vec4 p){ return vec4(p.x, p.y, -(2.0 * p.z + p.w), p.w); }\n" \
    "vec4 m4(int r, vec4 v){ return vec4(dot(c[r], v), dot(c[r + 1], v), dot(c[r + 2], v), dot(c[r + 3], v)); }\n" \
    "vec3 m3(int r, vec4 v){ return vec3(dot(c[r], v), dot(c[r + 1], v), dot(c[r + 2], v)); }\n"

/* RPROG_CLIP: v0 clip position, v1 colour (0..1), v2 uv0 */
static const char VS_CLIP[] = VS_HEAD
    "void main(){ gl_Position = pica_out(v0); v_color = v1; v_tc0 = vec4(v2.xy, 0.0, 1.0);\n"
    "  v_tc1 = v2.xy; v_tc2 = v2.xy; v_normal = vec3(0.0, 0.0, 1.0); v_view = vec3(0.0); }\n";

/* Generic "projection (c0-c3) x modelview (c4-c6, 3x4)" program: provisional body for the game
 * programs until each one is written by hand from SHADERS.md. v0 position, v1 colour (0..255
 * bytes or 0..1 floats: c7.x = colour scale, 0 means 1), v2 uv0, v3 uv1. */
static const char VS_MVP[] = VS_HEAD
    "void main(){ vec4 p = vec4(m3(4, vec4(v0.xyz, 1.0)), 1.0); gl_Position = pica_out(m4(0, p));\n"
    "  float cs = c[7].x != 0.0 ? c[7].x : 1.0; v_color = v1 * cs; v_tc0 = vec4(v2.xy, 0.0, 1.0);\n"
    "  v_tc1 = v3.xy; v_tc2 = v3.xy; v_normal = vec3(0.0, 0.0, 1.0); v_view = -p.xyz; }\n";

static const char *VS_SRC[RPROG_COUNT] = { /* RPROG_H3D_DEFAULT: extension GLSL when registered */
    [RPROG_CLIP] = VS_CLIP,
    [RPROG_GR_DEFAULT] = VS_MVP,
    [RPROG_FONT_RECT] = RENDER_VS_FONT_RECT,
    [RPROG_H3D_DEFAULT] = VS_MVP,
    [RPROG_PRIMITIVE] = RENDER_VS_PRIMITIVE,   /* prim.c */
    [RPROG_EFT] = RENDER_VS_EFT,               /* effects.c */
};

/* ================================================================== GL setup */

/* fragment program source (FL: with the fragment-lighting GLSL) */
static char *fs_source(bool fl) {
    size_t n = strlen(FS_HEAD) + strlen(FS_TEV) + strlen(RENDER_FS_FRAGLIGHT) + 64;
    char *s = xrealloc(NULL, n);
    { static int dbg = -1; if (dbg < 0) { const char *e = getenv("ALBW_FS_DEBUG"); dbg = e ? atoi(e) : 0; }
      snprintf(s, n, "#version 330 core\n#define FL %d\n#define DBGOUT %d\n%s%s%s", fl ? 1 : 0, dbg, FS_HEAD, fl ? RENDER_FS_FRAGLIGHT : "", FS_TEV); }
    return s;
}

#define FL_LUT_UNIT 4   /* texture unit of the fragment-lighting LUT texture */
#define FOG_LUT_UNIT 6  /* texture unit of the fog LUT (128 x 1 RG32F) */

static UniShadow *ush_of(const Program *p) {
    if (p->ush) return &p->ush->u;
    if (!g_ush) {
        g_ush = calloc(RPROG_COUNT + RENDER_PROG_VARIANTS, sizeof *g_ush);
        if (!g_ush) abort();
    }
    ptrdiff_t i = p - R.progs;
    if (i < 0 || i >= RPROG_COUNT) i = RPROG_COUNT + (p - R.variants);
    return &g_ush[i];
}

static char *fs_source_spec(bool fl, const uint32_t *key);
static void program_init_fs(Program *p, const char *vs, bool fl, const uint32_t *spec_key) {
    /* gl_program aborts with the log on a compile error: these sources are fixed */
    char *fs = spec_key ? fs_source_spec(fl, spec_key) : fs_source(fl);
    if (!p->vs_src) p->vs_src = strdup(vs);
    p->prog = gl_program(vs, fs);
    free(fs);
    p->fl = fl;
    GLuint g = p->prog;
    glUseProgram(g);
    g_glprog_bound = g;
    ush_of(p)->sh_valid = 0;
    p->c = glGetUniformLocation(g, "c");
    p->b = glGetUniformLocation(g, "b");
    p->tev_src = glGetUniformLocation(g, "tev_src");
    p->tev_cmb = glGetUniformLocation(g, "tev_cmb");
    p->tev_const = glGetUniformLocation(g, "tev_const");
    p->tev_buf = glGetUniformLocation(g, "tev_buf");
    p->tev_bufcol = glGetUniformLocation(g, "tev_bufcol");
    p->alpha = glGetUniformLocation(g, "alpha_test");
    p->texcfg = glGetUniformLocation(g, "texcfg");
    p->scale = glGetUniformLocation(g, "res_scale");
    p->ws = glGetUniformLocation(g, "u_ws");
    p->fog = glGetUniformLocation(g, "fog");
    p->fog_color = glGetUniformLocation(g, "fog_color");
    p->fog_lut = glGetUniformLocation(g, "fog_lut");
    if (p->fog_lut >= 0) glUniform1i(p->fog_lut, FOG_LUT_UNIT);
    static const char *tn[3] = {"tex0", "tex1", "tex2"};
    for (int i = 0; i < 3; i++) {
        p->tex[i] = glGetUniformLocation(g, tn[i]);
        glUniform1i(p->tex[i], i);
    }
    p->texcube = glGetUniformLocation(g, "texcube0");
    glUniform1i(p->texcube, 3);
    if (fl) {
        fraglight_locations(g, &p->fll);
        glUniform1i(p->fll.lut, FL_LUT_UNIT);
    }
}

static void program_init(Program *p, const char *vs, bool fl) { program_init_fs(p, vs, fl, NULL); }
static void program_build(int id) { program_init(&R.progs[id], VS_SRC[id], false); }

unsigned long render_variant_builds;   /* h3d program variants compiled so far (frame-time debug) */
static Program *build_variant(int v);
/* the program for the current prog id / variant (extension variants compiled on first use) */
static void use_program(GLuint prog) {
    if (prog != g_glprog_bound) { glUseProgram(prog); g_glprog_bound = prog; }
}

static Program *cur_program(void) {
    if (R.prog_id != RPROG_H3D_DEFAULT || !R.ext.h3d_vs_body || !R.ext.h3d_defines) return &R.progs[R.prog_id];
    int v = R.prog_variant >= 0 && R.prog_variant < RENDER_PROG_VARIANTS ? R.prog_variant : 0;
    return build_variant(v);
}

/* compile h3d program variant v if not done yet (first use, or the per-frame pre-build) */
static Program *build_variant(int v) {
    Program *p = &R.variants[v];
    if (!p->prog) {
        char def[512];
        R.ext.h3d_defines(v, def, sizeof def);
        size_t n = strlen(def) + strlen(R.ext.h3d_vs_body) + 32;
        char *src = xrealloc(NULL, n);
        snprintf(src, n, "#version 330 core\n%s\n%s", def, R.ext.h3d_vs_body);
        program_init(p, src, true); /* the DefaultShader GLSL outputs v_tangent */
        render_variant_builds++;
        free(src);
        if (g_trace) rlog("built h3d program variant %d", v);
    }
    return p;
}

static const char *SCAN_VS =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *SCAN_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o; uniform sampler2D img; uniform int rot;\n"
    "void main(){ vec2 t = rot == 0 ? uv : rot == 1 ? vec2(uv.y, 1.0 - uv.x) : vec2(uv.y, uv.x);\n"
    "  o = vec4(texture(img, t).rgb, 1.0); }\n";

static void reset_state(void);
static int g_light_trace;
static uint64_t g_light_digest;
static unsigned g_light_draws;
static long g_dump_frame = -1, g_dump_last = -1;   /* ALBW_RENDER_DUMP=<frame>[-<last>] (see dumping()) */

#if defined(__ANDROID__)
static void (*g_depth_rangef)(GLfloat, GLfloat);
static void standin_DepthRange(double n, double f) {
    if (!g_depth_rangef) g_depth_rangef = (void (*)(GLfloat, GLfloat))SDL_GL_GetProcAddress("glDepthRangef");
    if (g_depth_rangef) g_depth_rangef((GLfloat)n, (GLfloat)f);
}
static void standin_LogicOp(GLenum op) {
    static int warned;
    if (!warned++) rlog("GL: glLogicOp(0x%x) not available on GLES: ignored (README: Android step 6)", op);
}
/* single-output fragment shaders get location 0 anyway */
static void standin_BindFragDataLocation(GLuint p, GLuint c, const GLchar *n) { (void)p; (void)c; (void)n; }
static void *gles_standin(const char *name) {
    if (!strcmp(name, "glDepthRange")) return (void *)standin_DepthRange;
    if (!strcmp(name, "glLogicOp")) return (void *)standin_LogicOp;
    if (!strcmp(name, "glBindFragDataLocation")) return (void *)standin_BindFragDataLocation;
    return NULL;
}
#endif

int render_ready(void) {
    if (R.ready) return 1;
    if (R.failed || !SDL_GL_GetCurrentContext()) return 0;
    lock();
    if (R.ready) { unlock(); return 1; }
    g_trace = getenv("ALBW_RENDER_TRACE") != NULL;
    int missing = 0;
#if defined(__ANDROID__)
    /* GLES 3.2 lacks these desktop functions: stand-ins (logic ops: README step 6) */
#define RGL_LOAD(ret, name, args) \
    rgl##name = (ret(*) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!rgl##name) rgl##name = (ret(*) args)gles_standin("gl" #name); \
    if (!rgl##name) { rlog("GL: missing gl%s", #name); missing++; }
#else
#define RGL_LOAD(ret, name, args) \
    rgl##name = (ret(*) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!rgl##name) { rlog("GL: missing gl%s", #name); missing++; }
#endif
    RGL_FUNCS(RGL_LOAD)
#undef RGL_LOAD
    if (missing) {
        R.failed = true;
        unlock();
        return 0;
    }
    const char *s = getenv("ALBW_RENDER_SCALE");
    if (s && atoi(s) >= 1 && atoi(s) <= 8) R.scale = atoi(s);
    g_light_trace = getenv("ALBW_LIGHT_TRACE") != NULL;
    s = getenv("ALBW_RENDER_DUMP");
    if (s) { char *e; g_dump_frame = strtol(s, &e, 10); g_dump_last = *e == '-' ? atol(e + 1) : g_dump_frame; }
    s = getenv("ALBW_SCANOUT");
    R.scanout_mode = !s ? 0 : !strcmp(s, "direct") ? 1 : !strcmp(s, "rot") ? 2 : !strcmp(s, "rotflip") ? 3 : 0;
    for (int i = 0; i < RPROG_COUNT; i++) program_build(i);
    R.scan_prog = gl_program(SCAN_VS, SCAN_FS);
    glUseProgram(R.scan_prog);
    glUniform1i(glGetUniformLocation(R.scan_prog, "img"), 0);
    R.scan_rot = glGetUniformLocation(R.scan_prog, "rot");
    glGenVertexArrays(1, &R.vao);
    glGenBuffers(1, &R.vbo);
    glGenBuffers(1, &R.ibo);
    glBindVertexArray(R.vao);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glBufferData(GL_ARRAY_BUFFER, STREAM_BYTES, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, STREAM_BYTES / 4, NULL, GL_STREAM_DRAW);
    glBindVertexArray(0);
    reset_state();
    R.ready = true;
    rlog("OpenGL renderer ready (internal scale %d)", R.scale);
    unlock();
    return 1;
}

/* ================================================================== surfaces */

static void surface_alloc(Surface *s) {
    s->ph = surf_wide_shape(s) ? (int)((long)s->h * g_ws_rows / 400) : s->h;
    int W = s->w * R.scale, H = s->ph * R.scale;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (s->depth) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, W, H, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        /* framebuffer formats without alpha read back alpha = 1 when sampled */
        if (s->fmt == RFMT_RGB8 || s->fmt == RFMT_RGB565) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    if (s->depth) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s->tex, 0);
        GLenum none = 0;
        rglDrawBuffers(1, &none);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        rlog("surface %#lx (%dx%d %s) FBO incomplete", (unsigned long)s->addr, s->w, s->h, s->depth ? "depth" : "colour");
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    R.st.surfaces++;
}

static void surface_free(Surface *s) {
    if (s->fbo) rglDeleteFramebuffers(1, &s->fbo);
    if (s->tex) glDeleteTextures(1, &s->tex);
    s->fbo = s->tex = 0;
}

static void fbo_forget(Surface *s) {
    for (size_t i = 0; i < R.nfbo;) {
        if (R.fbos[i].c == s || R.fbos[i].d == s) {
            if (R.fbos[i].fbo == R.cur_fbo) R.cur_fbo = 0;
            rglDeleteFramebuffers(1, &R.fbos[i].fbo);
            R.fbos[i] = R.fbos[--R.nfbo];
        } else {
            i++;
        }
    }
    if (R.cur_c == s) R.cur_c = NULL;
    if (R.cur_d == s) R.cur_d = NULL;
}

/* surface whose memory range contains addr (exact start preferred) */
static Surface *surface_at(uintptr_t addr, int depth) {
    Surface *best = NULL;
    for (size_t i = 0; i < R.nsurf; i++) {
        Surface *s = R.surf[i];
        if (depth >= 0 && s->depth != (depth != 0)) continue;
        if (s->addr == addr) return s;
        if (!best && addr > s->addr && addr < s->addr + s->bytes) best = s;
    }
    return best;
}

static void clear_surface(Surface *s, uint32_t value, int bits);

/* surface at addr with this size/format, (re)created as needed */
static Surface *surface_get(uintptr_t addr, int w, int h, int fmt, bool depth) {
    if (!addr || w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    for (size_t i = 0; i < R.nsurf; i++) {
        Surface *s = R.surf[i];
        if (s->addr != addr || s->depth != depth) continue;
        if (s->w == w && s->h == h && s->fmt == fmt) return s;
        /* same memory, new shape: recreate */
        fbo_forget(s);
        surface_free(s);
        s->w = w; s->h = h; s->fmt = fmt;
        s->bytes = (size_t)w * h * (depth ? depth_bytes(fmt) : fmt_bytes(fmt));
        surface_alloc(s);
        return s;
    }
    Surface *s = calloc(1, sizeof *s);
    if (!s) abort();
    s->addr = addr; s->w = w; s->h = h; s->fmt = fmt; s->depth = depth;
    s->bytes = (size_t)w * h * (depth ? depth_bytes(fmt) : fmt_bytes(fmt));
    surface_alloc(s);
    VEC_PUSH(R.surf, R.nsurf, R.capsurf);
    R.surf[R.nsurf++] = s;
    if (g_trace) rlog("new %s surface %#lx %dx%d fmt %d", depth ? "depth" : "colour", (unsigned long)addr, w, h, fmt);
    /* a fill that happened before the surface existed, else a defined start (black / far) */
    unsigned k = 1;
    for (; k <= 16; k++) {
        const PendingFill *f = &R.pend[(R.pend_next - k) % 16];
        if (f->end && addr >= f->start && addr < f->end) { clear_surface(s, f->value, f->bits); break; }
    }
    if (k > 16) {
        clear_surface(s, depth ? 0x00FFFFFFu : 0x000000FFu, 32);
        R.st.fills--; /* not a game fill */
    }
    return s;
}

static GLuint fbo_for(Surface *c, Surface *d) {
    if (c && !d) return c->fbo;
    if (d && !c) return d->fbo;
    if (!c) return 0;
    for (size_t i = 0; i < R.nfbo; i++)
        if (R.fbos[i].c == c && R.fbos[i].d == d) return R.fbos[i].fbo;
    if (c->w != d->w || c->h != d->h) {
        WARN_ONCE("colour %dx%d and depth %dx%d buffers differ in size: depth ignored", c->w, c->h, d->w, d->h);
        return c->fbo;
    }
    FboEnt e = {0, c, d};
    glGenFramebuffers(1, &e.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rlog("combined FBO incomplete");
    VEC_PUSH(R.fbos, R.nfbo, R.capfbo);
    R.fbos[R.nfbo++] = e;
    return e.fbo;
}

/* ================================================================== fills */

/* fill pattern -> colour, as packed by the game's clear code (003CB588 / 003CB5F0) */
static void fill_color(int fmt, uint32_t v, int bits, float c[4]) {
    if (fmt < 0) fmt = bits == 32 ? RFMT_RGBA8 : bits == 24 ? RFMT_RGB8 : RFMT_RGB565;
    switch (fmt) {
    case RFMT_RGBA8: c[0] = (v >> 24 & 255) / 255.f; c[1] = (v >> 16 & 255) / 255.f; c[2] = (v >> 8 & 255) / 255.f; c[3] = (v & 255) / 255.f; break;
    case RFMT_RGB8: c[0] = (v >> 16 & 255) / 255.f; c[1] = (v >> 8 & 255) / 255.f; c[2] = (v & 255) / 255.f; c[3] = 1; break;
    case RFMT_RGB565: c[0] = (v >> 11 & 31) / 31.f; c[1] = (v >> 5 & 63) / 63.f; c[2] = (v & 31) / 31.f; c[3] = 1; break;
    case RFMT_RGB5A1: c[0] = (v >> 11 & 31) / 31.f; c[1] = (v >> 6 & 31) / 31.f; c[2] = (v >> 1 & 31) / 31.f; c[3] = (float)(v & 1); break;
    default: c[0] = (v >> 12 & 15) / 15.f; c[1] = (v >> 8 & 15) / 15.f; c[2] = (v >> 4 & 15) / 15.f; c[3] = (v & 15) / 15.f; break;
    }
}

static void clear_surface(Surface *s, uint32_t value, int bits) {
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glDisable(GL_SCISSOR_TEST);
    if (s->depth) {
        float depth;
        int stencil = 0;
        if (s->fmt == RDEPTH_D16) depth = (value & 0xFFFF) / 65535.f;
        else depth = (value & 0xFFFFFF) / 16777215.f;
        if (s->fmt == RDEPTH_D24S8) stencil = (int)(value >> 24);
        glDepthMask(GL_TRUE);
        rglStencilMask(0xFF);
        rglClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
    } else {
        float c[4];
        fill_color(s->fmt, value, bits, c);
        rglColorMask(1, 1, 1, 1);
        rglClearBufferfv(GL_COLOR, 0, c);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    R.frag_dirty = true; /* masks / scissor changed */
    R.st.fills++;
}

static bool dumping(void);
int render_fill(const void *start, uint32_t size, uint32_t value, int bits) {
    if (!start || !size || !render_ready()) return 0;
    lock();
    if (dumping()) rlog("dump: FILL %#lx +%#x value %#x bits %d", (unsigned long)(uintptr_t)start, size, value, bits);
    uintptr_t a = (uintptr_t)start;
    int done = 0;
    for (size_t i = 0; i < R.nsurf; i++) {
        Surface *s = R.surf[i];
        if (s->addr >= a && s->addr < a + size) { clear_surface(s, value, bits); done = 1; }
    }
    if (!done) {
        Surface *s = surface_at(a, -1);
        if (s) { clear_surface(s, value, bits); done = 1; }
    }
    if (!done) {
        PendingFill *f = &R.pend[R.pend_next++ % 16];
        f->start = a; f->end = a + size; f->value = value; f->bits = bits;
    }
    unlock();
    return done;
}

/* ================================================================== display images / transfers */

static Display *display_at(uintptr_t a) {
    for (size_t i = 0; i < R.ndisp; i++)
        if (a >= R.disp[i].mem && a < R.disp[i].mem + R.disp[i].bytes) return &R.disp[i];
    return NULL;
}

void render_display_register(const void *mem, size_t bytes, int w, int h, int fmt) {
    lock();
    Display *d = display_at((uintptr_t)mem);
    if (!d) {
        VEC_PUSH(R.disp, R.ndisp, R.capdisp);
        d = &R.disp[R.ndisp++];
        memset(d, 0, sizeof *d);
    }
    d->mem = (uintptr_t)mem; d->bytes = bytes; d->w = w; d->h = h; d->fmt = fmt; d->valid = false;
    unlock();
}

void render_display_unregister(const void *mem) {
    lock();
    for (size_t i = 0; i < R.ndisp; i++)
        if (R.disp[i].mem == (uintptr_t)mem) {
            if (R.disp[i].img && render_ready()) { surface_free(R.disp[i].img); free(R.disp[i].img); }
            R.disp[i] = R.disp[--R.ndisp];
            break;
        }
    unlock();
}

static Surface *display_image(Display *d, int w, int h, int fmt) {
    if (d->img && (d->img->w != w || d->img->h != h)) { surface_free(d->img); free(d->img); d->img = NULL; }
    if (!d->img) {
        d->img = calloc(1, sizeof *d->img);
        if (!d->img) abort();
        d->img->addr = d->mem; d->img->w = w; d->img->h = h; d->img->fmt = fmt;
        d->img->bytes = d->bytes;
        surface_alloc(d->img);
    }
    return d->img;
}

unsigned long g_render_draws;   /* draws executed (both paths; ALBW_PASS_TRACE) */
int render_transfer(const void *src, int sw, int sh, int sfmt, void *dst, int dw, int dh, int dfmt, int flip, int aa) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !render_ready()) return 0;
    lock();
    /* the source may start inside a larger colour surface: the game shares one render buffer
     * between the screens (sead FrameBufferCTR top 240x400 and bottom 240x320 both use the
     * 0x1F000080 buffer) and transfers the bottom screen from 80 rows into it */
    uintptr_t a = (uintptr_t)src;
    Surface *s = surface_at(a, 0);
    int off_rows = 0;
    if (s && s->addr != a) {
        size_t row_bytes = (size_t)s->w * (size_t)fmt_bytes(s->fmt);
        size_t off = a - s->addr;
        if (row_bytes && off % row_bytes == 0 && (int)(off / row_bytes) < s->h) off_rows = (int)(off / row_bytes);
        else s = NULL;
    }
    if (!s) s = surface_get(a, sw, sh, sfmt, false);
    Display *d = display_at((uintptr_t)dst);
    Surface *t = d ? display_image(d, dw, dh, dfmt) : surface_get((uintptr_t)dst, dw, dh, dfmt, false);
    {   /* debug ALBW_PASS_TRACE=1: each copy to a display with the draws since that display's last copy */
        static int tr = -1;
        static uintptr_t last_dst[4]; static unsigned long last_n[4];
        if (tr < 0) tr = getenv("ALBW_PASS_TRACE") != NULL;
        if (tr && d) {
            int k = 0;
            while (k < 4 && last_dst[k] && last_dst[k] != (uintptr_t)dst) k++;
            if (k < 4) {
                extern long g_plat_frames __attribute__((weak));
                if (!last_dst[k]) { last_dst[k] = (uintptr_t)dst; last_n[k] = g_render_draws; }
                static unsigned long last_any;
                fprintf(stderr, "pass: f=%ld copy %p+%d rows -> display %p %dx%d draws %lu since-any %lu vp %d,%d,%d,%d\n", &g_plat_frames ? g_plat_frames : 0L,
                        (void *)a, off_rows, dst, dw, dh, g_render_draws - last_n[k], g_render_draws - last_any, R.vp[0], R.vp[1], R.vp[2], R.vp[3]);
                last_n[k] = g_render_draws;
                last_any = g_render_draws;
            }
        }
    }
    int ok = 0;
    if (s && t && s != t) {
        int S = R.scale;
        /* AA downscale: 0 none, 1 2x horizontal, 2 2x2; the transfer reads only what the output
         * needs (input rows beyond dh * ay are not read) */
        int ax = aa >= 1 ? 2 : 1, ay = aa >= 2 ? 2 : 1;
        int bw = dw * ax, bh = dh * ay;
        if (bw > sw) bw = sw;
        if (bh > sh) bh = sh;
        if (bw > s->w) bw = s->w;
        if (bh > s->h - off_rows) bh = s->h - off_rows;
        int ow = bw / ax, oh = bh / ay;
        /* memory row m of the source = GL row s->h-1-m (top window row first in memory); the
         * destination keeps memory row order: source memory row off_rows -> destination row 0 */
        int sy1 = s->h - off_rows, sy0 = sy1 - bh;
        int dy1 = dh, dy0 = dh - oh;
        /* widescreen: a copy of the whole top buffer takes all its physical rows (into all of the
         * destination's); a part (bottom screen) stays 1:1. In logical units times S below. */
        float sy0f = (float)sy0, sy1f = (float)sy1, dy0f = (float)dy0, dy1f = (float)dy1;
        /* wide buffers: rows from the start of the top buffer (the top screen's pass, drawn
         * stretched) map in proportion; a part further in (the bottom screen's 240x320 at +80
         * rows of the shared buffer, drawn 1:1) does not */
        if (off_rows == 0 && sph(s) != s->h) { float k = (float)sph(s) / s->h; sy0f *= k; sy1f *= k; }
        if (sph(t) != t->h) { float k = (float)sph(t) / t->h; dy0f *= k; dy1f *= k; }
        if (flip) { float tmp = dy0f; dy0f = dy1f; dy1f = tmp; }
        glDisable(GL_SCISSOR_TEST);
        rglColorMask(1, 1, 1, 1);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
        rglBlitFramebuffer(0, (int)(sy0f * S), bw * S, (int)(sy1f * S), 0, (int)(dy0f * S), ow * S, (int)(dy1f * S), GL_COLOR_BUFFER_BIT,
                           (ax > 1 || ay > 1) ? GL_LINEAR : GL_NEAREST);
        {   /* debug ALBW_PASS_DUMP_DIR=<dir>: for copies to a display in host frames 300..310 (or
             * ALBW_PASS_DUMP_FRAMES=<a>-<b>), the source buffer and the display image as PPM files */
            static const char *dd = (const char *)1;
            static long fa = 300, fb = 310;
            if (dd == (const char *)1) {
                dd = getenv("ALBW_PASS_DUMP_DIR");
                const char *fr = getenv("ALBW_PASS_DUMP_FRAMES");
                if (fr) sscanf(fr, "%ld-%ld", &fa, &fb);
            }
            extern long g_plat_frames __attribute__((weak));
            long fn = &g_plat_frames ? g_plat_frames : 0;
            if (dd && d && fn >= fa && fn <= fb) {
                static int seq;
                const Surface *both[2] = {s, t};
                for (int k = 0; k < 2; k++) {
                    const Surface *x = both[k];
                    int w = x->w * S, h = sph(x) * S;
                    uint8_t *px = malloc((size_t)w * h * 4);
                    if (!px) break;
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, x->fbo);
                    rglReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    char path[512];
                    snprintf(path, sizeof path, "%s/pass_%04d_f%ld_%s_%08lx_%dx%d.ppm", dd, seq, fn, k ? "dst" : "src",
                             (unsigned long)(k ? (uintptr_t)dst : a), w, h);
                    FILE *fp = fopen(path, "wb");
                    if (fp) {
                        fprintf(fp, "P6\n%d %d\n255\n", w, h);
                        for (int y = h - 1; y >= 0; y--)
                            for (int xx = 0; xx < w; xx++) fwrite(px + ((size_t)y * w + xx) * 4, 1, 3, fp);
                        fclose(fp);
                    }
                    free(px);
                }
                seq++;
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
            }
        }
        /* The colour buffer at 0x1F000080 is shared by the screens: each frame the top screen is
         * drawn into it and copied to its display (twice, one per eye), and every other frame the
         * bottom screen is drawn into rows 80-400 and copied to the bottom display. Once a region
         * has been copied to a DISPLAY its contents are not needed any more: clear exactly that
         * region (colour 0, depth far, stencil 0), so the next pass starts clean even if its own
         * clear draw fails. Before, a pass that drew nothing (user slot 3, Eastern Palace 1F) left
         * the bottom screen's map in the buffer and it was copied to the top screen ("the items
         * menu keeps coming up"). Copies to other surfaces (captures used later in the frame) keep
         * it. ALBW_KEEP_AFTER_SCANOUT=1 turns this off. */
        if (d && s->fbo && !getenv("ALBW_KEEP_AFTER_SCANOUT")) {
            static const float black[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, (int)(sy0f * S), bw * S, (int)((sy1f - sy0f) * S));
            glDepthMask(GL_TRUE);
            rglClearBufferfv(GL_COLOR, 0, black);
            rglClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
            glDisable(GL_SCISSOR_TEST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
        R.frag_dirty = true;
        if (d) d->valid = true;
        /* The bottom screen's pass is a 320-row window into the shared buffer (copied from +80
         * rows). The top-screen passes expect the whole buffer and some draw most of their content
         * before setting viewport / scissor (the game's GraphicsContext emits those commands only
         * when its cached copy changes, 0048AD94 / 0048AD08), so the bottom's 240x320 scissor and
         * viewport leaked into the next top pass whenever the screens' lists interleaved that way:
         * an 80-pixel strip of the top screen went black every other frame (README: Android boot
         * stop A8). After the bottom screen's copy, both go back to the whole buffer.
         * ALBW_KEEP_BOTTOM_VP=1 turns this off. */
        if (d && off_rows > 0 && R.cur_c == s && !getenv("ALBW_KEEP_BOTTOM_VP")) {
            R.vp[0] = 0; R.vp[1] = 0; R.vp[2] = s->w; R.vp[3] = s->h;
            R.sc[0] = 0; R.sc[1] = 0; R.sc[2] = s->w; R.sc[3] = s->h;
            R.frag_dirty = true;
        }
        ok = 1;
        R.st.transfers++;
        if (g_trace || dumping()) rlog("transfer %#lx (+%d rows of %#lx) %dx%d -> %s %#lx %dx%d flip %d aa %d", (unsigned long)a,
                          off_rows, (unsigned long)s->addr, sw, sh, d ? "display" : "surface",
                          (unsigned long)(uintptr_t)dst, dw, dh, flip, aa);
    }
    unlock();
    return ok;
}

void render_block_copy(const void *src, void *dst, uint32_t size) {
    render_mem_written(dst, size);
    if (!render_ready()) return;
    lock();
    Surface *s = surface_at((uintptr_t)src, 0);
    if (s && s->addr == (uintptr_t)src && size >= s->bytes / 2) {
        Display *d = display_at((uintptr_t)dst);
        Surface *t = d ? display_image(d, s->w, s->h, s->fmt) : surface_get((uintptr_t)dst, s->w, s->h, s->fmt, false);
        if (t && t != s) {
            glDisable(GL_SCISSOR_TEST);
            rglColorMask(1, 1, 1, 1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
            rglBlitFramebuffer(0, 0, s->w * R.scale, sph(s) * R.scale, 0, 0, t->w * R.scale, sph(t) * R.scale,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
            R.frag_dirty = true;
            if (d) d->valid = true;
        }
    }
    unlock();
}

int render_scanout(int screen, const void *mem) {
    if (!mem || !render_ready()) return 0;
    lock();
    Display *d = display_at((uintptr_t)mem);
    int ok = 0;
    if (d && d->valid && d->img) {
        GLint fbo = 0;
        glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
        int sw, sh;
        plat_screen_size(screen, &sw, &sh);
        glViewport(0, 0, sw, sh);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_COLOR_LOGIC_OP);
        glDisable(GL_STENCIL_TEST);
        rglColorMask(1, 1, 1, 1);
        glUseProgram(R.scan_prog);
        int rot = R.scanout_mode == 1 ? 0 : R.scanout_mode == 2 ? 1 : R.scanout_mode == 3 ? 2
                : d->img->w < d->img->h ? 1 : 0; /* portrait buffer = rotated LCD */
        glUniform1i(R.scan_rot, rot);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->img->tex);
        glBindVertexArray(R.vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        R.frag_dirty = true;
        R.tev_dirty = true;
        ok = 1;
    }
    unlock();
    return ok;
}

int render_read_surface(uintptr_t color_addr, uint8_t *rgba, int *w, int *h) {
    if (!render_ready()) return 0;
    lock();
    Surface *s = surface_at(color_addr, 0);
    Display *d = s ? NULL : display_at(color_addr);
    if (!s && d && d->img) s = d->img;
    int ok = 0;
    if (s) {
        *w = s->w * R.scale;
        *h = sph(s) * R.scale;
        if (rgba) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
            rglReadPixels(0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
        }
        ok = 1;
    }
    unlock();
    return ok;
}

/* ================================================================== textures */

static uint64_t hash_bytes(const void *p, size_t n) {
    const uint64_t *q = p;
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) h = (h ^ q[i / 8]) * 0x100000001b3ull;
    for (; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

void render_mem_written(const void *addr, size_t size) {
    if (!addr || !size) return;
    if (R.ext.mem) R.ext.mem((uintptr_t)addr, size);
    uintptr_t a = (uintptr_t)addr, e = a + size;
    lock();
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("ALBW_SURF_WRITE_TRACE") != NULL;
    if (dbg && a >= 0x1F000000u && a < 0x1F600000u)
        rlog("vram-write %#lx +%#zx frame %u", (unsigned long)a, size, (unsigned)R.frame);
    if (dbg)
        for (size_t i = 0; i < R.nsurf; i++) {
            Surface *s = R.surf[i];
            if (s->addr < e && s->addr + s->bytes > a) {
                const uint32_t *w = (const uint32_t *)addr;
                size_t n = size / 4, same = 1;
                for (size_t k = 1; k < n; k++) if (w[k] != w[0]) { same = 0; break; }
                rlog("surf-write %#lx +%#zx into %s surface %#lx (%dx%d) first %08x uniform %zu frame %u",
                     (unsigned long)a, size, s->depth ? "depth" : "colour", (unsigned long)s->addr, s->w, s->h, n ? w[0] : 0,
                     same, (unsigned)R.frame);
            }
        }
    for (size_t i = 0; i < R.ntex; i++) {
        TexEnt *t = &R.tex[i];
        size_t bytes = texfmt_size(t->fmt, t->w, t->h) * (t->cube ? 1 : 2); /* + mip chain bound */
        if (t->cube) {
            for (int f = 0; f < 6; f++)
                if (t->faces[f] < e && t->faces[f] + bytes > a) t->dirty = true;
        } else if (t->addr < e && t->addr + bytes > a) {
            t->dirty = true;
        }
    }
    unlock();
}

static size_t level_bytes(int fmt, int w, int h, int levels) {
    size_t n = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 8 || lh < 8) break;
        n += texfmt_size(fmt, lw, lh);
    }
    return n;
}

static void tex_upload(TexEnt *t) {
    static uint8_t *buf;
    static size_t cap;
    size_t need = (size_t)t->w * t->h * 4;
    if (need > cap) { buf = xrealloc(buf, need); cap = need; }
    GLenum target = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(target, t->tex);
    int nfaces = t->cube ? 6 : 1;
    int levels = 0;
    for (int f = 0; f < nfaces; f++) {
        const uint8_t *src = (const uint8_t *)(t->cube ? t->faces[f] : t->addr);
        if (!src) continue;
        int l = 0;
        for (; l < t->levels; l++) {
            int lw = t->w >> l, lh = t->h >> l;
            if (lw < 8 || lh < 8) break;
            if (tex_decode(t->fmt, src, lw, lh, buf)) break;
            {   /* debug: ALBW_TEX_DEBUG=1: every texture becomes a flat colour unique to its address, with an
                   8-texel checker (UV scale), a red band on the top rows and a blue band on the left
                   columns (orientation); alpha kept so cut-outs / masks still apply */
                static int td = -1;
                if (td < 0) td = getenv("ALBW_TEX_DEBUG") != NULL;
                if (td) {
                    uint32_t hsh = (uint32_t)(t->addr * 2654435761u) ^ (uint32_t)(t->addr >> 13);
                    uint8_t cr = (uint8_t)(64 + (hsh & 0x7F)), cg = (uint8_t)(64 + (hsh >> 8 & 0x7F)), cb = (uint8_t)(64 + (hsh >> 16 & 0x7F));
                    int band = lh / 16 > 1 ? lh / 16 : 1, bandx = lw / 16 > 1 ? lw / 16 : 1;
                    for (int y = 0; y < lh; y++)          /* buf rows: row 0 = bottom (image top = row lh-1) */
                        for (int x = 0; x < lw; x++) {
                            uint8_t *px = buf + ((size_t)y * lw + x) * 4;
                            int top = (lh - 1 - y) < band, left = x < bandx;
                            int chk = (((x >> (3 - (l < 3 ? l : 3))) ^ (y >> (3 - (l < 3 ? l : 3)))) & 1);
                            uint8_t r = cr, g = cg, b = cb;
                            if (chk) { r = (uint8_t)(r * 3 / 4); g = (uint8_t)(g * 3 / 4); b = (uint8_t)(b * 3 / 4); }
                            if (top) { r = 255; g = 0; b = 0; } else if (left) { r = 0; g = 0; b = 255; }
                            px[0] = r; px[1] = g; px[2] = b;
                        }
                }
            }
            if (l == 0 && f == 0) { /* debug: ALBW_TEX_DUMP_DIR=<dir>: every decoded texture (level 0) as a PAM image */
                static const char *dd = (const char *)1;
                if (dd == (const char *)1) dd = getenv("ALBW_TEX_DUMP_DIR");
                if (dd) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/tex_%08lx_%dx%d_f%d.pam", dd, (unsigned long)t->addr, lw, lh, t->fmt);
                    FILE *fp = fopen(path, "wb");
                    if (fp) {
                        fprintf(fp, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", lw, lh);
                        for (int y = lh - 1; y >= 0; y--) fwrite(buf + (size_t)y * lw * 4, 4, (size_t)lw, fp); /* rows top first */
                        fclose(fp);
                    }
                }
            }
            {   /* experiment: ALBW_TEX_FLIPV=1 flips every 2D texture vertically */
                static int fv = -1;
                if (fv < 0) fv = getenv("ALBW_TEX_FLIPV") != NULL;
                if (fv && !t->cube)
                    for (int y = 0; y < lh / 2; y++) {
                        uint8_t *r0 = buf + (size_t)y * lw * 4, *r1 = buf + (size_t)(lh - 1 - y) * lw * 4;
                        for (int x = 0; x < lw * 4; x++) { uint8_t tmp = r0[x]; r0[x] = r1[x]; r1[x] = tmp; }
                    }
            }
            glTexImage2D(t->cube ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f) : GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, buf);
            src += texfmt_size(t->fmt, lw, lh);
        }
        levels = l;
    }
    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels > 0 ? levels - 1 : 0);
    t->up_levels = levels > 0 ? levels : 1;
    {   /* experiment: ALBW_TEX_GENMIP=1 replaces the stored mip levels with GL-generated ones */
        static int gm = -1;
        if (gm < 0) gm = getenv("ALBW_TEX_GENMIP") != NULL;
        if (gm && levels > 1) {
            static void (*genmip)(GLenum);
            if (!genmip) genmip = (void (*)(GLenum))SDL_GL_GetProcAddress("glGenerateMipmap");
            if (genmip) genmip(target);
        }
    }
    t->dirty = false;
    R.st.tex_uploads++;
}

static TexEnt *tex_lookup(const RTexUnit *u) {
    bool cube = u->type == 1 || u->type == 4;
    int levels = (u->min == 0 || u->min == 3) ? 1 : u->max_level + 1;
    if (levels < 1) levels = 1;
    for (size_t i = 0; i < R.ntex; i++) {
        TexEnt *t = &R.tex[i];
        if (t->addr == u->addr && t->w == u->w && t->h == u->h && t->fmt == u->fmt && t->cube == cube &&
            (!cube || !memcmp(t->faces, u->cube, sizeof t->faces))) {
            if (t->levels < levels) { t->levels = levels; t->dirty = true; }
            return t;
        }
    }
    {   /* debug: ALBW_TEX_TRACE=1: every texture's first use with its sampler request */
        static int tt = -1;
        if (tt < 0) tt = getenv("ALBW_TEX_TRACE") != NULL;
        if (tt) rlog("tex %#lx %dx%d fmt %d%s min %d mag %d max_level %d -> levels %d lod_bias %.2f wrap %d/%d",
                     (unsigned long)u->addr, u->w, u->h, u->fmt, cube ? " cube" : "", u->min, u->mag, u->max_level,
                     levels, (double)u->lod_bias, u->wrap_s, u->wrap_t);
    }
    VEC_PUSH(R.tex, R.ntex, R.captex);
    TexEnt *t = &R.tex[R.ntex++];
    memset(t, 0, sizeof *t);
    t->addr = u->addr; t->w = u->w; t->h = u->h; t->fmt = u->fmt; t->cube = cube; t->levels = levels;
    memcpy(t->faces, u->cube, sizeof t->faces);
    t->p_wrap_s = t->p_wrap_t = t->p_min = t->p_mag = -1;
    t->p_maxl = t->p_minl = -1;
    glGenTextures(1, &t->tex);
    t->dirty = true;
    return t;
}

static const GLenum WRAP[4] = {GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_REPEAT, GL_MIRRORED_REPEAT};
static const GLenum MINF[6] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR,
                               GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR};

static void sampler_params(GLenum target, const RTexUnit *u, int *ws, int *wt, int *mi, int *ma, int levels) {
    int s = u->wrap_s & 3, t = u->wrap_t & 3, m = u->min < 6 ? u->min : 3, g = u->mag & 1;
    { /* experiment: ALBW_TEX_FORCE_LINEAR=1 samples every texture bilinear (+ linear mips) */
        static int fl = -1;
        if (fl < 0) fl = getenv("ALBW_TEX_FORCE_LINEAR") != NULL;
        if (fl) { m = levels > 1 ? 5 : 3; g = 1; }
    }
    if (levels <= 1) m = (m >= 3) ? 3 : 0; /* no mip chain: drop the mip part */
    if (*ws != s) { glTexParameteri(target, GL_TEXTURE_WRAP_S, (GLint)WRAP[s]); *ws = s; }
    if (*wt != t) { glTexParameteri(target, GL_TEXTURE_WRAP_T, (GLint)WRAP[t]); *wt = t; }
    if (*mi != m) { glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)MINF[m]); *mi = m; }
    if (*ma != g) { glTexParameteri(target, GL_TEXTURE_MAG_FILTER, g ? GL_LINEAR : GL_NEAREST); *ma = g; }
    float bc[4] = {u->border[0] / 255.f, u->border[1] / 255.f, u->border[2] / 255.f, u->border[3] / 255.f};
    if (s == 1 || t == 1) rglTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, bc);
    {   /* experiment: ALBW_TEX_LODBIAS=<f> added to every texture's LOD bias */
        static float xb = -1e9f;
        if (xb == -1e9f) { const char *e = getenv("ALBW_TEX_LODBIAS"); xb = e ? (float)atof(e) : 0.0f; }
        rglTexParameterf(target, GL_TEXTURE_LOD_BIAS, u->lod_bias + xb);
    }
}

/* bind texture unit i for the next draw */
static void bind_unit(int i) {
    const RTexUnit *u = &R.unit[i];
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    if (!(R.tex_enable >> i & 1) || !u->addr || u->type == 5 || !u->w || !u->h) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    bool cube = u->type == 1 || u->type == 4;
    if (!cube) {
        Surface *s = surface_at(u->addr, 0); /* render-to-texture */
        if (!s) s = surface_at(u->addr, 1);
        if (s && s->addr == u->addr) {
            glBindTexture(GL_TEXTURE_2D, s->tex);
            int a = -1, b = -1, c = -1, d = -1;
            sampler_params(GL_TEXTURE_2D, u, &a, &b, &c, &d, 1);
            return;
        }
    }
    TexEnt *t = tex_lookup(u);
    t->used_frame = R.frame;
    if (!t->dirty && t->checked_frame != R.frame) {
        /* CPU-written textures (font sheets...) are not announced: re-hash small ones once a frame */
        size_t n = level_bytes(t->fmt, t->w, t->h, 1);
        static int all = -1;   /* ALBW_TEX_REHASH_ALL=1: re-hash every texture once a frame (test) */
        if (all < 0) all = getenv("ALBW_TEX_REHASH_ALL") != NULL;
        if (!cube && (all || n <= 256 * 1024)) {
            uint64_t h = hash_bytes((const void *)t->addr, n);
            if (h != t->hash) { t->hash = h; t->dirty = true; }
        }
        t->checked_frame = R.frame;
    }
    if (t->dirty) {
        if (!cube) t->hash = hash_bytes((const void *)t->addr, level_bytes(t->fmt, t->w, t->h, 1));
        tex_upload(t);
        t->checked_frame = R.frame;
        t->p_wrap_s = t->p_wrap_t = t->p_min = t->p_mag = -1;
        t->p_maxl = t->p_minl = -1;
    }
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    if (cube) {
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_CUBE_MAP, t->tex);
    } else {
        glBindTexture(GL_TEXTURE_2D, t->tex);
    }
    /* This draw's own mip range. A texture is cached once per (address, size, format) and its
       levels are uploaded up to the largest max level any sampler asked for; on the PICA every
       draw clamps sampling to its own LOD range (TEXUNIT LOD register: max level, min level), so
       apply it per binding. Without this a model appearing with a longer mip chain changed the
       sampled levels of every other draw using the texture: the title flyover's sudden texture /
       tint jumps (BOOT_LOG 98). */
    int want = (u->min == 0 || u->min == 3) ? 1 : u->max_level + 1;
    int have = t->up_levels > 0 && t->up_levels < t->levels ? t->up_levels : t->levels;   /* never past the uploaded levels: */
    int eff = want < have ? want : have;                                                    /* mipmap-incomplete samples black */
    if (eff < 1) eff = 1;
    int minl = u->min_level < eff ? u->min_level : eff - 1;
    if (t->p_maxl != eff - 1) { glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, eff - 1); t->p_maxl = eff - 1; }
    if (t->p_minl != minl) { rglTexParameterf(target, GL_TEXTURE_MIN_LOD, (float)minl); t->p_minl = minl; }
    sampler_params(target, u, &t->p_wrap_s, &t->p_wrap_t, &t->p_min, &t->p_mag, eff);
}

/* ================================================================== pipeline state */

static void reset_state(void) {
    memset(&R.frag, 0, sizeof R.frag);
    R.frag.color_mask = 15;
    R.frag.depth_func = 1;
    R.frag.alpha_func = 1;
    R.frag.stencil_func = 1;
    R.frag.src_rgb = R.frag.src_a = 1;
    R.frag.depth_near = 0.f;
    R.frag.depth_far = 1.f;
    R.frag_dirty = true;
    memset(&R.tev, 0, sizeof R.tev);
    for (int i = 0; i < 6; i++) {
        RTevStage *s = &R.tev.stage[i];
        /* default combiner: stage 0 = primary colour, others pass the previous result */
        s->src_rgb[0] = s->src_a[0] = i == 0 ? 0 : 15;
    }
    R.tev_dirty = true;
    R.tex_enable = 0;
    R.prog_id = RPROG_CLIP;
    R.prog_variant = 0;
    fraglight_reset();
    for (int i = 0; i < RENDER_MAX_ATTRIBS; i++) {
        memset(&R.attr[i], 0, sizeof R.attr[i]);
        R.attr[i].k[3] = 1;
    }
    R.vsf_dirty = true;
}

static const GLenum TESTF[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
static const GLenum BEQ[5] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
static const GLenum BF[15] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR,
                              GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                              GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                              GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE};
static const GLenum SOP[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
/* PICA logic op order -> GL (GL_CLEAR + n) */
static const uint8_t LOGIC[16] = {0x0, 0x1, 0x2, 0x3, 0xF, 0xC, 0x5, 0xA, 0xE, 0x7, 0x8, 0x6, 0x9, 0x4, 0xB, 0xD};

static void apply_frag(void) {
    const RFragState *f = &R.frag;
    unsigned cmask = f->color_mask;
    if (f->logic_enable) {
        glDisable(GL_BLEND);
#if defined(__ANDROID__)
        /* GLES has no logic ops: the ones with an exact equivalent. NOOP (PICA 6) is what the
         * stencil shadow volumes use (mark the stencil, keep the colour): drawn as colour, they were
         * the solid black blobs under characters and signs (README: Android stop A10) */
        switch (f->logic_op & 15) {
        case 6: cmask = 0; break;                                  /* noop: no colour write */
        case 3: break;                                             /* copy: plain write */
        case 0:                                                    /* clear: 0 */
            glEnable(GL_BLEND);
            rglBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            rglBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ZERO, GL_ZERO);
            break;
        default: {
            static unsigned warned;
            if (!(warned >> (f->logic_op & 15) & 1)) {
                warned |= 1u << (f->logic_op & 15);
                rlog("GL: logic op %d has no GLES equivalent here: drawn as copy", f->logic_op & 15);
            }
            break;
        }
        }
#else
        glEnable(GL_COLOR_LOGIC_OP);
        rglLogicOp(GL_CLEAR + LOGIC[f->logic_op & 15]);
#endif
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
        if (f->blend_enable) {
            glEnable(GL_BLEND);
            rglBlendEquationSeparate(BEQ[f->eq_rgb < 5 ? f->eq_rgb : 0], BEQ[f->eq_a < 5 ? f->eq_a : 0]);
            rglBlendFuncSeparate(BF[f->src_rgb % 15], BF[f->dst_rgb % 15], BF[f->src_a % 15], BF[f->dst_a % 15]);
            rglBlendColor(f->blend_color[0] / 255.f, f->blend_color[1] / 255.f, f->blend_color[2] / 255.f,
                          f->blend_color[3] / 255.f);
        } else {
            glDisable(GL_BLEND);
        }
    }
    /* PICA writes depth whenever depth writes are on, even with the test off */
    if (f->depth_test || f->depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(f->depth_test ? TESTF[f->depth_func & 7] : GL_ALWAYS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(f->depth_write ? GL_TRUE : GL_FALSE);
    rglColorMask(cmask & 1, cmask >> 1 & 1, cmask >> 2 & 1, cmask >> 3 & 1);
    if (f->stencil_test) {
        glEnable(GL_STENCIL_TEST);
        rglStencilFunc(TESTF[f->stencil_func & 7], f->stencil_ref, f->stencil_mask);
        rglStencilOp(SOP[f->stencil_fail & 7], SOP[f->stencil_zfail & 7], SOP[f->stencil_zpass & 7]);
        rglStencilMask(f->stencil_write);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (f->cull) {
        glEnable(GL_CULL_FACE);
        rglFrontFace(GL_CCW);
        {   /* experiment: ALBW_CULL_FLIP=1 swaps the culled side */
            static int cf = -1;
            if (cf < 0) cf = getenv("ALBW_CULL_FLIP") != NULL;
            rglCullFace((f->cull == 1) != (cf == 1) ? GL_FRONT : GL_BACK);
        }
    } else {
        glDisable(GL_CULL_FACE);
    }
    float n = f->depth_near, fa = f->depth_far;
    if (n == fa) { n = 0; fa = 1; }
    rglDepthRange(n < 0 ? 0 : n > 1 ? 1 : n, fa < 0 ? 0 : fa > 1 ? 1 : fa);
    if (f->poly_offset_enable) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        {   /* experiment: ALBW_POLYOFS_SCALE=<f> multiplies every depth offset (z-fighting tests) */
            static float ps = -1e9f;
            if (ps == -1e9f) { const char *e = getenv("ALBW_POLYOFS_SCALE"); ps = e ? (float)atof(e) : 1.0f; }
            rglPolygonOffset(0.f, f->poly_offset_units * ps);
        }
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    /* widescreen: a viewport over all 400 rows of a top buffer covers its physical rows; the
     * scissor follows the same stretch */
    const Surface *vs = R.cur_c ? R.cur_c : R.cur_d;
    /* every pass over the whole buffer is stretched; the 3D projections are widened to match
     * (0057F6C0), and the layout shader (font.c, u_ws) draws HUD panes and text 1:1 and centred
     * inside the stretch, while panes that cover the whole width (fades, backgrounds) fill it */
    float fy = 1.0f, oy = 0.0f;
    if (vs && sph(vs) != vs->h && R.vp[1] == 0 && R.vp[3] == vs->h) fy = (float)sph(vs) / (float)vs->h;
    g_cur_fy = fy;
    if (R.scissor) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(R.sc[0] * R.scale, (int)((R.sc[1] * fy + oy) * R.scale), R.sc[2] * R.scale, (int)(R.sc[3] * R.scale * fy + 0.5f));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glViewport(R.vp[0] * R.scale, (int)((R.vp[1] * fy + oy) * R.scale), R.vp[2] * R.scale, (int)(R.vp[3] * R.scale * fy + 0.5f));
    R.frag_dirty = false;
}

/* ================================================================== internal resolution */

/* Re-create one surface at the current scale, its old contents stretched into the new texture
 * (render targets the game reuses across frames, such as the screen capture behind the pause
 * menus, keep their picture). `old` = the scale the surface was made with. */
static void surface_rescale(Surface *s, int old) {
    if (!s->tex) return;
    int oph = sph(s);
    GLuint otex = s->tex, ofbo = s->fbo;
    s->tex = s->fbo = 0;
    surface_alloc(s);
    R.st.surfaces--;
    glDisable(GL_SCISSOR_TEST);
    rglColorMask(1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ofbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
    if (s->depth) {
        glDepthMask(GL_TRUE);
        rglStencilMask(0xFF);
        rglBlitFramebuffer(0, 0, s->w * old, oph * old, 0, 0, s->w * R.scale, sph(s) * R.scale,
                           GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    } else {
        rglBlitFramebuffer(0, 0, s->w * old, oph * old, 0, 0, s->w * R.scale, sph(s) * R.scale,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    rglDeleteFramebuffers(1, &ofbo);
    glDeleteTextures(1, &otex);
}

int render_get_scale(void) { return R.scale; }

/* Change the internal resolution (1..8 x the 3DS screens) between frames: every surface and
 * display image is re-created at the new size, the combined colour+depth FBOs rebuilt on use. */
void render_set_scale(int scale) {
    if (scale < 1 || scale > 8 || !render_ready()) return;
    lock();
    int old = R.scale;
    if (scale != old) {
        R.scale = scale;
        for (size_t i = 0; i < R.nfbo; i++) rglDeleteFramebuffers(1, &R.fbos[i].fbo);
        R.nfbo = 0;
        R.cur_fbo = 0;   /* may have been one of those: surface_alloc rebinds R.cur_fbo */
        for (size_t i = 0; i < R.nsurf; i++) surface_rescale(R.surf[i], old);
        for (size_t i = 0; i < R.ndisp; i++)
            if (R.disp[i].img) surface_rescale(R.disp[i].img, old);
        R.cur_fbo = fbo_for(R.cur_c, R.cur_d);
        glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
        R.frag_dirty = R.tev_dirty = R.vsf_dirty = true;
        rlog("internal scale %d -> %d", old, scale);
    }
    unlock();
}

/* Widescreen: rows = the top screen's long side in 3DS pixels (400 = off; 427 = 16:9). The
 * top-shaped surfaces and display images are re-created (contents stretched across). */
void render_set_widescreen(int rows) {
    if (rows < 400) rows = 400;
    if (rows > 800) rows = 800;
    if (!render_ready()) { g_ws_rows = rows; return; }
    lock();
    if (rows != g_ws_rows) {
        g_ws_rows = rows;
        for (size_t i = 0; i < R.nfbo; i++) rglDeleteFramebuffers(1, &R.fbos[i].fbo);
        R.nfbo = 0;
        R.cur_fbo = 0;
        for (size_t i = 0; i < R.nsurf; i++)
            if (surf_wide_shape(R.surf[i])) surface_rescale(R.surf[i], R.scale);
        for (size_t i = 0; i < R.ndisp; i++)
            if (R.disp[i].img && surf_wide_shape(R.disp[i].img)) surface_rescale(R.disp[i].img, R.scale);
        R.cur_fbo = fbo_for(R.cur_c, R.cur_d);
        glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
        R.frag_dirty = R.tev_dirty = R.vsf_dirty = true;
        rlog("widescreen: top screen %d x 240", rows);
    }
    unlock();
}

/* ================================================================== immediate API */


void render_set_target(uintptr_t color, int cfmt, uintptr_t depth, int dfmt, int w, int h) {
    if (!render_ready()) return;
    lock();
    Surface *c = color ? surface_get(color, w, h, cfmt, false) : R.cur_c;
    Surface *d = depth ? surface_get(depth, w, h, dfmt, true) : R.cur_d;
    if (color) R.cur_c = c;
    if (depth) R.cur_d = d;
    R.cur_fbo = fbo_for(R.cur_c, R.cur_d);
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    R.frag_dirty = true;
    unlock();
}

void render_set_viewport(int x, int y, int w, int h) {
    {   /* debug ALBW_PASS_TRACE: viewport changes with the draw count */
        static int tr = -1; if (tr < 0) tr = getenv("ALBW_PASS_TRACE") != NULL;
        extern long g_plat_frames __attribute__((weak));
        if (tr && (R.vp[0] != x || R.vp[1] != y || R.vp[2] != w || R.vp[3] != h))
            fprintf(stderr, "pass: f=%ld viewport %d,%d,%d,%d draws %lu\n", &g_plat_frames ? g_plat_frames : 0L, x, y, w, h, g_render_draws);
    }
    R.vp[0] = x; R.vp[1] = y; R.vp[2] = w; R.vp[3] = h;
    R.frag_dirty = true;
}

void render_set_scissor(int enable, int x, int y, int w, int h) {
    R.scissor = enable != 0;
    R.sc[0] = x; R.sc[1] = y; R.sc[2] = w > 0 ? w : 0; R.sc[3] = h > 0 ? h : 0;
    R.frag_dirty = true;
}

RFragState *render_frag_state(void) { return &R.frag; }
void render_frag_dirty(void) { R.frag_dirty = true; }

void render_set_texunit(int unit, const RTexUnit *t) {
    if (unit < 0 || unit > 2) return;
    R.unit[unit] = *t;
}
void render_set_texconfig(int enable_mask, int tex2_from_tc1) {
    R.tex_enable = enable_mask & 7;
    R.tex2_tc1 = tex2_from_tc1 != 0;
}
void render_set_tev(const RTev *t) { R.tev = *t; R.tev_dirty = true; }
void render_set_tev_const(int mask, const uint8_t rgba[4]) {
    for (int i = 0; i < 6; i++)
        if (mask >> i & 1) memcpy(R.tev.stage[i].konst, rgba, 4);
    R.tev_dirty = true;
}
void render_set_tev_stages(int mask, const RTev *t, int flags) {
    for (int i = 0; i < 6; i++)
        if (mask >> i & 1) R.tev.stage[i] = t->stage[i];
    if (flags & 1) { R.tev.buf_update_rgb = t->buf_update_rgb; R.tev.buf_update_a = t->buf_update_a; }
    if (flags & 2) memcpy(R.tev.buf_color, t->buf_color, 4);
    R.tev_dirty = true;
}

/* ================================================================== specialised fragment programs
 * The generic fragment program (FS_TEV) evaluates the 6 combiner stages, their source / operand /
 * mode selections and the alpha test from uniforms, for every pixel. On Adreno (Android) that cost
 * ~10x the rest of the frame: the title at render scale 2 ran 28 fps with the GPU at 99%, 60 fps
 * at 25% with a trivial shader (README: Android performance). Like the 3DS emulators, each draw
 * instead uses a program with its fragment configuration baked in as straight-line GLSL (same
 * maths as FS_TEV, constants instead of selections), compiled on first use and cached. Colours,
 * constants, the alpha reference and fog colour stay uniforms. ALBW_SPEC_SHADERS=0: generic
 * program (the default on the PC, where the GPU copes; =1 to enable there). */
#define SPEC_KEY_WORDS 32   /* 0-27 combiner / texture / alpha / fog, 28-31 lighting (fraglight_spec_key) */
typedef struct SpecEnt { uint32_t key[SPEC_KEY_WORDS]; const Program *base; Program prog; struct UniShadow_ ush; struct SpecEnt *next; } SpecEnt;
static SpecEnt *g_spec_tab[1024];
static unsigned g_spec_count;
unsigned long render_spec_builds;

static int spec_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("ALBW_SPEC_SHADERS");
#if defined(__ANDROID__)
        on = !e || strcmp(e, "0");
#else
        on = e && strcmp(e, "0");
#endif
    }
    return on;
}

/* the fragment configuration of the next draw (same values upload_uniforms sends) */
static int fog_bind(void);
static void spec_key(uint32_t *k) {
    memset(k, 0, SPEC_KEY_WORDS * sizeof *k);
    for (int i = 0; i < 6; i++) {
        const RTevStage *t = &R.tev.stage[i];
        k[i * 4 + 0] = (uint32_t)(t->src_rgb[0] | t->src_rgb[1] << 4 | t->src_rgb[2] << 8) | (uint32_t)(t->src_a[0] | t->src_a[1] << 4 | t->src_a[2] << 8) << 12;
        k[i * 4 + 1] = (uint32_t)(t->op_rgb[0] | t->op_rgb[1] << 4 | t->op_rgb[2] << 8) | (uint32_t)(t->op_a[0] | t->op_a[1] << 4 | t->op_a[2] << 8) << 12;
        k[i * 4 + 2] = (uint32_t)(t->combine_rgb & 0xFF) | (uint32_t)(t->combine_a & 0xFF) << 8;
        k[i * 4 + 3] = (uint32_t)(t->scale_rgb & 0xFF) | (uint32_t)(t->scale_a & 0xFF) << 8;
    }
    k[24] = (uint32_t)(R.tev.buf_update_rgb & 0xF) | (uint32_t)(R.tev.buf_update_a & 0xF) << 4;
    int at_on = R.frag.alpha_test, at_fn = R.frag.alpha_func;
    {   static const char *ef = (const char *)1;
        if (ef == (const char *)1) ef = getenv("ALBW_EFT_FRAG");
        if (ef && strchr(ef, 'a') && R.prog_id == RPROG_EFT) at_on = 0;
    }
    k[25] = (uint32_t)(at_on ? 1 : 0) | (uint32_t)(at_fn & 7) << 1;
    int t0type = (R.unit[0].type == 1 || R.unit[0].type == 4) ? 1 : R.unit[0].type == 3 ? 2 : 0;
    k[26] = (uint32_t)(R.tex_enable & 7) | (uint32_t)(R.tex2_tc1 ? 1 : 0) << 3 | (uint32_t)t0type << 4;
    int fog_on = R.fog.mode == 5 && !(&g_dbg_fog_off && g_dbg_fog_off) && fog_bind();
    k[27] = (uint32_t)(fog_on ? 1 : 0) | (uint32_t)(R.fog.flip ? 1 : 0) << 1;
}
static void spec_key_fl(uint32_t *k) { fraglight_spec_key(k + 28); }

static void spec_src(char *o, size_t n, int s, int stage) {
    switch (s & 15) {
    case 0: snprintf(o, n, "vc"); break;
    case 1: snprintf(o, n, "fprim"); break;
    case 2: snprintf(o, n, "fsec"); break;
    case 3: snprintf(o, n, "t0"); break;
    case 4: snprintf(o, n, "t1"); break;
    case 5: snprintf(o, n, "t2"); break;
    case 13: snprintf(o, n, "buf"); break;
    case 14: snprintf(o, n, "tev_const[%d]", stage); break;
    case 15: snprintf(o, n, "prev"); break;
    default: snprintf(o, n, "vec4(0.0)"); break;
    }
}
static void spec_op_rgb(char *o, size_t n, int op, const char *v) {
    switch (op & 15) {
    case 1: snprintf(o, n, "(1.0 - %s.rgb)", v); break;
    case 2: snprintf(o, n, "vec3(%s.a)", v); break;
    case 3: snprintf(o, n, "vec3(1.0 - %s.a)", v); break;
    case 4: snprintf(o, n, "vec3(%s.r)", v); break;
    case 5: snprintf(o, n, "vec3(1.0 - %s.r)", v); break;
    case 8: snprintf(o, n, "vec3(%s.g)", v); break;
    case 9: snprintf(o, n, "vec3(1.0 - %s.g)", v); break;
    case 12: snprintf(o, n, "vec3(%s.b)", v); break;
    case 13: snprintf(o, n, "vec3(1.0 - %s.b)", v); break;
    default: snprintf(o, n, "%s.rgb", v); break;
    }
}
static void spec_op_a(char *o, size_t n, int op, const char *v) {
    static const char *f[8] = {"%s.a", "(1.0 - %s.a)", "%s.r", "(1.0 - %s.r)", "%s.g", "(1.0 - %s.g)", "%s.b", "(1.0 - %s.b)"};
    snprintf(o, n, (op & 15) < 8 ? f[op & 15] : "%s.a", v);
}
static void spec_cmb(char *o, size_t n, int c, const char *a, const char *b, const char *d, int alpha) {
    switch (c) {
    case 1: snprintf(o, n, "(%s * %s)", a, b); break;
    case 2: snprintf(o, n, "min(%s + %s, 1.0)", a, b); break;
    case 3: snprintf(o, n, "clamp(%s + %s - 0.5, 0.0, 1.0)", a, b); break;
    case 4: snprintf(o, n, "(%s * %s + %s * (1.0 - %s))", a, d, b, d); break;
    case 5: snprintf(o, n, "max(%s - %s, 0.0)", a, b); break;
    case 6: case 7:
        if (alpha) snprintf(o, n, "%s", a);
        else snprintf(o, n, "vec3(clamp(4.0 * dot(%s - 0.5, %s - 0.5), 0.0, 1.0))", a, b);
        break;
    case 8: snprintf(o, n, "min(%s * %s + %s, 1.0)", a, b, d); break;
    case 9: snprintf(o, n, "(min(%s + %s, 1.0) * %s)", a, b, d); break;
    default: snprintf(o, n, "%s", a); break;
    }
}

static char *fs_source_spec(bool fl, const uint32_t *k) {
    char *flsrc = fl ? fraglight_spec_glsl(k + 28) : NULL;   /* lighting with its configuration baked in */
    size_t cap = 16384 + strlen(FS_HEAD) + (flsrc ? strlen(flsrc) : 0);
    char *s = xrealloc(NULL, cap);
    size_t len = (size_t)snprintf(s, cap, "#version 330 core\n#define FL %d\n#define DBGOUT 0\n%s%s"
        "vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), fprim = vec4(0.0), fsec = vec4(0.0);\n"
        "void main(){\n  vec4 vc = clamp(v_color, 0.0, 1.0);\n", fl ? 1 : 0, FS_HEAD, flsrc ? flsrc : "");
    free(flsrc);
#define EMIT(...) do { len += (size_t)snprintf(s + len, cap - len, __VA_ARGS__); } while (0)
    int en = (int)(k[26] & 7), t2tc1 = (int)(k[26] >> 3 & 1), t0type = (int)(k[26] >> 4 & 3);
    if (en & 1) EMIT(t0type == 1 ? "  t0 = texture(texcube0, v_tc0.xyz);\n" : t0type == 2 ? "  t0 = textureProj(tex0, v_tc0.xyw);\n" : "  t0 = texture(tex0, v_tc0.xy);\n");
    if (en & 2) EMIT("  t1 = texture(tex1, v_tc1);\n");
    if (en & 4) EMIT("  t2 = texture(tex2, %s);\n", t2tc1 ? "v_tc1" : "v_tc2");
    if (fl) EMIT("  fl_compute(t0, t1, t2, fprim, fsec);\n");
    EMIT("  vec4 prev = vec4(0.0), buf = vec4(0.0), next = tev_bufcol;\n");
    int bu_rgb = (int)(k[24] & 0xF), bu_a = (int)(k[24] >> 4 & 0xF);
    for (int i = 0; i < 6; i++) {
        uint32_t src = k[i * 4], op = k[i * 4 + 1], cm = k[i * 4 + 2], sc = k[i * 4 + 3];
        char v[3][32], va[3][32], ar[3][64], aa[3][64], rgb[512], al[512];
        for (int j = 0; j < 3; j++) {
            spec_src(v[j], sizeof v[j], (int)(src >> (4 * j)), i);
            spec_src(va[j], sizeof va[j], (int)(src >> (12 + 4 * j)), i);
            spec_op_rgb(ar[j], sizeof ar[j], (int)(op >> (4 * j)), v[j]);
            spec_op_a(aa[j], sizeof aa[j], (int)(op >> (12 + 4 * j)), va[j]);
        }
        int crgb = (int)(cm & 0xFF), ca = (int)(cm >> 8 & 0xFF), srgb = (int)(sc & 0xFF), sa = (int)(sc >> 8 & 0xFF);
        EMIT("  {\n    vec3 a0 = %s, a1 = %s, a2 = %s;\n    float b0 = %s, b1 = %s, b2 = %s;\n", ar[0], ar[1], ar[2], aa[0], aa[1], aa[2]);
        spec_cmb(rgb, sizeof rgb, crgb, "a0", "a1", "a2", 0);
        spec_cmb(al, sizeof al, ca, "b0", "b1", "b2", 1);
        EMIT("    vec3 rgb = min(%s * %.1f, 1.0);\n", rgb, (double)(1 << (srgb & 31)));
        if (crgb == 7) EMIT("    float al = rgb.r;\n");
        else EMIT("    float al = min(%s * %.1f, 1.0);\n", al, (double)(1 << (sa & 31)));
        EMIT("    prev = vec4(rgb, al);\n    buf = next;\n");
        if (i < 4 && (bu_rgb >> i & 1)) EMIT("    next.rgb = prev.rgb;\n");
        if (i < 4 && (bu_a >> i & 1)) EMIT("    next.a = prev.a;\n");
        EMIT("  }\n");
    }
    if (k[25] & 1) {
        static const char *cmp[8] = {"false", "true", "==", "!=", "<", "<=", ">", ">="};
        int f = (int)(k[25] >> 1 & 7);
        if (f == 0) EMIT("  discard;\n");
        else if (f != 1) EMIT("  if (!(int(prev.a * 255.0 + 0.5) %s alpha_test.z)) discard;\n", cmp[f]);
    }
    if (k[27] & 1) {
        EMIT("  {\n    float z = clamp(%s, 0.0, 1.0) * 128.0;\n    float zi = min(floor(z), 127.0);\n"
             "    vec2 e = texelFetch(fog_lut, ivec2(int(zi), 0), 0).rg;\n"
             "    prev.rgb = mix(fog_color, prev.rgb, clamp(e.x + e.y * (z - zi), 0.0, 1.0));\n  }\n",
             (k[27] & 2) ? "1.0 - gl_FragCoord.z" : "gl_FragCoord.z");
    }
    EMIT("  o_color = prev;\n}\n");
#undef EMIT
    if (len >= cap) { fprintf(stderr, "render: specialised shader too long\n"); abort(); }
    return s;
}

/* the specialised program for base program `base` and the current fragment configuration */
static Program *spec_program(Program *base) {
    if (!spec_enabled() || !base->vs_src) return base;
    uint32_t k[SPEC_KEY_WORDS];
    spec_key(k);
    if (base->fl) spec_key_fl(k);
    uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)base;
    for (int i = 0; i < SPEC_KEY_WORDS; i++) h = (h ^ k[i]) * 16777619u;
    SpecEnt **slot = &g_spec_tab[h % 1024];
    for (SpecEnt *e = *slot; e; e = e->next)
        if (e->base == base && !memcmp(e->key, k, sizeof k)) return &e->prog;
    if (g_spec_count >= 4096) return base;   /* safety cap: generic program */
    SpecEnt *e = calloc(1, sizeof *e);
    if (!e) return base;
    memcpy(e->key, k, sizeof k);
    e->base = base;
    e->prog.ush = &e->ush;
    program_init_fs(&e->prog, base->vs_src, base->fl, k);
    e->next = *slot;
    *slot = e;
    g_spec_count++;
    render_spec_builds++;
    if (g_trace) rlog("built specialised fragment program %u (base %ld)", g_spec_count, (long)(base - R.progs));
    return &e->prog;
}

static void upload_uniforms(Program *p);

void render_set_program(int prog) { render_set_program_variant(prog, 0); }
void render_set_program_variant(int prog, int variant) {
    if (prog < 0 || prog >= RPROG_COUNT) prog = RPROG_CLIP;
    if (prog != R.prog_id || variant != R.prog_variant) {
        R.prog_id = prog;
        R.prog_variant = variant;
        R.tev_dirty = R.vsf_dirty = true;
    }
}

int render_current_program(int *variant) {
    if (variant) *variant = R.prog_variant;
    return R.prog_id;
}

void render_program_alias(uintptr_t key, int prog, int variant) {
    lock();
    size_t i = 0;
    for (; i < R.nalias; i++)
        if (R.alias[i].key == key) break;
    if (i == R.nalias) {
        VEC_PUSH(R.alias, R.nalias, R.capalias);
        R.nalias++;
    }
    R.alias[i] = (ProgAlias){key, prog, variant};
    unlock();
}

static bool alias_get(uintptr_t key, int *prog, int *variant) {
    if (!key) return false;
    for (size_t i = 0; i < R.nalias; i++)
        if (R.alias[i].key == key) { *prog = R.alias[i].prog; *variant = R.alias[i].variant; return true; }
    return false;
}

void render_set_ext(const RenderExt *ext) {
    lock();
    R.ext = *ext;
    unlock();
}

RTexUnit *render_texunit(int unit) { return &R.unit[unit < 0 || unit > 2 ? 0 : unit]; }

static bool dumping(void);
static void dump_read(uint8_t **buf);
static void dump_draw_after(const uint8_t *before, const char *kind, uint32_t count);
static uint8_t *g_ext_before;

int render_draw_bind(void) {
    if (!render_ready()) return 0;
    lock();
    if (!R.cur_fbo) { WARN_ONCE("draw without a render target (dropped)"); unlock(); return 0; }
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    if (R.frag_dirty) apply_frag();
    /* particle draws (nw::eft) run without the stencil test: the effect system's render state
     * (recorded once by eft System setup 003F9B70: culling, blend, alpha, stencil off, depth) is
     * applied on the 3DS from the PICA words of that block, which the game copies by fixed
     * offsets; the port's markers have another layout, the stencil part is not among the copied
     * words, and the particles inherited the shadow pass's test (stencil & 0x3F != 0): no leaves
     * from cut bushes, no sword trails, no effect drawn outside shadows (BOOT_LOG 160y) */
    if (R.prog_id == RPROG_EFT) {
        glDisable(GL_STENCIL_TEST);
        R.frag_dirty = true;   /* the next non-particle draw restores the recorded state */
    }
    {   /* experiment: ALBW_EFT_FRAG=<letters> for particle draws (RPROG_EFT): d depth test off, a alpha
           test off, b blending off, c culling off, s stencil off (which test rejects a particle draw) */
        const char *ef = R.prog_id == RPROG_EFT ? getenv("ALBW_EFT_FRAG") : NULL;   /* not cached: states restore statics */
        if (ef && R.prog_id == RPROG_EFT) {
            if (strchr(ef, 'd')) glDisable(GL_DEPTH_TEST);
            if (strchr(ef, 'b')) glDisable(GL_BLEND);
            if (strchr(ef, 'c')) glDisable(GL_CULL_FACE);
            if (strchr(ef, 's')) glDisable(GL_STENCIL_TEST);
            R.frag_dirty = true;
        }
    }
    Program *p = spec_program(cur_program());
    use_program(p->prog);
    upload_uniforms(p);
    for (int i = 0; i < 3; i++) bind_unit(i);
    if (dumping()) { g_ext_before = NULL; dump_read(&g_ext_before); }
    return 1; /* keeps the lock until render_draw_done */
}

void render_draw_done(void) {
    if (dumping()) {
        dump_draw_after(g_ext_before, "extdraw", 0);
        if (R.prog_id == RPROG_EFT) {   /* particle uniforms: colour c82, texture repeat c83, block 0 scale / tex params */
            const float *c = R.vsf;
            rlog("dump:     eft c82 %g %g %g %g c83 %g %g %g %g | P2 %g %g %g %g P3 %g %g %g %g P4 %g %g %g %g",
                 c[82*4], c[82*4+1], c[82*4+2], c[82*4+3], c[83*4], c[83*4+1], c[83*4+2], c[83*4+3],
                 c[2*4], c[2*4+1], c[2*4+2], c[2*4+3], c[3*4], c[3*4+1], c[3*4+2], c[3*4+3], c[4*4], c[4*4+1], c[4*4+2], c[4*4+3]);
        }
    }
    glBindVertexArray(0);
    R.st.draws++;
    unlock();
}
void render_set_vs_float(int reg, const float *v, int count) {
    if (reg < 0 || reg >= RENDER_VS_FLOATS) return;
    if (reg + count > RENDER_VS_FLOATS) count = RENDER_VS_FLOATS - reg;
    memcpy(&R.vsf[reg * 4], v, (size_t)count * 4 * sizeof(float));
    R.vsf_dirty = true;
}
void render_set_vs_bool(uint16_t mask) { R.vsb = mask; R.vsf_dirty = true; }
/* debug readback of the vertex-shader float / bool registers (render/h3d.c ALBW_H3D_DUMP) */
const RTev *render_tev_state(void) { return &R.tev; }
const float *render_vs_floats(uint16_t *bools) {
    if (bools) *bools = R.vsb;
    return R.vsf;
}
void render_set_attrib(int reg, const void *ptr, int type, int count, int stride) {
    if (reg < 0 || reg >= RENDER_MAX_ATTRIBS) return;
    R.attr[reg].ptr = (uintptr_t)ptr;
    R.attr[reg].type = type & 3;
    R.attr[reg].count = count < 1 ? 1 : count > 4 ? 4 : count;
    R.attr[reg].stride = stride;
    R.attr[reg].array = ptr != NULL;
}
void render_set_attrib_const(int reg, const float v[4]) {
    if (reg < 0 || reg >= RENDER_MAX_ATTRIBS) return;
    R.attr[reg].array = false;
    memcpy(R.attr[reg].k, v, sizeof R.attr[reg].k);
}
void render_disable_attribs(void) {
    for (int i = 0; i < RENDER_MAX_ATTRIBS; i++) R.attr[i].array = false;
}

/* fog LUTs registered by the producers (sdk/h3d.c decoder), by content key */
typedef struct { uint32_t key; float lut[128][2]; } FogLut;
static FogLut *g_fog_luts; static size_t g_nfog, g_capfog;
static pthread_mutex_t g_fog_mx = PTHREAD_MUTEX_INITIALIZER;

void render_fog_lut_set(uint32_t key, const float lut[128][2]) {
    if (!key) return;
    pthread_mutex_lock(&g_fog_mx);
    size_t i = 0;
    while (i < g_nfog && g_fog_luts[i].key != key) i++;
    if (i == g_nfog) {
        VEC_PUSH(g_fog_luts, g_nfog, g_capfog);
        g_fog_luts[g_nfog].key = key;
        memcpy(g_fog_luts[g_nfog].lut, lut, sizeof g_fog_luts[0].lut);
        g_nfog++;
    }
    pthread_mutex_unlock(&g_fog_mx);
}

/* the current fog LUT on FOG_LUT_UNIT; 0 when there is none */
static int fog_bind(void) {
    if (!R.fog.key) return 0;
    if (!R.fog.tex) {
        glGenTextures(1, &R.fog.tex);
        glActiveTexture(GL_TEXTURE0 + FOG_LUT_UNIT);
        glBindTexture(GL_TEXTURE_2D, R.fog.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glActiveTexture(GL_TEXTURE0 + FOG_LUT_UNIT);
    glBindTexture(GL_TEXTURE_2D, R.fog.tex);
    if (R.fog.tex_key != R.fog.key) {
        int found = 0;
        pthread_mutex_lock(&g_fog_mx);
        for (size_t i = 0; i < g_nfog; i++)
            if (g_fog_luts[i].key == R.fog.key) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, 128, 1, 0, GL_RG, GL_FLOAT, g_fog_luts[i].lut);
                found = 1;
                break;
            }
        pthread_mutex_unlock(&g_fog_mx);
        if (!found) return 0;
        R.fog.tex_key = R.fog.key;
    }
    return 1;
}

#define SH_CHANGED(field, src) (!S->sh_valid || memcmp(S->field, (src), sizeof S->field))
static void upload_uniforms(Program *p) {
    UniShadow *S = ush_of(p);
    static int no_shadow = -1;   /* debug ALBW_NO_USHADOW=1: send every uniform every draw (read once) */
    if (no_shadow < 0) no_shadow = getenv("ALBW_NO_USHADOW") != NULL;
    if (no_shadow) S->sh_valid = 0;
    if (p->ws >= 0 && (!S->sh_valid || S->sh_ws != g_cur_fy)) { glUniform1f(p->ws, g_cur_fy); S->sh_ws = g_cur_fy; }
    if (SH_CHANGED(sh_c, R.vsf)) { glUniform4fv(p->c, RENDER_VS_FLOATS, R.vsf); memcpy(S->sh_c, R.vsf, sizeof S->sh_c); }
    if (!S->sh_valid || S->sh_b != (GLint)R.vsb) { glUniform1i(p->b, R.vsb); S->sh_b = R.vsb; }
    {
        GLint src[24], cmb[24];
        float k[24];
        for (int i = 0; i < 6; i++) {
            const RTevStage *s = &R.tev.stage[i];
            src[i * 4 + 0] = s->src_rgb[0] | s->src_rgb[1] << 4 | s->src_rgb[2] << 8;
            src[i * 4 + 1] = s->src_a[0] | s->src_a[1] << 4 | s->src_a[2] << 8;
            src[i * 4 + 2] = s->op_rgb[0] | s->op_rgb[1] << 4 | s->op_rgb[2] << 8;
            src[i * 4 + 3] = s->op_a[0] | s->op_a[1] << 4 | s->op_a[2] << 8;
            cmb[i * 4 + 0] = s->combine_rgb;
            cmb[i * 4 + 1] = s->combine_a;
            cmb[i * 4 + 2] = s->scale_rgb;
            cmb[i * 4 + 3] = s->scale_a;
            for (int c = 0; c < 4; c++) k[i * 4 + c] = s->konst[c] / 255.f;
        }
        if (SH_CHANGED(sh_src, src)) { rglUniform4iv(p->tev_src, 6, src); memcpy(S->sh_src, src, sizeof src); }
        if (SH_CHANGED(sh_cmb, cmb)) { rglUniform4iv(p->tev_cmb, 6, cmb); memcpy(S->sh_cmb, cmb, sizeof cmb); }
        if (SH_CHANGED(sh_k, k)) { glUniform4fv(p->tev_const, 6, k); memcpy(S->sh_k, k, sizeof k); }
        GLint bu[2] = {R.tev.buf_update_rgb, R.tev.buf_update_a};
        if (SH_CHANGED(sh_buf, bu)) { rglUniform2i(p->tev_buf, bu[0], bu[1]); memcpy(S->sh_buf, bu, sizeof bu); }
        float bc[4] = {R.tev.buf_color[0] / 255.f, R.tev.buf_color[1] / 255.f, R.tev.buf_color[2] / 255.f, R.tev.buf_color[3] / 255.f};
        if (SH_CHANGED(sh_bufcol, bc)) { rglUniform4f(p->tev_bufcol, bc[0], bc[1], bc[2], bc[3]); memcpy(S->sh_bufcol, bc, sizeof bc); }
    }
    GLint at[4] = {R.frag.alpha_test, R.frag.alpha_func, R.frag.alpha_ref, 0};
    {   static const char *ef = (const char *)1;   /* ALBW_EFT_FRAG 'a' (see render_draw_bind) */
        if (ef == (const char *)1) ef = getenv("ALBW_EFT_FRAG");
        if (ef && strchr(ef, 'a') && R.prog_id == RPROG_EFT) at[0] = 0;
    }
    {   /* experiment: ALBW_ATEST_TEX=<hex addr>: enable the configured alpha test for draws sampling that texture */
        static long want = -2;
        if (want == -2) { const char *e = getenv("ALBW_ATEST_TEX"); want = e ? (long)strtoul(e, NULL, 16) : -1; }
        if (want >= 0 && (R.tex_enable & 1) && R.unit[0].addr == (uintptr_t)want) {
            at[0] = 1;
            const char *fe = getenv("ALBW_ATEST_FUNC");   /* optional PICA function override (4 = less) */
            if (fe) at[1] = atoi(fe);
        }
    }
    if (SH_CHANGED(sh_at, at)) { rglUniform4iv(p->alpha, 1, at); memcpy(S->sh_at, at, sizeof at); }
    if (p->fog >= 0) {
        int on = R.fog.mode == 5 && !(&g_dbg_fog_off && g_dbg_fog_off) && fog_bind();   /* Esc menu Debug > Fog */
        GLint fg[2] = {on ? 5 : 0, R.fog.flip};
        if (SH_CHANGED(sh_fog, fg)) { rglUniform2i(p->fog, fg[0], fg[1]); memcpy(S->sh_fog, fg, sizeof fg); }
        if (on && SH_CHANGED(sh_fogc, R.fog.rgb)) { rglUniform3f(p->fog_color, R.fog.rgb[0], R.fog.rgb[1], R.fog.rgb[2]); memcpy(S->sh_fogc, R.fog.rgb, sizeof S->sh_fogc); }
    }
    GLint tc[4] = {R.tex_enable, R.tex2_tc1, (R.unit[0].type == 1 || R.unit[0].type == 4) ? 1 : R.unit[0].type == 3 ? 2 : 0, 0};
    if (SH_CHANGED(sh_tc, tc)) { rglUniform4iv(p->texcfg, 1, tc); memcpy(S->sh_tc, tc, sizeof tc); }
    if (p->fl) fraglight_upload(&p->fll, FL_LUT_UNIT);
    if (g_light_trace) { /* debug: per-frame digest of the lighting / combiner constants all draws saw */
        uint64_t h = p->fl ? fraglight_state_hash() : 0;
        for (int i = 0; i < 6; i++) h = (h ^ R.tev.stage[i].konst[0] ^ (uint64_t)R.tev.stage[i].konst[1] << 8 ^
                                         (uint64_t)R.tev.stage[i].konst[2] << 16 ^ (uint64_t)R.tev.stage[i].konst[3] << 24) * 1099511628211ull;
        g_light_digest = (g_light_digest ^ h) * 1099511628211ull;
        g_light_draws++;
    }
    S->sh_valid = 1;
    R.vsf_dirty = R.tev_dirty = false;
}

/* append to a streaming buffer, orphaning it when full; returns the offset.
 * Written through glMapBufferRange(UNSYNCHRONIZED): each draw's data goes to a range no queued draw
 * uses (the ring only wraps after an invalidate of the whole buffer), but a driver cannot know that
 * for glBufferSubData, and Adreno waited for the GPU on every call: ~48% of the GL thread's time and
 * 17 fps on the title screen (README: Android performance). Adreno waits for UNSYNCHRONIZED maps too,
 * so Android uses the buffer pool below (60 fps). */
#define GL_MAP_WRITE_BIT_ 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT_ 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT_ 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT_ 0x0020
/* ALBW_STREAM_MODE: 2 (Android default) a pool of buffer objects, each draw's data re-specified with
 * glBufferData into the next one (the driver gives a busy buffer new storage instead of waiting:
 * Adreno waited even for UNSYNCHRONIZED maps of a buffer queued draws use); 1 the ring through
 * glMapBufferRange(UNSYNCHRONIZED); 0 (PC default) the ring through glBufferSubData. Pool mode returns offset 0
 * and leaves the buffer it used bound to `target`. */
#define STREAM_POOL 512
static int stream_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("ALBW_STREAM_MODE");
#if defined(__ANDROID__)
        m = e ? atoi(e) : 2;
#else
        m = e ? atoi(e) : 0;   /* the PC drivers (Mesa) handle the ring; pool mode untested there */
#endif
        if (getenv("ALBW_STREAM_SUBDATA")) m = 0;
    }
    return m;
}
static size_t stream(GLenum target, GLuint buf, size_t *off, size_t cap, const void *data, size_t n) {
    static void *(*map)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
    static GLboolean (*unmap)(GLenum);
    static int mode = -1;   /* 2 pool, 1 map, 0 subdata */
    if (mode < 0) {
        map = (void *(*)(GLenum, GLintptr, GLsizeiptr, GLbitfield))SDL_GL_GetProcAddress("glMapBufferRange");
        unmap = (GLboolean (*)(GLenum))SDL_GL_GetProcAddress("glUnmapBuffer");
        mode = stream_mode();
        if (mode == 1 && !(map && unmap)) mode = 0;
    }
    if (mode == 2) {
        static GLuint pool[2][STREAM_POOL];
        static unsigned next[2];
        int k = target == GL_ARRAY_BUFFER ? 0 : 1;
        if (!pool[k][0]) glGenBuffers(STREAM_POOL, pool[k]);
        GLuint b = pool[k][next[k]++ % STREAM_POOL];
        glBindBuffer(target, b);
        glBufferData(target, (GLsizeiptr)(n ? n : 16), data, GL_STREAM_DRAW);
        (void)buf; (void)off; (void)cap;
        return 0;
    }
    glBindBuffer(target, buf);
    size_t o = (*off + 15) & ~(size_t)15;
    bool wrap = o + n > cap;
    if (wrap) o = 0;
    if (mode == 1 && n) {
        GLbitfield fl = GL_MAP_WRITE_BIT_ | (wrap ? GL_MAP_INVALIDATE_BUFFER_BIT_ : GL_MAP_UNSYNCHRONIZED_BIT_ | GL_MAP_INVALIDATE_RANGE_BIT_);
        void *m = map(target, (GLintptr)o, (GLsizeiptr)n, fl);
        if (m) {
            memcpy(m, data, n);
            unmap(target);
            *off = o + n;
            return o;
        }
        WARN_ONCE("glMapBufferRange failed: streaming with glBufferSubData");
        mode = 0;
    }
    if (wrap) glBufferData(target, (GLsizeiptr)cap, NULL, GL_STREAM_DRAW);
    glBufferSubData(target, (GLintptr)o, (GLsizeiptr)n, data);
    *off = o + n;
    return o;
}

static const int ATTR_SIZE[4] = {1, 1, 2, 4};
static const GLenum ATTR_GL[4] = {GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_FLOAT};
static const GLenum PRIM_GL[3] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};

static bool draw_setup(uint32_t vmin, uint32_t vmax) {
    g_render_draws++;
    if (!R.cur_fbo) { WARN_ONCE("draw without a render target (dropped)"); return false; }
    /* the platform layer / scan-out rebind other framebuffers and state between frames */
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    if (R.frag_dirty) apply_frag();
    Program *p = spec_program(cur_program());
    use_program(p->prog);
    upload_uniforms(p);   /* per program: only what changed since its last draw */
    for (int i = 0; i < 3; i++) bind_unit(i);
    glBindVertexArray(R.vao);
    uint32_t nv = vmax - vmin + 1;
    for (int i = 0; i < RENDER_MAX_ATTRIBS; i++) {
        if (!R.attr[i].array) {
            rglDisableVertexAttribArray((GLuint)i);
            rglVertexAttrib4f((GLuint)i, R.attr[i].k[0], R.attr[i].k[1], R.attr[i].k[2], R.attr[i].k[3]);
            continue;
        }
        int esz = ATTR_SIZE[R.attr[i].type] * R.attr[i].count;
        int stride = R.attr[i].stride ? R.attr[i].stride : esz;
        size_t n = (size_t)(nv - 1) * (size_t)stride + (size_t)esz;
        if (n > STREAM_BYTES / 2) { WARN_ONCE("vertex range too large (%zu bytes)", n); return false; }
        size_t o = stream(GL_ARRAY_BUFFER, R.vbo, &R.vbo_off, STREAM_BYTES,
                          (const uint8_t *)R.attr[i].ptr + (size_t)vmin * (size_t)stride, n);
        glVertexAttribPointer((GLuint)i, R.attr[i].count, ATTR_GL[R.attr[i].type], GL_FALSE, stride,
                              (const void *)(uintptr_t)o);
        glEnableVertexAttribArray((GLuint)i);
    }
    return true;
}

/* ALBW_RENDER_DUMP=<frame>: log every executed op of that host frame, and for each draw the
 * pipeline state and how many pixels of the target changed (glReadPixels before/after). Debug only. */
static bool dumping(void) { return g_dump_frame >= 0 && (long)R.frame >= g_dump_frame && (long)R.frame <= g_dump_last; }
static uint8_t *g_dump_px; static size_t g_dump_cap;
static void dump_read(uint8_t **buf) {
    if (!R.cur_c) { *buf = NULL; return; }
    size_t n = (size_t)R.cur_c->w * R.scale * sph(R.cur_c) * R.scale * 4;
    if (g_dump_cap < 2 * n) { g_dump_px = xrealloc(g_dump_px, 2 * n); g_dump_cap = 2 * n; }
    if (!*buf) *buf = g_dump_px; else *buf = g_dump_px + n;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.cur_c->fbo);
    rglReadPixels(0, 0, R.cur_c->w * R.scale, sph(R.cur_c) * R.scale, GL_RGBA, GL_UNSIGNED_BYTE, *buf);
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
}
static void dump_probe(const uint8_t *before) {
    if (!before) return;
    size_t n = (size_t)R.cur_c->w * R.scale * sph(R.cur_c) * R.scale * 4;
    uint8_t *after = g_dump_px + n;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.cur_c->fbo);
    rglReadPixels(0, 0, R.cur_c->w * R.scale, sph(R.cur_c) * R.scale, GL_RGBA, GL_UNSIGNED_BYTE, after);
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    size_t changed = 0, first = (size_t)-1;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1, W = R.cur_c->w * R.scale;
    for (size_t i = 0; i < n; i += 4)
        if (memcmp(before + i, after + i, 4)) {
            changed++;
            if (first == (size_t)-1) first = i;
            int x = (int)(i / 4 % (size_t)W), y = (int)(i / 4 / (size_t)W);
            if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
    const RTevStage *t = &R.tev.stage[0];
    if (changed)
        rlog("dump:   probe (no blend/depth): changed %zu box %d,%d-%d,%d first rgba %u %u %u %u; tex0 %#lx %ux%u fmt %u; "
             "tev0 src %u/%u/%u cmb %u a-src %u/%u/%u cmb %u k %u %u %u %u", changed, minx, miny, maxx, maxy,
             after[first], after[first + 1], after[first + 2], after[first + 3], (unsigned long)R.unit[0].addr,
             R.unit[0].w, R.unit[0].h, R.unit[0].fmt, t->src_rgb[0], t->src_rgb[1], t->src_rgb[2], t->combine_rgb,
             t->src_a[0], t->src_a[1], t->src_a[2], t->combine_a, t->konst[0], t->konst[1], t->konst[2], t->konst[3]);
    else {
        rlog("dump:   probe (no blend/depth): nothing rasterised");
        for (int r = 0; r < 44; r++)
            if (r < 12 || r >= 32) rlog("dump:     c%-2d %g %g %g %g", r, R.vsf[r * 4], R.vsf[r * 4 + 1], R.vsf[r * 4 + 2], R.vsf[r * 4 + 3]);
        for (int i = 0; i < 4; i++)
            if (R.attr[i].array) {
                const uint8_t *v = (const uint8_t *)R.attr[i].ptr;
                if (R.attr[i].type == RATTR_FLOAT)
                    rlog("dump:     v%d ptr %#lx f32x%d stride %d: %g %g %g", i, (unsigned long)R.attr[i].ptr, R.attr[i].count,
                         R.attr[i].stride, ((const float *)v)[0], ((const float *)v)[1], R.attr[i].count > 2 ? ((const float *)v)[2] : 0.f);
                else
                    rlog("dump:     v%d ptr %#lx type %d x%d stride %d: %02x %02x %02x %02x", i, (unsigned long)R.attr[i].ptr,
                         R.attr[i].type, R.attr[i].count, R.attr[i].stride, v[0], v[1], v[2], v[3]);
            } else
                rlog("dump:     v%d const %g %g %g %g", i, R.attr[i].k[0], R.attr[i].k[1], R.attr[i].k[2], R.attr[i].k[3]);
    }
}

static void dump_draw_after(const uint8_t *before, const char *kind, uint32_t count) {
    if (!before) { rlog("dump: %s %u: no colour target", kind, count); return; }
    size_t n = (size_t)R.cur_c->w * R.scale * sph(R.cur_c) * R.scale * 4;
    uint8_t *after = g_dump_px + n;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.cur_c->fbo);
    rglReadPixels(0, 0, R.cur_c->w * R.scale, sph(R.cur_c) * R.scale, GL_RGBA, GL_UNSIGNED_BYTE, after);
    glBindFramebuffer(GL_FRAMEBUFFER, R.cur_fbo);
    size_t changed = 0, lit = 0;
    int W = R.cur_c->w * R.scale, bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    for (size_t i = 0; i < n; i += 4) {
        if (memcmp(before + i, after + i, 4)) {
            changed++;
            int x = (int)(i / 4 % (size_t)W), y = (int)(i / 4 / (size_t)W);
            if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y;
        }
        if (after[i] | after[i + 1] | after[i + 2]) lit++;
    }
    /* surface coordinates (GL rows of the 240-wide target) -> LCD pixels: lcd_x = H-1-y, lcd_y = W-1-x */
    if (changed)
        rlog("dump:     lcd box x %d-%d y %d-%d", sph(R.cur_c) * R.scale - 1 - by1, sph(R.cur_c) * R.scale - 1 - by0,
             W - 1 - bx1, W - 1 - bx0);
    for (int u = 0; u < 3; u++)
        if (R.tex_enable >> u & 1)
            rlog("dump:     unit%d tex %#lx %ux%u fmt %u min %u mag %u lvl %u-%u", u, (unsigned long)R.unit[u].addr, R.unit[u].w,
                 R.unit[u].h, R.unit[u].fmt, R.unit[u].min, R.unit[u].mag, R.unit[u].min_level, R.unit[u].max_level);
    const RFragState *f = &R.frag;
    rlog("dump: %s %u prog %d/%d target %#lx vp %d,%d %dx%d sc %d(%d,%d %dx%d) cmask %x blend %d(%d,%d) depth %d/%d/%d "
         "stencil %d alpha %d/%d/%d cull %d tex %x eq %d/%d logic %d/%d -> changed %zu lit %zu",
         kind, count, R.prog_id, R.prog_variant, (unsigned long)R.cur_c->addr, R.vp[0], R.vp[1], R.vp[2], R.vp[3],
         R.scissor, R.sc[0], R.sc[1], R.sc[2], R.sc[3], f->color_mask, f->blend_enable, f->src_rgb, f->dst_rgb, f->depth_test, f->depth_func,
         f->depth_write, f->stencil_test, f->alpha_test, f->alpha_func, f->alpha_ref, f->cull, R.tex_enable,
         f->eq_rgb, f->eq_a, f->logic_enable, f->logic_op,
         changed, lit);
    for (int i = 0; i < 6; i++) {
        const RTevStage *t = &R.tev.stage[i];
        rlog("dump:     tev%d rgb src %u/%u/%u op %u/%u/%u cmb %u x%u | a src %u/%u/%u op %u/%u/%u cmb %u x%u | k %u %u %u %u",
             i, t->src_rgb[0], t->src_rgb[1], t->src_rgb[2], t->op_rgb[0], t->op_rgb[1], t->op_rgb[2], t->combine_rgb,
             t->scale_rgb, t->src_a[0], t->src_a[1], t->src_a[2], t->op_a[0], t->op_a[1], t->op_a[2], t->combine_a,
             t->scale_a, t->konst[0], t->konst[1], t->konst[2], t->konst[3]);
    }
    for (int r = 0; r < 72; r++)  /* projection, stereo c4, record c9, pools c32.. / c64.. */
        if (r < 32 || (r >= 32 && r < 44) || r >= 64)
            rlog("dump:     c%d %g %g %g %g", r, R.vsf[r * 4], R.vsf[r * 4 + 1], R.vsf[r * 4 + 2], R.vsf[r * 4 + 3]);
    rlog("dump:     bools %#06x tevbuf rgb %#x a %#x col %u %u %u %u", R.vsb, R.tev.buf_update_rgb, R.tev.buf_update_a,
         R.tev.buf_color[0], R.tev.buf_color[1], R.tev.buf_color[2], R.tev.buf_color[3]);
    rlog("dump:     depthofs %u units %g range %g..%g wbuf %u depthfmt %d", f->poly_offset_enable, (double)f->poly_offset_units,
         (double)f->depth_near, (double)f->depth_far, f->wbuffer, R.cur_d ? R.cur_d->fmt : -1);
    rlog("dump:     stencil test %u func %u ref %#x mask %#x write %#x ops fail %u zfail %u zpass %u | depth surface %#lx",
         f->stencil_test, f->stencil_func, f->stencil_ref, f->stencil_mask, f->stencil_write, f->stencil_fail, f->stencil_zfail,
         f->stencil_zpass, R.cur_d ? (unsigned long)R.cur_d->addr : 0ul);
    fraglight_debug_print(stderr); /* lights, ambient, bound LUTs of this draw */
}

void render_draw(int prim, uint32_t first, uint32_t count) {
    if (!count || !render_ready()) return;
    lock();
    if (draw_setup(first, first + count - 1)) {
        uint8_t *before = NULL;
        bool dmp = dumping();
        if (dmp) { dump_read(&before); glBindVertexArray(R.vao); }
        glDrawArrays(PRIM_GL[prim % 3], 0, (GLsizei)count);
        R.st.draws++;
        if (dmp) {
            dump_draw_after(before, "draw", count);
            /* probe: the same draw without blending / depth / alpha test, what does it output? */
            glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
            { static const float mag[4] = {1, 0, 1, 0.5f}; rglColorMask(1, 1, 1, 1); rglClearBufferfv(GL_COLOR, 0, mag); }
            glBindVertexArray(R.vao);
            before = NULL;
            dump_read(&before);
            glDrawArrays(PRIM_GL[prim % 3], 0, (GLsizei)count);
            dump_probe(before);
            R.frag_dirty = true;
        }
    }
    glBindVertexArray(0);
    unlock();
}

void render_draw_indexed(int prim, const void *indices, int index_u16, uint32_t count) {
    if (!count || !indices || !render_ready()) return;
    lock();
    uint32_t mn = 0xFFFFFFFFu, mx = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = index_u16 ? ((const uint16_t *)indices)[i] : ((const uint8_t *)indices)[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    /* eft "primitive" emitters (Fire / Tornado Rod flames: cone and ring meshes, e.g. 240 indices over
     * 63 vertices) bind a program family not written yet (effects.c: v0 is a 3D position there, not a
     * quad corner). Drawn with the quad program they became screen-wide shards: skip them until that
     * program exists. A non-instanced quad program draws one particle = 6 indices. */
    if (R.prog_id == RPROG_EFT && !(R.vsb & 16) && count > 6) {
        static int keep = -1, tr = -1;   /* debug: ALBW_EFT_PRIM=1 draws them anyway, ALBW_EFT_PRIM_TRACE=1 logs skips */
        if (keep < 0) { keep = getenv("ALBW_EFT_PRIM") != NULL; tr = getenv("ALBW_EFT_PRIM_TRACE") != NULL; }
        if (tr) { extern long g_plat_frames __attribute__((weak));
                  fprintf(stderr, "eft-prim: f=%ld skipped %u indices (vertices %u..%u) b %#x\n", &g_plat_frames ? g_plat_frames : 0L, (unsigned)count, (unsigned)mn, (unsigned)mx, (unsigned)R.vsb); }
        if (!keep) { unlock(); return; }
    }
    if (draw_setup(mn, mx)) {
        size_t isz = (size_t)count * (index_u16 ? 2 : 1);
        size_t o = stream(GL_ELEMENT_ARRAY_BUFFER, R.ibo, &R.ibo_off, STREAM_BYTES / 4, indices, isz);
        uint8_t *before = NULL;
        bool dmp = dumping();
        if (dmp) { dump_read(&before); glBindVertexArray(R.vao); }
        rglDrawElementsBaseVertex(PRIM_GL[prim % 3], (GLsizei)count, index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                                  (const void *)(uintptr_t)o, -(GLint)mn);
        R.st.draws++;
        if (dmp) {
            dump_draw_after(before, "drawidx", count);
            if (R.prog_id == RPROG_EFT) {   /* the particle blocks this draw used (c1.., + 10 n) */
                const float *c = R.vsf;
                rlog("dump:     eft bools %#x idx %u..%u c84 %g %g %g %g", R.vsb, mn, mx, c[84*4], c[84*4+1], c[84*4+2], c[84*4+3]);
                for (int blk = 0; blk < 16 && 1 + 10 * blk + 9 < 82; blk += (blk == 1 ? 14 : 1)) {
                    int b0 = 1 + 10 * blk;
                    rlog("dump:     eft blk %d col %g %g %g %g | scl %g %g %g %g | pos %g %g %g | vel %g %g %g | rot %g %g %g",
                         blk, c[b0*4], c[b0*4+1], c[b0*4+2], c[b0*4+3], c[(b0+1)*4], c[(b0+1)*4+1], c[(b0+1)*4+2], c[(b0+1)*4+3],
                         c[(b0+4)*4], c[(b0+4)*4+1], c[(b0+4)*4+2], c[(b0+5)*4], c[(b0+5)*4+1], c[(b0+5)*4+2],
                         c[(b0+6)*4], c[(b0+6)*4+1], c[(b0+6)*4+2]);
                }
            }
            glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
            { static const float mag[4] = {1, 0, 1, 0.5f}; rglColorMask(1, 1, 1, 1); rglClearBufferfv(GL_COLOR, 0, mag); }
            glBindVertexArray(R.vao);
            before = NULL;
            dump_read(&before);
            rglDrawElementsBaseVertex(PRIM_GL[prim % 3], (GLsizei)count, index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                                      (const void *)(uintptr_t)o, -(GLint)mn);
            dump_probe(before);
            R.frag_dirty = true;
        }
    }
    glBindVertexArray(0);
    unlock();
}

/* ================================================================== op pool + markers */

#define POOL_MAX ((1u << 20) - 2)
#define POOL_GC_HIGH 200000u
#define POOL_GC_AGE 600u

typedef struct {
    ROp op;
    uint64_t hash;
    uint32_t last_frame;
    uint8_t gen, live, pinned;
} PoolEnt;

static PoolEnt *g_pool;
static uint32_t g_npool, g_cappool, g_live;
static uint32_t *g_free; static uint32_t g_nfree, g_capfree;
static uint32_t *g_htab; static uint32_t g_hcap; /* open addressing: slot+1, 0 empty */

/* Bytes of an op that matter: the header and the union member its type uses (every recorder
 * zeroes the whole op first, so the rest is always zero). Hashing / comparing only these, instead
 * of all 264 bytes, is the same equality for half the cost of the op pool (profile: rec +
 * hash_bytes were the top two native functions of the title sequence). */
#define OP_LEN(m) (offsetof(ROp, u) + sizeof(((ROp *)0)->u.m))
static size_t op_len(const ROp *op) {
    switch (op->type) {
    case ROP_TARGET: return OP_LEN(target);
    case ROP_VIEWPORT: case ROP_SCISSOR: return OP_LEN(rect);
    case ROP_FRAG: case ROP_BLEND: case ROP_LOGICOP: case ROP_ALPHATEST: case ROP_STENCIL:
    case ROP_DEPTH: case ROP_COLORMASK: case ROP_CULL: case ROP_DEPTHMAP: return OP_LEN(frag);
    case ROP_TEXUNIT: return OP_LEN(tex);
    case ROP_TEXCONFIG: return OP_LEN(texcfg);
    case ROP_TEV: case ROP_TEV_STAGES: return OP_LEN(tev);
    case ROP_TEV_CONST: return OP_LEN(tev_const);
    case ROP_PROGRAM: return OP_LEN(program);
    case ROP_VS_FLOAT: {   /* registers past the count (op->unit, at most 16) are zero */
        unsigned n = op->unit > 16 ? 16 : op->unit;
        return offsetof(ROp, u.vsf.v) + n * 4 * sizeof(float);
    }
    case ROP_VS_BOOL: return OP_LEN(vsb);
    case ROP_ATTRIB: return OP_LEN(attrib);
    case ROP_DRAW: return OP_LEN(draw);
    case ROP_CALL: return OP_LEN(call);
    case ROP_EXT: return OP_LEN(ext);
    case ROP_FOG: return OP_LEN(fog);
    default: return sizeof *op;   /* ROP_FL and anything new: the whole op */
    }
}
/* hash_bytes multiplies (FNV style), so its low bits depend only on the inputs' low bits, and the
 * pool table is indexed by the low bits: ops differing in their upper bits (addresses, floats)
 * piled into a few buckets. During gameplay on the RP5 the linear probing in pool_put took 84% of
 * the game thread (README: Android performance). fmix64 (MurmurHash3) spreads every bit. */
static uint64_t fmix64(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
}
static uint64_t op_hash(const ROp *op) { return fmix64(hash_bytes(op, op_len(op))) | 1; }

/* marker token: slot + 1 (never 0) | generation << 20 | 4-bit check << 28 */
static uint32_t token_of(uint32_t slot, uint8_t gen) {
    uint32_t chk = (slot * 2654435761u >> 28 ^ gen) & 15;
    return (slot + 1) | (uint32_t)gen << 20 | chk << 28;
}

static void htab_rebuild(uint32_t cap) {
    free(g_htab);
    g_hcap = cap;
    g_htab = calloc(cap, sizeof *g_htab);
    if (!g_htab) abort();
    for (uint32_t s = 0; s < g_npool; s++) {
        if (!g_pool[s].live) continue;
        uint32_t h = (uint32_t)g_pool[s].hash & (cap - 1);
        while (g_htab[h]) h = (h + 1) & (cap - 1);
        g_htab[h] = s + 1;
    }
}

static uint32_t pool_put(const ROp *op, int pinned) {
    uint64_t h = op_hash(op);
    if (g_hcap && g_htab) {
        for (uint32_t i = (uint32_t)h & (g_hcap - 1);; i = (i + 1) & (g_hcap - 1)) {
            uint32_t e = g_htab[i];
            if (!e) break;
            PoolEnt *p = &g_pool[e - 1];
            if (p->live && p->hash == h && !memcmp(&p->op, op, op_len(op))) {
                p->last_frame = R.frame;
                if (pinned) p->pinned = 1;
                return token_of(e - 1, p->gen);
            }
        }
    }
    uint32_t slot;
    if (g_nfree) {
        slot = g_free[--g_nfree];
    } else {
        if (g_npool >= POOL_MAX) {
            WARN_ONCE("op pool full (%u ops): op dropped", POOL_MAX);
            return 0;
        }
        if (g_npool == g_cappool) {
            g_cappool = g_cappool ? g_cappool * 2 : 4096;
            g_pool = xrealloc(g_pool, g_cappool * sizeof *g_pool);
        }
        slot = g_npool++;
        g_pool[slot].gen = 0;
    }
    PoolEnt *p = &g_pool[slot];
    p->op = *op;
    p->hash = h;
    p->last_frame = R.frame;
    p->live = 1;
    p->pinned = (uint8_t)(pinned != 0);
    g_live++;
    if (!g_htab || g_live * 2 > g_hcap) {
        htab_rebuild(g_hcap ? g_hcap * 2 : 8192);
    } else {
        uint32_t i = (uint32_t)h & (g_hcap - 1);
        while (g_htab[i]) i = (i + 1) & (g_hcap - 1);
        g_htab[i] = slot + 1;
    }
    R.st.ops_pooled = g_live;
    return token_of(slot, p->gen);
}

static const ROp *pool_get(uint32_t token) {
    uint32_t slot = (token & 0xFFFFF) - 1;
    uint8_t gen = (uint8_t)(token >> 20);
    if ((token & 0xFFFFF) == 0 || slot >= g_npool || token_of(slot, gen) != token) return NULL;
    PoolEnt *p = &g_pool[slot];
    if (!p->live || p->gen != gen) return NULL;
    p->last_frame = R.frame;
    return &p->op;
}

static uint32_t *rec(uint32_t *p, const ROp *op, int pinned) {
    lock();
    uint32_t tok = pool_put(op, pinned);
    R.st.ops_recorded++;
    unlock();
    if (!p) return NULL;
    p[0] = tok;
    p[1] = tok ? RENDER_MARK : 0;
    return p + RENDER_MARK_WORDS;
}

uint32_t *render_rec(uint32_t *p, const ROp *op) { return rec(p, op, 0); }
uint32_t *render_rec_pinned(uint32_t *p, const ROp *op) { return rec(p, op, 1); }

static void raw_flush(void);
void albw_ws_refresh(void) __attribute__((weak));        /* compilefix 0057F6C0 */
void plat_frame_end_hook(void) __attribute__((weak));    /* platform.c: deferred widescreen toggle */
void render_frame_end(void) {
    g_glprog_bound = 0;   /* scan-out and the platform's composite bound their own programs */
    /* between frames (the frame was presented, the next one not begun): settings that change the
     * top screen's shape, then projections built for the other shape are marked for a rebuild */
    if (plat_frame_end_hook) plat_frame_end_hook();
    if (albw_ws_refresh) albw_ws_refresh();
    lock();
    raw_flush();
    if (g_light_trace) {
        rlog("light-trace frame %lu digest %016llx draws %u", (unsigned long)R.frame, (unsigned long long)g_light_digest, g_light_draws);
        g_light_digest = 0;
        g_light_draws = 0;
    }
    if (dumping()) rlog("dump: ==== end of render frame %lu", (unsigned long)R.frame);
    R.frame++;
    R.frag_dirty = R.tev_dirty = R.vsf_dirty = true; /* plat_end_frame changed GL state */
    {   /* Pre-build the h3d program variants, one per frame: compiling the ones a new area needs
         * inside its loading frame (6 compile + link in the GL driver) was most of the overworld
         * area-switch hitch (77-95 ms frame, BOOT_LOG 156). Valid variants: skin 0..4 x light
         * 0..2 x prog 0..3 (60). ALBW_NO_PREWARM=1: build on first use only (comparison). */
        static int off = -1, next;
        if (off < 0) off = getenv("ALBW_NO_PREWARM") != NULL;
        while (!off && R.ext.h3d_vs_body && R.ext.h3d_defines && next < 5 * 3 * 4) {
            int skin = next % 5, light = (next / 5) % 3, prog = next / 15;
            next++;
            int v = skin | light << 3 | prog << 5;
            if (v < RENDER_PROG_VARIANTS && !R.variants[v].prog) { build_variant(v); break; }
        }
    }
    if (g_live > POOL_GC_HIGH) {
        uint32_t freed = 0;
        for (uint32_t s = 0; s < g_npool; s++) {
            PoolEnt *p = &g_pool[s];
            /* render-target / viewport / scissor binds are kept: the game records them once into
             * lists it copies later (renderer +0x16C: "bind the main buffer", copied by 0054D264
             * after every render-to-texture pass, possibly minutes after boot). Freed, the marker
             * went stale and the frame stayed on the 128x128 texture: black screen after the
             * Sanctuary cutscene (BOOT_LOG 160o). Few distinct ops (deduplicated). */
            if (p->live && !p->pinned && R.frame - p->last_frame > POOL_GC_AGE &&
                p->op.type != ROP_TARGET && p->op.type != ROP_VIEWPORT && p->op.type != ROP_SCISSOR) {
                p->live = 0;
                p->gen++;
                g_live--;
                VEC_PUSH(g_free, g_nfree, g_capfree);
                g_free[g_nfree++] = s;
                freed++;
            }
        }
        htab_rebuild(g_hcap);
        if (g_trace) rlog("op pool: freed %u, %u live", freed, g_live);
    }
    R.st.ops_pooled = g_live;
    unlock();
}

/* save states (native/runtime/savestate.c): the op pool by value. The markers in game memory (model
 * blobs translated at load, pinned) name pool slots, so a loaded state needs the pool exactly as it
 * was: same slots, generations and ops. Entry: gen, live, pinned, op length, op bytes. */
size_t render_pool_export(void **out) {
    lock();
    size_t n = 8;
    for (uint32_t s = 0; s < g_npool; s++) n += 4 + (g_pool[s].live ? op_len(&g_pool[s].op) : 0);
    uint8_t *b = malloc(n), *q = b;
    if (b) {
        memcpy(q, "POOL", 4); memcpy(q + 4, &g_npool, 4); q += 8;
        for (uint32_t s = 0; s < g_npool; s++) {
            const PoolEnt *p = &g_pool[s];
            uint16_t len = p->live ? (uint16_t)op_len(&p->op) : 0;
            q[0] = p->gen; q[1] = p->live; memcpy(q + 2, &len, 2);
            q[1] |= (uint8_t)(p->pinned << 1);
            q += 4;
            memcpy(q, &p->op, len);
            q += len;
        }
    }
    unlock();
    *out = b;
    return b ? n : 0;
}

int render_pool_import(const void *in, size_t n) {
    const uint8_t *q = in, *end = q + n;
    uint32_t np;
    if (n < 8 || memcmp(q, "POOL", 4)) return -1;
    memcpy(&np, q + 4, 4);
    q += 8;
    if (np > POOL_MAX) return -1;
    lock();
    if (np > g_cappool) { g_cappool = np; g_pool = xrealloc(g_pool, g_cappool * sizeof *g_pool); }
    g_npool = np;
    g_nfree = 0;
    g_live = 0;
    int bad = 0;
    for (uint32_t s = 0; s < np && !bad; s++) {
        uint16_t len;
        if (q + 4 > end) { bad = 1; break; }
        PoolEnt *p = &g_pool[s];
        memset(p, 0, sizeof *p);
        p->gen = q[0];
        p->live = q[1] & 1;
        p->pinned = (q[1] >> 1) & 1;
        memcpy(&len, q + 2, 2);
        q += 4;
        if (len > sizeof p->op || q + len > end) { bad = 1; break; }
        memcpy(&p->op, q, len);
        q += len;
        if (p->live) {
            p->hash = op_hash(&p->op);
            p->last_frame = R.frame;
            g_live++;
        } else {
            VEC_PUSH(g_free, g_nfree, g_capfree);
            g_free[g_nfree++] = s;
        }
    }
    if (bad) { g_npool = 0; g_nfree = 0; g_live = 0; }
    uint32_t cap = g_hcap ? g_hcap : 8192;
    while (g_live * 2 >= cap) cap *= 2;   /* size first: a full open-addressing table never finds a slot */
    htab_rebuild(cap);
    R.st.ops_pooled = g_live;
    unlock();
    return bad ? -1 : 0;
}

static int exec_depth;

void render_exec_op(const ROp *op) {
    if (dumping() && op->type != ROP_DRAW) {
        if (op->type == ROP_TARGET)
            rlog("dump: op TARGET c %#lx(%d) d %#lx(%d) %ux%u", (unsigned long)op->u.target.color, op->u.target.has_color,
                 (unsigned long)op->u.target.depth, op->u.target.has_depth, op->u.target.w, op->u.target.h);
        else if (op->type == ROP_PROGRAM)
            rlog("dump: op PROGRAM %u key %#lx", op->u.program.id, (unsigned long)op->u.program.key);
        else if (op->type == ROP_CALL)
            rlog("dump: op CALL %#lx +%u", (unsigned long)op->u.call.addr, op->u.call.size);
        else if (op->type == ROP_EXT)
            rlog("dump: op EXT kind %u", op->u.ext.kind);
        else
            rlog("dump: op %u", op->type);
    }
    switch (op->type) {
    case ROP_TARGET:
        render_set_target(op->u.target.has_color ? op->u.target.color : 0, op->u.target.cfmt,
                          op->u.target.has_depth ? op->u.target.depth : 0, op->u.target.dfmt, op->u.target.w,
                          op->u.target.h);
        break;
    case ROP_VIEWPORT: render_set_viewport(op->u.rect.x, op->u.rect.y, op->u.rect.w, op->u.rect.h); break;
    case ROP_SCISSOR: render_set_scissor(op->u.rect.enable, op->u.rect.x, op->u.rect.y, op->u.rect.w, op->u.rect.h); break;
    case ROP_FRAG: {
        R.frag = op->u.frag;
        R.frag_dirty = true;
        break;
    }
    case ROP_BLEND: {
        const RFragState *f = &op->u.frag;
        R.frag.blend_enable = f->blend_enable;
        R.frag.eq_rgb = f->eq_rgb; R.frag.eq_a = f->eq_a;
        R.frag.src_rgb = f->src_rgb; R.frag.dst_rgb = f->dst_rgb;
        R.frag.src_a = f->src_a; R.frag.dst_a = f->dst_a;
        memcpy(R.frag.blend_color, f->blend_color, 4);
        R.frag.logic_enable = 0; /* Blend::MakeCommand selects blend mode (0x100 byte 1) */
        R.frag_dirty = true;
        break;
    }
    case ROP_LOGICOP:
        R.frag.logic_enable = op->u.frag.logic_enable;
        R.frag.logic_op = op->u.frag.logic_op;
        R.frag_dirty = true;
        break;
    case ROP_ALPHATEST:
        R.frag.alpha_test = op->u.frag.alpha_test;
        R.frag.alpha_func = op->u.frag.alpha_func;
        R.frag.alpha_ref = op->u.frag.alpha_ref;
        break;
    case ROP_STENCIL: {
        const RFragState *f = &op->u.frag;
        R.frag.stencil_test = f->stencil_test; R.frag.stencil_func = f->stencil_func;
        R.frag.stencil_ref = f->stencil_ref; R.frag.stencil_mask = f->stencil_mask;
        R.frag.stencil_write = f->stencil_write; R.frag.stencil_fail = f->stencil_fail;
        R.frag.stencil_zfail = f->stencil_zfail; R.frag.stencil_zpass = f->stencil_zpass;
        R.frag_dirty = true;
        break;
    }
    case ROP_DEPTH:
        R.frag.depth_test = op->u.frag.depth_test;
        R.frag.depth_func = op->u.frag.depth_func;
        R.frag.depth_write = op->u.frag.depth_write;
        R.frag.color_mask = op->u.frag.color_mask;
        R.frag_dirty = true;
        break;
    case ROP_COLORMASK:
        R.frag.color_mask = op->u.frag.color_mask;
        R.frag_dirty = true;
        break;
    case ROP_CULL:
        R.frag.cull = op->u.frag.cull;
        R.frag_dirty = true;
        break;
    case ROP_DEPTHMAP:
        R.frag.depth_near = op->u.frag.depth_near;
        R.frag.depth_far = op->u.frag.depth_far;
        R.frag.poly_offset_enable = op->u.frag.poly_offset_enable;
        R.frag.poly_offset_units = op->u.frag.poly_offset_units;
        R.frag.wbuffer = op->u.frag.wbuffer;
        R.frag_dirty = true;
        break;
    case ROP_TEXUNIT: render_set_texunit(op->unit, &op->u.tex); break;
    case ROP_TEXCONFIG: render_set_texconfig(op->u.texcfg.enable_mask, op->u.texcfg.tex2_from_tc1); break;
    case ROP_TEV: render_set_tev(&op->u.tev); break;
    case ROP_TEV_CONST: render_set_tev_const(op->u.tev_const.stage_mask, op->u.tev_const.rgba); break;
    case ROP_TEV_STAGES: render_set_tev_stages(op->flags & 0x3F, &op->u.tev, op->unit); break;
    case ROP_PROGRAM: {
        int prog = (int)op->u.program.id, variant = 0;
        alias_get(op->u.program.key, &prog, &variant);
        render_set_program_variant(prog, variant);
        break;
    }
    case ROP_EXT:
        if (R.ext.op) R.ext.op(op);
        break;
    case ROP_FL: fraglight_exec(op); break;
    case ROP_FOG:
        R.fog.mode = op->u.fog.mode;
        R.fog.flip = op->u.fog.flip;
        for (int c = 0; c < 3; c++) R.fog.rgb[c] = op->u.fog.rgb[c] / 255.f;
        if (op->u.fog.key) R.fog.key = op->u.fog.key;
        R.frag_dirty = R.tev_dirty = true;
        break;
    case ROP_VS_FLOAT: render_set_vs_float(op->u.vsf.reg, op->u.vsf.v, op->unit); break;
    case ROP_VS_BOOL: render_set_vs_bool(op->u.vsb.mask); break;
    case ROP_ATTRIB:
        if (op->u.attrib.ptr)
            render_set_attrib(op->unit, (const void *)op->u.attrib.ptr, op->u.attrib.type, op->u.attrib.count,
                              op->u.attrib.stride);
        else
            render_set_attrib_const(op->unit, op->u.attrib.konst);
        break;
    case ROP_DRAW:
        {   /* debug: ALBW_SKIP_DRAW=1 drops every plain draw op (native producers, live blocks
               without an attribute base); 2 only those (flag 0x80) - find a draw on screen */
            static int sk = -1;
            if (sk < 0) { const char *e = getenv("ALBW_SKIP_DRAW"); sk = e ? atoi(e) : 0; }
            if (sk == 1 || (sk == 2 && (op->flags & 0x80))) break;
        }
        if (op->u.draw.indexed && (op->flags & 0x80)) {
            /* index address from a PICA index register (28-bit offset, base 0: sdk/h3d.c live
               translation): the top bits are those of the bound vertex arrays (same allocation) */
            uintptr_t low = op->u.draw.indices & 0x0FFFFFFFu, ia = low;
            for (int a = 0; a < RENDER_MAX_ATTRIBS; a++)
                if (R.attr[a].array && R.attr[a].ptr) {
                    uintptr_t top = R.attr[a].ptr & ~(uintptr_t)0x0FFFFFFFu, best = top | low;
                    for (int k = -1; k <= 1; k += 2) {
                        uintptr_t c = (uintptr_t)((intptr_t)top + k * (intptr_t)0x10000000) | low;
                        uintptr_t dc = c > R.attr[a].ptr ? c - R.attr[a].ptr : R.attr[a].ptr - c;
                        uintptr_t db = best > R.attr[a].ptr ? best - R.attr[a].ptr : R.attr[a].ptr - best;
                        if (dc < db) best = c;
                    }
                    ia = best;
                    break;
                }
            if (ia != low) render_draw_indexed(op->u.draw.prim, (const void *)ia, op->u.draw.index_u16, op->u.draw.count);
        } else if (op->u.draw.indexed)
            render_draw_indexed(op->u.draw.prim, (const void *)op->u.draw.indices, op->u.draw.index_u16, op->u.draw.count);
        else
            render_draw(op->u.draw.prim, op->u.draw.first, op->u.draw.count);
        break;
    case ROP_CALL:
        if (exec_depth < 8) {
            exec_depth++;
            /* command blobs of loaded model files run their natively decoded ops (render/h3d.c);
               any other range is scanned for our markers */
            if (!R.ext.call || !R.ext.call(op->u.call.addr, op->u.call.size))
                render_exec_cmds((const void *)op->u.call.addr, op->u.call.size);
            exec_depth--;
        }
        break;
    default: break;
    }
}

/* debug: ALBW_RAW_TRACE=<first>-<last> (render frames): raw PICA register writes found in executed
 * command ranges (the words between our markers, which the renderer never interprets): a per-frame
 * summary "reg: writes (last value)", to find GPU state the game writes itself and the port drops */
static int g_raw_on = -1;
static long g_raw_lo, g_raw_hi;
static uint32_t g_raw_cnt[0x300], g_raw_last[0x300];
static void raw_note(const uint32_t *w, uintptr_t avail) {
    if ((long)R.frame < g_raw_lo || (long)R.frame > g_raw_hi) return;
    uint32_t h = w[1], reg = h & 0xFFFF, mask = (h >> 16) & 0xF, n = (h >> 20) & 0xFF, cons = h >> 31;
    if ((h >> 28) & 7 || !mask || reg >= 0x300 || (uintptr_t)(8 + 4 * n) > avail) return;
    g_raw_cnt[reg]++; g_raw_last[reg] = w[0];
    for (uint32_t k = 0; k < n && cons; k++) if (reg + k + 1 < 0x300) { g_raw_cnt[reg + k + 1]++; g_raw_last[reg + k + 1] = w[2 + k]; }
}
static void raw_flush(void) {
    if (g_raw_on <= 0 || (long)R.frame < g_raw_lo || (long)R.frame > g_raw_hi) return;
    char line[4096]; int o = 0;
    for (int r = 0; r < 0x300 && o < (int)sizeof line - 40; r++)
        if (g_raw_cnt[r]) o += snprintf(line + o, sizeof line - (size_t)o, " %03x:%u(%08x)", r, g_raw_cnt[r], g_raw_last[r]);
    rlog("raw frame %lu:%s", (unsigned long)R.frame, o ? line : " none");
    memset(g_raw_cnt, 0, sizeof g_raw_cnt);
}

/* debug: ALBW_RAW_DUMP=<frame>: list, in stream order, the raw (non-marker) PICA writes of the
 * ranges executed in that frame - "reg=value" per write, markers as "[op type]" - to find the
 * producers that still write raw commands (they are never executed: see the header). */
static long g_rawdump = -2;
static void raw_dump_range(const void *addr, uint32_t size) {
    if (g_rawdump == -2) { const char *e = getenv("ALBW_RAW_DUMP"); g_rawdump = e ? atol(e) : -1; }
    if (g_rawdump < 0 || (long)R.frame != g_rawdump) return;
    const uint32_t *w = (const uint32_t *)(((uintptr_t)addr + 3) & ~(uintptr_t)3);
    uint32_t n = size / 4, i = 0;
    fprintf(stderr, "rawdump: range %p %u words\n", addr, n);
    char line[1024]; int o = 0;
    while (i + 1 < n) {
        uint32_t p0 = w[i], h = w[i + 1];
        if (h == RENDER_MARK) {
            const ROp *op = pool_get(p0);
            o += snprintf(line + o, sizeof line - (size_t)o, " [op%u]", op ? op->type : 99u);
            i += 2;
        } else {
            uint32_t reg = h & 0xFFFF, mask = (h >> 16) & 0xF, cnt = (h >> 20) & 0xFF, cons = h >> 31;
            if (reg >= 0x300 || !mask || i + 2 + cnt > n) { o += snprintf(line + o, sizeof line - (size_t)o, " ?%08x", h); i += 1; }
            else {
                o += snprintf(line + o, sizeof line - (size_t)o, " %03x=%x", reg, p0);
                if (cnt) o += snprintf(line + o, sizeof line - (size_t)o, "+%u%s", cnt, cons ? "c" : "");
                i += 2 + cnt; if (i & 1) i++;
            }
        }
        if (o > 900) { fprintf(stderr, "rawdump:%s\n", line); o = 0; }
    }
    if (o) fprintf(stderr, "rawdump:%s\n", line);
}

int render_exec_cmds(const void *addr, uint32_t size) {
    raw_dump_range(addr, size);
    if (g_raw_on < 0) {
        const char *e = getenv("ALBW_RAW_TRACE");
        g_raw_on = e != NULL;
        if (e) { char *x; g_raw_lo = strtol(e, &x, 10); g_raw_hi = *x == '-' ? atol(x + 1) : g_raw_lo; }
    }
    if (!addr || size < 8 || !render_ready()) return 0;
    /* a range reaching 4 GiB is a bad size (seen in the 64-bit build: size = 0 - addr from a caller's
     * zero end pointer); the 32-bit build skipped it only because addr + size wrapped to 0 */
    if ((uint64_t)(uintptr_t)addr + size >= 0x100000000ull) {
        static int warned;
        if (!warned++) fprintf(stderr, "render: command range %p + %#x reaches 4 GiB: skipped\n", addr, size);
        return 0;
    }
    uintptr_t a = ((uintptr_t)addr + 3) & ~(uintptr_t)3, e = (uintptr_t)addr + size;
    int n = 0;
    lock();
    /* only our own 2-word markers are looked for; nothing else in the range is interpreted */
    for (int seg = 0;; seg++) {
        uintptr_t call_slot = 0; /* return-segment size slot of the last CALL run in this segment */
        for (; a + 8 <= e; a += 4) {
            const uint32_t *w = (const uint32_t *)a;
            if (w[1] != RENDER_MARK) {
                if (g_raw_on && !((a - (uintptr_t)addr) & 7)) raw_note(w, e - a);
                continue;
            }
            const ROp *op = pool_get(w[0]);
            if (!op) {
                static int told;
                int st = getenv("ALBW_STALE_TRACE") != NULL;
                if (st && told < 40) {
                    told++;
                    uint32_t slot = (w[0] & 0xFFFFF) - 1;
                    rlog("stale marker %#x at %p: slot %u %s type %d gen now %u", w[0], (void *)a, slot,
                         slot < g_npool ? (g_pool[slot].live ? "live" : "freed") : "out of range",
                         slot < g_npool ? (int)g_pool[slot].op.type : -1, slot < g_npool ? g_pool[slot].gen : 0);
                }
                continue;
            }
            ROp copy = *op; /* the pool may grow (realloc) while the op runs */
            render_exec_op(&copy);
            n++;
            a += 4;
            call_slot = copy.type == ROP_CALL ? a + 4 : 0;
        }
        /* Chained segments (003CC090 / 003CC230 / 003CC054): a range that ENDS with a CALL (its
           slot words included) continues, like the hardware returning through CMDBUF ADDR1, with
           the return segment right after the call: *slot x 8 bytes (patched by the next CALL or by
           003CC054). Requests sized with the cursor's first-CALL offset (ctx[4]: 005FFF7C callers,
           002FE918 scene lists) cover only the first segment. A full-range request ends with the
           tail after its last CALL (slot = 0 or the call not last), so nothing runs twice. */
        if (!call_slot || call_slot + 8 < e || seg > 65536) break;
        uint32_t next = *(const uint32_t *)call_slot;
        if (!next || next > (1u << 21)) break;
        a = call_slot + 8;
        e = a + (uintptr_t)next * 8;
    }
    R.st.ops_run += (uint64_t)n;
    unlock();
    return n;
}

RStats render_stats(void) { return R.st; }

/* save states (native/runtime/savestate.c): game memory was replaced - re-read every texture
 * (large ones are otherwise re-uploaded only when announced by render_mem_written). The op pool
 * needs nothing: markers of pinned ops (model data) stay valid, per-frame ops are re-recorded. */
void render_ss_loaded(void) {
    for (size_t i = 0; i < R.ntex; i++) { R.tex[i].dirty = true; R.tex[i].hash = 0; }
}

/* debug (gdb): pool slot state = live | type << 8 | pinned << 16 | gen << 24 */
uint32_t render_debug_pool_slot(uint32_t slot) {
    if (slot >= g_npool) return 0xFFFFFFFFu;
    const PoolEnt *p = &g_pool[slot];
    return (uint32_t)p->live | (uint32_t)p->op.type << 8 | (uint32_t)p->pinned << 16 | (uint32_t)p->gen << 24;
}
