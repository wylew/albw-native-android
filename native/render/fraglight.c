/* 3DS fragment lighting: state, LUT texture, hand-written GLSL (see fraglight.h).
 *
 * ---------------------------------------------------------------- RENDER_FS_FRAGLIGHT
 * Hand-written GLSL 330 of the PICA fragment-lighting unit, written from its documented
 * behaviour (inputs, LUT indexing, light loop) - no shader or command word is translated.
 * Interface with the fragment program of render.c (compiled only into programs built with
 * FL = 1, i.e. the nw::h3d DefaultShader variants whose vertex program provides v_tangent):
 *   in  v_normal (view-space normal), v_tangent (view-space tangent, may be 0), v_view (vector
 *       from the fragment to the eye, view space, not normalised), textures t0..t2 (bump map)
 *   out fl_compute(out vec4 primary, out vec4 secondary)
 * Uniforms: fl_cfg   x light count (0 = lighting off), y enabled LUT mask (bit FLI_*: D0 D1 SP FR
 *                    RB RG RR), z Fresnel selector | clamp highlights << 2 | bump mode << 4 |
 *                    bump unit << 6 | renormalise << 8, w absolute-input mask (bit FLI_*)
 *           fl_in    selectors of D0 D1 SP FR / RB RG RR
 *           fl_scale scales of D0 D1 SP FR / RB RG RR
 *           fl_amb   global ambient
 *           fl_l     7 vec4 per light slot: diffuse, ambient, specular 0, specular 1,
 *                    (position | direction, directional), (spot direction, flags: 1 two-sided
 *                    diffuse, 2 geometric factor 0, 4 geometric factor 1, 8 spot, 16 distance
 *                    attenuation), (distance scale, distance bias, light id)
 *           fl_lut   256 x (registered LUTs + 1) RG32F atlas: row 0 zeros, row h = registry handle h,
 *                    (value, delta to the next entry); uploaded only when the registry changes
 *           fl_row   [24]: LUT id -> atlas row (the bound handle), sent with the other uniforms.
 *                    (Before: one row per LUT id, re-uploaded with glTexSubImage2D whenever a draw
 *                    bound other LUTs: ~16% of the GL thread's CPU on Android plus driver stalls,
 *                    as the texture was still in use by queued draws.)
 * Per light (slot k, light id i): L = normalize(directional ? P : P + V), H = normalize(V) + L,
 *   dot = two-sided ? |L.N| : max(L.N, 0); distance attenuation DA_i(clamp(scale |P + V| + bias));
 *   spot SP_i(input); geometric factor min(dot / H.H, 1); D0, D1, RR/RG/RB (RG/RB default to RR),
 *   specular = (D0 spec0 [geo0] + D1 refl spec1 [geo1]) [clamped where L.N <= 0];
 *   diffuse += (diffuse dot + ambient) DA SP; specular += specular DA SP;
 *   the LAST light's Fresnel factor replaces primary / secondary alpha (selector).
 * primary = clamp(diffuse + global ambient), secondary = clamp(specular).
 * LUT indexing: absolute inputs |x| in [0, 1] -> 256 entries; signed inputs x in [-1, 1] ->
 * 8-bit two's complement index (0..127 positive, 128..255 negative); linear interpolation with
 * the stored delta; result times the LUT scale. Shadows (no shadow maps on this path) are not
 * applied.
 */
#define _GNU_SOURCE
#include "fraglight.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../platform/platform.h"

const char RENDER_FS_FRAGLIGHT[] =
    "in vec3 v_tangent;\n"
    "uniform ivec4 fl_cfg; uniform ivec4 fl_in[2]; uniform vec4 fl_scale[2]; uniform vec4 fl_amb;\n"
    "uniform vec4 fl_l[56]; uniform sampler2D fl_lut; uniform int fl_row[24];\n"
    "int fl_sel(int i){ return i < 4 ? fl_in[0][i] : fl_in[1][i - 4]; }\n"
    "float fl_sc(int i){ return i < 4 ? fl_scale[0][i] : fl_scale[1][i - 4]; }\n"
    "bool fl_on(int i){ return ((fl_cfg.y >> i) & 1) != 0; }\n"
    "float fl_lut_u(int id, float x){\n"
    "  x = clamp(x, 0.0, 1.0) * 256.0; int i = min(int(x), 255);\n"
    "  vec2 e = texelFetch(fl_lut, ivec2(i, fl_row[id]), 0).rg; return e.r + e.g * (x - float(i));\n"
    "}\n"
    "float fl_lut_s(int id, float x){\n"
    "  x = clamp(x, -1.0, 1.0) * 128.0; int i = clamp(int(floor(x)), -128, 127); float d = x - float(i);\n"
    "  if (i < 0) i += 256;\n"
    "  vec2 e = texelFetch(fl_lut, ivec2(i, fl_row[id]), 0).rg; return e.r + e.g * d;\n"
    "}\n"
    /* LUT `i` (FLI_*) of PICA id `id` for the input it selects */
    "float fl_lookup(int i, int id, vec3 N, vec3 V, vec3 L, vec3 H, vec3 T, vec3 S){\n"
    "  int s = fl_sel(i); float x;\n"
    "  vec3 h = normalize(H);\n"
    "  if (s == 0) x = dot(N, h); else if (s == 1) x = dot(V, h); else if (s == 2) x = dot(N, V);\n"
    "  else if (s == 3) x = dot(L, N); else if (s == 4) x = dot(L, S);\n"
    "  else x = dot(h - N * dot(N, h), T);\n"
    "  float r = ((fl_cfg.w >> i) & 1) != 0 ? fl_lut_u(id, abs(x)) : fl_lut_s(id, x);\n"
    "  return r * fl_sc(i);\n"
    "}\n"
    "void fl_compute(vec4 t0, vec4 t1, vec4 t2, out vec4 prim, out vec4 sec){\n"
    "  prim = vec4(0.0); sec = vec4(0.0);\n"
    "  int n = fl_cfg.x; if (n == 0) return;\n"
    "  vec3 N = normalize(v_normal), V = normalize(v_view);\n"
    "  vec3 T = v_tangent - N * dot(N, v_tangent), B;\n"
    /* no tangent: the DefaultShader's quaternion from the normal alone (DVLE code 186-202:
       q = (n.xy / 2s, s, 0), s = sqrt((1 + n.z) / 2); q = (1, 0, 0, 0) when 1 + n.z <= 0), i.e. the
       frame q rotates x / y onto - for a flat normal (0, 0, 1): T = -x, B = -y */
    "  if (dot(T, T) < 1e-8) {\n"
    "    float k = 1.0 + N.z;\n"
    "    if (k > 1e-4) {\n"
    "      T = vec3(-N.z - N.y * N.y / k, N.x * N.y / k, N.x);\n"
    "      B = vec3(N.x * N.y / k, -N.z - N.x * N.x / k, N.y);\n"
    "    } else { T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, -1.0, 0.0); }\n"
    "  } else {\n"
    "    T = normalize(T); B = cross(N, T);\n"
    "  }\n"
    "  int bm = (fl_cfg.z >> 4) & 3;\n"
    "  if (bm != 0) {\n"
    "    int bs = (fl_cfg.z >> 6) & 3; vec4 bt = bs == 0 ? t0 : bs == 1 ? t1 : t2;\n"
    "    vec3 p = bt.rgb * 2.0 - 1.0;\n"
    "    if (((fl_cfg.z >> 8) & 1) != 0) p.z = sqrt(max(1.0 - p.x * p.x - p.y * p.y, 0.0));\n"
    "    vec3 q = normalize(T * p.x + B * p.y + N * p.z);\n"
    "    if (bm == 1) N = q; else T = q;\n"
    "  }\n"
    "  int fsel = fl_cfg.z & 3; bool clamp_hl = ((fl_cfg.z >> 2) & 1) != 0;\n"
    "  vec3 dsum = vec3(0.0), ssum = vec3(0.0); float pa = 1.0, sa = 1.0;\n"
    "  for (int k = 0; k < 8; k++) {\n"
    "    if (k >= n) break;\n"
    "    vec4 P = fl_l[k * 7 + 4], S = fl_l[k * 7 + 5], D = fl_l[k * 7 + 6];\n"
    "    int f = int(S.w + 0.5), id = int(D.z + 0.5);\n"
    "    vec3 L = normalize(P.w != 0.0 ? P.xyz : P.xyz + v_view);\n"
    "    vec3 H = V + L;\n"
    "    float ln = dot(L, N);\n"
    "    float dp = (f & 1) != 0 ? abs(ln) : max(ln, 0.0);\n"
    "    float chl = clamp_hl ? sign(dp) : 1.0;\n"
    "    float da = (f & 16) != 0 ? fl_lut_u(16 + id, D.x * length(P.xyz + v_view) + D.y) : 1.0;\n"
    "    float sp = (f & 8) != 0 ? fl_lookup(2, 8 + id, N, V, L, H, T, S.xyz) : 1.0;\n"
    "    float geo = 1.0;\n"
    "    if ((f & 6) != 0) { float hh = dot(H, H); geo = hh == 0.0 ? 0.0 : min(dp / hh, 1.0); }\n"
    "    float d0 = fl_on(0) ? fl_lookup(0, 0, N, V, L, H, T, S.xyz) : 1.0;\n"
    "    vec3 s0 = d0 * fl_l[k * 7 + 2].rgb * ((f & 2) != 0 ? geo : 1.0);\n"
    "    vec3 refl = vec3(1.0);\n"
    "    if (fl_on(6)) refl.r = fl_lookup(6, 6, N, V, L, H, T, S.xyz);\n"
    "    refl.g = fl_on(5) ? fl_lookup(5, 5, N, V, L, H, T, S.xyz) : refl.r;\n"
    "    refl.b = fl_on(4) ? fl_lookup(4, 4, N, V, L, H, T, S.xyz) : refl.r;\n"
    "    float d1 = fl_on(1) ? fl_lookup(1, 1, N, V, L, H, T, S.xyz) : 1.0;\n"
    "    vec3 s1 = d1 * refl * fl_l[k * 7 + 3].rgb * ((f & 4) != 0 ? geo : 1.0);\n"
    "    if (fl_on(3) && k == n - 1 && fsel != 0) {\n"
    "      float fr = fl_lookup(3, 3, N, V, L, H, T, S.xyz);\n"
    "      if ((fsel & 1) != 0) pa = fr;\n"
    "      if ((fsel & 2) != 0) sa = fr;\n"
    "    }\n"
    "    dsum += (fl_l[k * 7].rgb * dp + fl_l[k * 7 + 1].rgb) * da * sp;\n"
    "    ssum += (s0 + s1) * chl * da * sp;\n"
    "  }\n"
    "  dsum += fl_amb.rgb;\n"
    "  prim = clamp(vec4(dsum, pa), 0.0, 1.0); sec = clamp(vec4(ssum, sa), 0.0, 1.0);\n"
    "}\n";

/* ================================================================== GL functions */

#define GL_RG32F_ 0x8230
#define GL_RG_ 0x8227
static void (*pUniform4iv)(GLint, GLsizei, const GLint *);
static void (*pUniform1iv)(GLint, GLsizei, const GLint *);

/* ================================================================== LUT registry (load time) */

typedef struct { uintptr_t blob; float e[256][2]; } Lut;

static pthread_mutex_t g_mx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static Lut *g_lut; static uint32_t g_nlut, g_caplut;
typedef struct { uintptr_t key; FLLutCfg cfg; } MatCfg;
static MatCfg *g_mat; static size_t g_nmat, g_capmat;

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "fraglight: out of memory\n"); abort(); }
    return q;
}

/* LUT tables changed (a re-registered address): rows of bound ids are re-uploaded */
static uint32_t g_lut_gen;

void fraglight_lut_set(uintptr_t blob, const float e[256][2]) {
    pthread_mutex_lock(&g_mx);
    uint32_t i = 0;
    for (; i < g_nlut; i++)
        if (g_lut[i].blob == blob) break;
    if (i == g_nlut) {
        if (g_nlut == g_caplut) { g_caplut = g_caplut ? g_caplut * 2 : 64; g_lut = xrealloc(g_lut, g_caplut * sizeof *g_lut); }
        g_nlut++;
    }
    g_lut[i].blob = blob;
    memcpy(g_lut[i].e, e, sizeof g_lut[i].e);
    g_lut_gen++;
    pthread_mutex_unlock(&g_mx);
}

uint32_t fraglight_lut_find(uintptr_t blob) {
    if (!blob) return 0;
    pthread_mutex_lock(&g_mx);
    uint32_t r = 0;
    for (uint32_t i = g_nlut; i-- > 0;)
        if (g_lut[i].blob == blob) { r = i + 1; break; }
    pthread_mutex_unlock(&g_mx);
    return r;
}

void fraglight_material_set(uintptr_t key, const FLLutCfg *cfg) {
    pthread_mutex_lock(&g_mx);
    size_t i = 0;
    for (; i < g_nmat; i++)
        if (g_mat[i].key == key) break;
    if (i == g_nmat) {
        if (g_nmat == g_capmat) { g_capmat = g_capmat ? g_capmat * 2 : 256; g_mat = xrealloc(g_mat, g_capmat * sizeof *g_mat); }
        g_nmat++;
    }
    g_mat[i].key = key;
    g_mat[i].cfg = *cfg;
    pthread_mutex_unlock(&g_mx);
}

int fraglight_material_get(uintptr_t key, FLLutCfg *cfg) {
    pthread_mutex_lock(&g_mx);
    int ok = 0;
    for (size_t i = g_nmat; i-- > 0;)
        if (g_mat[i].key == key) { *cfg = g_mat[i].cfg; ok = 1; break; }
    pthread_mutex_unlock(&g_mx);
    return ok;
}

/* save states (native/runtime/savestate.c): the LUT and material registries by value (recorded
 * FL ops refer to LUTs by index + 1) */
size_t fraglight_export(void **out) {
    pthread_mutex_lock(&g_mx);
    size_t n = 12 + (size_t)g_nlut * sizeof *g_lut + g_nmat * sizeof *g_mat;
    uint8_t *b = malloc(n);
    if (b) {
        uint32_t nl = g_nlut, nm = (uint32_t)g_nmat;
        memcpy(b, "FLR1", 4); memcpy(b + 4, &nl, 4); memcpy(b + 8, &nm, 4);
        memcpy(b + 12, g_lut, (size_t)nl * sizeof *g_lut);
        memcpy(b + 12 + (size_t)nl * sizeof *g_lut, g_mat, nm * sizeof *g_mat);
    }
    pthread_mutex_unlock(&g_mx);
    *out = b;
    return b ? n : 0;
}
int fraglight_import(const void *in, size_t n) {
    const uint8_t *b = in;
    uint32_t nl, nm;
    if (n < 12 || memcmp(b, "FLR1", 4)) return -1;
    memcpy(&nl, b + 4, 4); memcpy(&nm, b + 8, 4);
    if (n != 12 + (size_t)nl * sizeof *g_lut + (size_t)nm * sizeof *g_mat) return -1;
    pthread_mutex_lock(&g_mx);
    if (nl > g_caplut) { g_caplut = nl; g_lut = xrealloc(g_lut, g_caplut * sizeof *g_lut); }
    if (nm > g_capmat) { g_capmat = nm; g_mat = xrealloc(g_mat, g_capmat * sizeof *g_mat); }
    g_nlut = nl; g_nmat = nm;
    memcpy(g_lut, b + 12, (size_t)nl * sizeof *g_lut);
    memcpy(g_mat, b + 12 + (size_t)nl * sizeof *g_lut, (size_t)nm * sizeof *g_mat);
    g_lut_gen++;
    pthread_mutex_unlock(&g_mx);
    return 0;
}

ROp fraglight_op(int kind, const void *payload, size_t n) {
    ROp op;
    memset(&op, 0, sizeof op);
    op.type = ROP_FL;
    op.u.fl.kind = (uint8_t)kind;
    if (n > sizeof op.u.fl.data) n = sizeof op.u.fl.data;
    memcpy(op.u.fl.data, payload, n);
    return op;
}

/* ================================================================== execution state */

typedef struct {
    uint8_t diffuse[3], ambient[3], spec0[3], spec1[3];
    uint8_t directional, two_sided, geo0, geo1;
    float pos[3], spot[3], da_scale, da_bias;
} LightState;

static struct {
    FLEnv env;
    LightState light[8];
    FLLutCfg in;
    uint32_t bound[24];      /* LUT id -> registry handle */
    uint32_t gen;            /* state generation (uniform upload) */
    uint32_t lut_gen_up;     /* g_lut_gen at the last texture upload */
    uint32_t tex_bound[24];  /* (unused since the atlas) */
    uint32_t atlas_rows;     /* rows in the LUT atlas (registry handles below it are valid) */
    bool rows_dirty;
    GLuint tex;
} S;

void fraglight_reset(void) {
    uint32_t g = S.gen;
    GLuint tex = S.tex;
    uint32_t tb[24];
    memcpy(tb, S.tex_bound, sizeof tb);
    memset(&S, 0, sizeof S);
    S.gen = g + 1;
    S.tex = tex;
    memcpy(S.tex_bound, tb, sizeof tb);
    for (int i = 0; i < FLI_COUNT; i++) S.in.abs |= (uint8_t)(1u << i);
}

const FLEnv *fraglight_env(void) { return &S.env; }
/* debug (ALBW_LIGHT_TRACE): hash of the lighting state a draw sees (env, lights, LUT inputs and
 * bindings, LUT content generation) */
uint64_t fraglight_state_hash(void) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char *parts[4] = {(const void *)&S.env, (const void *)S.light, (const void *)&S.in, (const void *)S.bound};
    const size_t sizes[4] = {sizeof S.env, sizeof S.light, sizeof S.in, sizeof S.bound};
    for (int k = 0; k < 4; k++)
        for (size_t i = 0; i < sizes[k]; i++) h = (h ^ parts[k][i]) * 1099511628211ull;
    return (h ^ g_lut_gen) * 1099511628211ull;
}
void fraglight_debug_print(FILE *f) {
    fprintf(f, "h3d-dump:   env enable %u n %u amb %u %u %u config %u lut_off %#x spot %#x da %#x\n", S.env.enable,
            S.env.nlights, S.env.ambient[0], S.env.ambient[1], S.env.ambient[2], S.env.config, S.env.lut_off,
            S.env.spot_mask, S.env.da_mask);
    for (int k = 0; k < S.env.nlights && k < 8; k++) {
        const LightState *l = &S.light[S.env.perm[k] & 7];
        fprintf(f, "h3d-dump:   light %u: dif %u %u %u amb %u %u %u s0 %u %u %u s1 %u %u %u dir %u pos %g %g %g\n",
                S.env.perm[k], l->diffuse[0], l->diffuse[1], l->diffuse[2], l->ambient[0], l->ambient[1], l->ambient[2],
                l->spec0[0], l->spec0[1], l->spec0[2], l->spec1[0], l->spec1[1], l->spec1[2], l->directional, l->pos[0],
                l->pos[1], l->pos[2]);
    }
    fprintf(f, "h3d-dump:   luts bound:");
    for (int i = 0; i < 24; i++) if (S.bound[i]) fprintf(f, " %d->%u", i, S.bound[i]);
    fprintf(f, " | in abs %#x sel %u %u %u %u %u %u %u\n", S.in.abs, S.in.sel[0], S.in.sel[1], S.in.sel[2], S.in.sel[3],
            S.in.sel[4], S.in.sel[5], S.in.sel[6]);
    fprintf(f, "h3d-dump:   fresnel sel %u bump mode %u unit %u renorm %u clamp %u scales %u %u %u %u\n", S.env.fresnel_sel,
            S.env.bump_mode, S.env.bump_sel, S.env.bump_renorm, S.env.clamp_highlights, S.in.scale[0], S.in.scale[1],
            S.in.scale[2], S.in.scale[3]);
    for (int i = 0; i < 24; i++) {
        uint32_t h = S.bound[i];
        if (!h || h > g_nlut) continue;
        fprintf(f, "h3d-dump:   lut %d (blob %#lx):", i, (unsigned long)g_lut[h - 1].blob);
        for (int k = 0; k < 256; k += 16) fprintf(f, " %.3f", g_lut[h - 1].e[k][0]);
        fprintf(f, " | 255 %.3f\n", g_lut[h - 1].e[255][0]);
    }
}

void fraglight_exec(const ROp *op) {
    const void *d = op->u.fl.data;
    switch (op->u.fl.kind) {
    case FL_ENV: memcpy(&S.env, d, sizeof S.env); break;
    case FL_LIGHT: {
        FLLight l;
        memcpy(&l, d, sizeof l);
        LightState *s = &S.light[l.id & 7];
        if (l.parts & FLL_COLOR) {
            memcpy(s->diffuse, l.diffuse, 3); memcpy(s->ambient, l.ambient, 3);
            memcpy(s->spec0, l.spec0, 3); memcpy(s->spec1, l.spec1, 3);
        }
        if (l.parts & FLL_CONFIG) {
            s->directional = l.directional; s->two_sided = l.two_sided; s->geo0 = l.geo0; s->geo1 = l.geo1;
        }
        if (l.parts & FLL_DIST) { s->da_scale = l.da_scale; s->da_bias = l.da_bias; }
        break;
    }
    case FL_POS: {
        FLPos p;
        memcpy(&p, d, sizeof p);
        for (int i = 0; i < p.n && i < 8; i++) {
            LightState *s = &S.light[p.l[i].id & 7];
            memcpy(s->pos, p.l[i].pos, sizeof s->pos);
            memcpy(s->spot, p.l[i].spot, sizeof s->spot);
            s->directional = p.l[i].directional;
        }
        break;
    }
    case FL_LUTCFG: {
        FLLutCfg c;
        memcpy(&c, d, sizeof c);
        S.in.abs = c.abs;
        for (int i = 0; i < FLI_COUNT; i++)
            if (c.set >> i & 1) { S.in.sel[i] = c.sel[i]; S.in.scale[i] = c.scale[i]; }
        break;
    }
    case FL_LUT: {
        FLLutBind b;
        memcpy(&b, d, sizeof b);
        if (b.id < 24 && S.bound[b.id] != b.lut) { S.bound[b.id] = b.lut; S.rows_dirty = true; }
        break;
    }
    case FL_POLYOFFSET: {
        FLPoly p;
        memcpy(&p, d, sizeof p);
        RFragState *f = render_frag_state();
        f->poly_offset_enable = p.enable;
        f->poly_offset_units = p.units;
        render_frag_dirty();
        break;
    }
    default: return;
    }
    S.gen++;
}

/* ================================================================== uniforms */

void fraglight_locations(unsigned prog, FLLoc *loc) {
    if (!pUniform4iv) pUniform4iv = (void (*)(GLint, GLsizei, const GLint *))SDL_GL_GetProcAddress("glUniform4iv");
    loc->cfg = glGetUniformLocation(prog, "fl_cfg");
    loc->in0 = glGetUniformLocation(prog, "fl_in");
    loc->scale = glGetUniformLocation(prog, "fl_scale");
    loc->amb = glGetUniformLocation(prog, "fl_amb");
    loc->light = glGetUniformLocation(prog, "fl_l");
    loc->lut = glGetUniformLocation(prog, "fl_lut");
    loc->row = glGetUniformLocation(prog, "fl_row");
    if (!pUniform1iv) pUniform1iv = (void (*)(GLint, GLsizei, const GLint *))SDL_GL_GetProcAddress("glUniform1iv");
    loc->gen = 0;
}

static float scale_of(int code) {
    static const float k[8] = {1, 2, 4, 8, 1, 1, 0.25f, 0.5f};
    return k[code & 7];
}

/* LUTs a config provides (bit FLI_*) */
static int config_luts(int c) {
    enum { D0 = 1 << FLI_D0, D1 = 1 << FLI_D1, SP = 1 << FLI_SP, FR = 1 << FLI_FR, RB = 1 << FLI_RB,
           RG = 1 << FLI_RG, RR = 1 << FLI_RR };
    switch (c) {
    case 0: return D0 | RR | SP;
    case 1: return RR | FR | SP;
    case 2: return RR | D0 | D1;
    case 3: return D0 | D1 | FR;
    case 4: return RR | RG | RB | D0 | D1 | SP;
    case 5: return RR | RG | RB | D0 | FR | SP;
    case 6: return RR | D0 | D1 | FR | SP;
    default: return 0x7F;
    }
}

static void upload_rows(void) {
    /* the whole LUT registry as one atlas (rows: 0 = zeros, h = handle h); rebuilt when it changes */
    static GLint max_rows;
    if (!max_rows) { glGetIntegerv(0x0D33 /* GL_MAX_TEXTURE_SIZE */, &max_rows); if (max_rows < 64) max_rows = 64; }
    if (!S.tex) {
        glGenTextures(1, &S.tex);
        glBindTexture(GL_TEXTURE_2D, S.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, S.tex);
    }
    pthread_mutex_lock(&g_mx);
    uint32_t rows = g_nlut + 1;
    if (rows > (uint32_t)max_rows) {
        static int warned;
        if (!warned++) fprintf(stderr, "fraglight: %u LUTs, the atlas holds %d: the rest read as 0\n", g_nlut, max_rows - 1);
        rows = (uint32_t)max_rows;
    }
    float (*buf)[2] = calloc((size_t)rows * 256, sizeof *buf);
    if (buf) {
        for (uint32_t h = 1; h < rows; h++) memcpy(buf + (size_t)h * 256, g_lut[h - 1].e, sizeof g_lut[h - 1].e);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F_, 256, (GLsizei)rows, 0, GL_RG_, GL_FLOAT, buf);
        free(buf);
        S.atlas_rows = rows;
        S.gen++;   /* the row uniforms depend on the atlas size: every program re-sends them */
    }
    S.lut_gen_up = g_lut_gen;
    pthread_mutex_unlock(&g_mx);
    S.rows_dirty = false;
}

/* ================================================================== specialised lighting GLSL
 * The configuration the generic fl_compute reads from uniforms (light count, enabled LUTs, input
 * selectors, absolute-input flags, fresnel / clamp / bump mode, per-light flags and LUT ids) as 4
 * key words, and an fl_compute with them baked in (render.c specialised programs; README: Android
 * performance: the generic loop was about half of the GPU time left). Values (colours, positions,
 * scales, spot / attenuation parameters, LUT rows) stay uniforms. Same maths as RENDER_FS_FRAGLIGHT. */
void fraglight_spec_key(uint32_t k[4]) {
    const FLEnv *e = &S.env;
    int n = e->enable ? (e->nlights ? e->nlights : 1) : 0;
    if (n > 8) n = 8;
    int luts = config_luts(e->config);
    static const int off_bit[6] = {FLI_D0, FLI_D1, FLI_FR, FLI_RR, FLI_RG, FLI_RB};
    for (int i = 0; i < 6; i++)
        if (e->lut_off >> i & 1) luts &= ~(1 << off_bit[i]);
    uint32_t cz = (uint32_t)((e->fresnel_sel & 3) | (e->clamp_highlights ? 4 : 0) | (e->bump_mode & 3) << 4 |
                             (e->bump_sel & 3) << 6 | (e->bump_renorm ? 1 << 8 : 0));
    k[0] = (uint32_t)n | (uint32_t)(luts & 0xFF) << 4 | (uint32_t)(S.in.abs & 0xFF) << 12 | cz << 20;
    k[1] = 0;
    for (int i = 0; i < FLI_COUNT; i++) k[1] |= (uint32_t)(S.in.sel[i] & 7) << (3 * i);
    k[2] = k[3] = 0;
    for (int j = 0; j < n; j++) {
        int id = e->perm[j] & 7;
        const LightState *l = &S.light[id];
        int f = (l->two_sided ? 1 : 0) | (l->geo0 ? 2 : 0) | (l->geo1 ? 4 : 0) |
                ((e->spot_mask >> id & 1) && (luts >> FLI_SP & 1) ? 8 : 0) | (e->da_mask >> id & 1 ? 16 : 0);
        uint32_t w = (uint32_t)(f | id << 5) & 0xFF;
        if (j < 4) k[2] |= w << (8 * j); else k[3] |= w << (8 * (j - 4));
    }
}

static void fl_lookup_expr(char *o, size_t n, int i, const char *id, uint32_t sel, uint32_t abs) {
    static const char *x[6] = {"dot(N, h)", "dot(V, h)", "dot(N, V)", "dot(L, N)", "dot(L, S.xyz)", "dot(h - N * dot(N, h), T)"};
    int s = (int)(sel >> (3 * i) & 7);
    const char *xe = x[s < 5 ? s : 5];
    if (abs >> i & 1) snprintf(o, n, "(fl_lut_u(%s, abs(%s)) * fl_scale[%d][%d])", id, xe, i / 4, i % 4);
    else snprintf(o, n, "(fl_lut_s(%s, %s) * fl_scale[%d][%d])", id, xe, i / 4, i % 4);
}

/* GLSL: uniforms + helpers + a specialised fl_compute for key k (malloc'd) */
char *fraglight_spec_glsl(const uint32_t k[4]) {
    size_t cap = 24576;
    char *s = xrealloc(NULL, cap);
    size_t len = 0;
#define E(...) do { len += (size_t)snprintf(s + len, cap - len, __VA_ARGS__); } while (0)
    /* the header of RENDER_FS_FRAGLIGHT up to fl_lookup (uniforms, LUT helpers) */
    const char *full = RENDER_FS_FRAGLIGHT, *cut = strstr(full, "/* LUT"), *cut2 = strstr(full, "float fl_lookup(");
    const char *end = cut2 ? cut2 : (cut ? cut : full + strlen(full));
    E("%.*s", (int)(end - full), full);
    int n = (int)(k[0] & 15), luts = (int)(k[0] >> 4 & 0xFF);
    uint32_t abs = k[0] >> 12 & 0xFF, cz = k[0] >> 20 & 0x1FF, sel = k[1];
    E("void fl_compute(vec4 t0, vec4 t1, vec4 t2, out vec4 prim, out vec4 sec){\n  prim = vec4(0.0); sec = vec4(0.0);\n");
    if (n == 0) { E("}\n"); return s; }
    E("  vec3 N = normalize(v_normal), V = normalize(v_view);\n"
      "  vec3 T = v_tangent - N * dot(N, v_tangent), B;\n"
      "  if (dot(T, T) < 1e-8) {\n"
      "    float k = 1.0 + N.z;\n"
      "    if (k > 1e-4) {\n"
      "      T = vec3(-N.z - N.y * N.y / k, N.x * N.y / k, N.x);\n"
      "      B = vec3(N.x * N.y / k, -N.z - N.x * N.x / k, N.y);\n"
      "    } else { T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, -1.0, 0.0); }\n"
      "  } else {\n    T = normalize(T); B = cross(N, T);\n  }\n");
    int bm = (int)(cz >> 4 & 3), bs = (int)(cz >> 6 & 3), renorm = (int)(cz >> 8 & 1), fsel = (int)(cz & 3), clamp_hl = (int)(cz >> 2 & 1);
    if (bm) {
        E("  {\n    vec3 p = %s.rgb * 2.0 - 1.0;\n", bs == 0 ? "t0" : bs == 1 ? "t1" : "t2");
        if (renorm) E("    p.z = sqrt(max(1.0 - p.x * p.x - p.y * p.y, 0.0));\n");
        E("    vec3 q = normalize(T * p.x + B * p.y + N * p.z);\n    %s = q;\n  }\n", bm == 1 ? "N" : "T");
    }
    E("  vec3 dsum = vec3(0.0), ssum = vec3(0.0); float pa = 1.0, sa = 1.0;\n");
    for (int j = 0; j < n; j++) {
        uint32_t w = (j < 4 ? k[2] >> (8 * j) : k[3] >> (8 * (j - 4))) & 0xFF;
        int f = (int)(w & 31), id = (int)(w >> 5 & 7);
        char lk[512], idb[16];
        E("  {\n    vec4 P = fl_l[%d], S = fl_l[%d], D = fl_l[%d];\n", j * 7 + 4, j * 7 + 5, j * 7 + 6);
        E("    vec3 L = normalize(P.w != 0.0 ? P.xyz : P.xyz + v_view);\n    vec3 H = V + L; vec3 h = normalize(H);\n");
        E("    float ln = dot(L, N);\n    float dp = %s;\n", (f & 1) ? "abs(ln)" : "max(ln, 0.0)");
        E("    float chl = %s;\n", clamp_hl ? "sign(dp)" : "1.0");
        if (f & 16) { snprintf(idb, sizeof idb, "%d", 16 + id); E("    float da = fl_lut_u(%s, D.x * length(P.xyz + v_view) + D.y);\n", idb); }
        else E("    float da = 1.0;\n");
        if (f & 8) { snprintf(idb, sizeof idb, "%d", 8 + id); fl_lookup_expr(lk, sizeof lk, 2, idb, sel, abs); E("    float sp = %s;\n", lk); }
        else E("    float sp = 1.0;\n");
        if (f & 6) E("    float hh = dot(H, H); float geo = hh == 0.0 ? 0.0 : min(dp / hh, 1.0);\n");
        if (luts >> 0 & 1) { fl_lookup_expr(lk, sizeof lk, 0, "0", sel, abs); E("    float d0 = %s;\n", lk); } else E("    float d0 = 1.0;\n");
        E("    vec3 s0 = d0 * fl_l[%d].rgb%s;\n", j * 7 + 2, (f & 2) ? " * geo" : "");
        E("    vec3 refl = vec3(1.0);\n");
        if (luts >> 6 & 1) { fl_lookup_expr(lk, sizeof lk, 6, "6", sel, abs); E("    refl.r = %s;\n", lk); }
        if (luts >> 5 & 1) { fl_lookup_expr(lk, sizeof lk, 5, "5", sel, abs); E("    refl.g = %s;\n", lk); } else E("    refl.g = refl.r;\n");
        if (luts >> 4 & 1) { fl_lookup_expr(lk, sizeof lk, 4, "4", sel, abs); E("    refl.b = %s;\n", lk); } else E("    refl.b = refl.r;\n");
        if (luts >> 1 & 1) { fl_lookup_expr(lk, sizeof lk, 1, "1", sel, abs); E("    float d1 = %s;\n", lk); } else E("    float d1 = 1.0;\n");
        E("    vec3 s1 = d1 * refl * fl_l[%d].rgb%s;\n", j * 7 + 3, (f & 4) ? " * geo" : "");
        if ((luts >> 3 & 1) && j == n - 1 && fsel) {
            fl_lookup_expr(lk, sizeof lk, 3, "3", sel, abs);
            E("    float fr = %s;\n", lk);
            if (fsel & 1) E("    pa = fr;\n");
            if (fsel & 2) E("    sa = fr;\n");
        }
        E("    dsum += (fl_l[%d].rgb * dp + fl_l[%d].rgb) * da * sp;\n    ssum += (s0 + s1) * chl * da * sp;\n  }\n", j * 7, j * 7 + 1);
    }
    E("  dsum += fl_amb.rgb;\n  prim = clamp(vec4(dsum, pa), 0.0, 1.0); sec = clamp(vec4(ssum, sa), 0.0, 1.0);\n}\n");
#undef E
    if (len >= cap) { fprintf(stderr, "fraglight: specialised GLSL too long\n"); abort(); }
    return s;
}

void fraglight_upload(FLLoc *loc, int unit) {
    if (loc->cfg < 0) return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    if (!S.tex || S.lut_gen_up != g_lut_gen) upload_rows();
    else glBindTexture(GL_TEXTURE_2D, S.tex);
    glActiveTexture(GL_TEXTURE0);
    if (loc->gen == S.gen && loc->gen) return;
    loc->gen = S.gen;
    const FLEnv *e = &S.env;
    int n = e->enable ? (e->nlights ? e->nlights : 1) : 0;
    if (n > 8) n = 8;
    int luts = config_luts(e->config);
    /* environment disables (D0 D1 FR RR/RG/RB); SP is per light */
    static const int off_bit[6] = {FLI_D0, FLI_D1, FLI_FR, FLI_RR, FLI_RG, FLI_RB};
    for (int i = 0; i < 6; i++)
        if (e->lut_off >> i & 1) luts &= ~(1 << off_bit[i]);
    GLint cfg[4] = {n, luts,
                    (e->fresnel_sel & 3) | (e->clamp_highlights ? 4 : 0) | (e->bump_mode & 3) << 4 |
                        (e->bump_sel & 3) << 6 | (e->bump_renorm ? 1 << 8 : 0),
                    S.in.abs};
    pUniform4iv(loc->cfg, 1, cfg);
    if (loc->row >= 0 && pUniform1iv) {
        GLint row[24];
        for (int i = 0; i < 24; i++) row[i] = S.bound[i] < S.atlas_rows ? (GLint)S.bound[i] : 0;
        pUniform1iv(loc->row, 24, row);
    }
    GLint in[8] = {0};
    float sc[8] = {0};
    for (int i = 0; i < FLI_COUNT; i++) { in[i] = S.in.sel[i] & 7; sc[i] = scale_of(S.in.scale[i]); }
    pUniform4iv(loc->in0, 2, in);
    glUniform4fv(loc->scale, 2, sc);
    float amb[4] = {e->ambient[0] / 255.f, e->ambient[1] / 255.f, e->ambient[2] / 255.f, 0};
    glUniform4fv(loc->amb, 1, amb);
    float l[56 * 4];
    memset(l, 0, sizeof l);
    for (int k = 0; k < n; k++) {
        int id = e->perm[k] & 7;
        const LightState *s = &S.light[id];
        float *v = l + k * 28;
        for (int c = 0; c < 3; c++) {
            v[0 + c] = s->diffuse[c] / 255.f;
            v[4 + c] = s->ambient[c] / 255.f;
            v[8 + c] = s->spec0[c] / 255.f;
            v[12 + c] = s->spec1[c] / 255.f;
            v[16 + c] = s->pos[c];
            v[20 + c] = s->spot[c];
        }
        v[19] = s->directional ? 1.0f : 0.0f;
        int f = (s->two_sided ? 1 : 0) | (s->geo0 ? 2 : 0) | (s->geo1 ? 4 : 0) |
                ((e->spot_mask >> id & 1) && (luts >> FLI_SP & 1) ? 8 : 0) | (e->da_mask >> id & 1 ? 16 : 0);
        v[23] = (float)f;
        v[24] = s->da_scale;
        v[25] = s->da_bias;
        v[26] = (float)id;
    }
    glUniform4fv(loc->light, 56, l);
}
