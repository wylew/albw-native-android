/* nw::h3d model draw backend: see h3d.h.
 *
 * ---------------------------------------------------------------- RENDER_VS_H3D
 * Hand-written GLSL 330 for Shader/nwh3d_DefaultShader.bch (EU/RegionBoot.szs). The program was
 * READ (DVLE symbol tables + a disassembly listing, as a reading aid) and rewritten by hand; no
 * bytecode is translated. Interface = the game's: input registers v0..v11 (aPosition, aNormal,
 * aTangent, aColor, aTexCoord0..2, aBoneIndex, aBoneWeight, aUserAttribute0..2), float registers
 * c0..c95, bool registers b0..b15 (bit n of `b`), int register i0 (LightCt):
 *   c0-c2 WrldMtx (not read by the program)  c3-c5 NormMtx   c6 PosOffs   c7-c8 IrScale
 *   c10 TexcMap   c11-c13 TexMtx0  c14-c16 TexMtx1  c17-c18 TexMtx2  c19 TexTran  c20 MatAmbi
 *   c21 MatDiff  c22 HslGCol  c23 HslSCol  c24 HslSDir  c25-c84 UnivReg  c86-c89 ProjMtx
 *   c90-c92 ViewMtx  (c93-c95: program constants (0,1,2,3) (1/8,1/256,1/2,1/4) (3,4,5,6))
 *   b1 IsSmoSk  b2 IsRgdSk  b3 IsQuate  b5 IsHemiL  b6 IsHemiO  b7 IsVertA  b8 IsBoneW
 *   b9-b11 UvMap0-2  b12 IsVertL  b13 IsTex1  b14 IsTex2
 * What it computes (per vertex):
 *   - p = v0 * c7.x + c6 (w = 1), n = v1 * c7.y, t = v2 * c7.z; a normal / tangent exists when
 *     its scale (c7.y / c7.z) is non-zero.
 *   - skinning with the UnivReg palette (3 registers per bone, a0 = 3 * bone index):
 *       smooth: sum of w_k * (bone_k * p) over bones x, y, z (if aBoneWeight.z != 0) and
 *               w (if b8 and aBoneWeight.w != 0), w_k = aBoneWeight_k * c8.w; same for n, t;
 *       rigid (b2): bone aBoneIndex.x; neither: UnivReg[0] = the model (world) matrix.
 *     The result is world space; view space = ViewMtx * world, NormMtx * normal (both 3x4 / 3x3),
 *     clip = ProjMtx * view. The view vector output is -view position.
 *   - colour: MatDiff, or with a vertex colour (c7.w != 0): rgb = MatAmbi.w * colour * c7.w,
 *     alpha = MatDiff.a (* vertex alpha if b7); lit programs add per-vertex lights (b12, i0+1
 *     lights stored downward from c79, 6 registers each) and hemisphere lighting (b5,
 *     mix(HslGCol, HslSCol, dot(HslSDir.xyz, n) * HslSDir.w + HslSDir.w) * MatDiff).
 *   - texcoord 0..2 (programs 0..3 output 3, 2, 1, 0 of them): attribute uv (b9..b11; TexcMap
 *     selects aTexCoord0/1/2) as (u, v, 1, 1) (DVLE 316-323: `mov r6.zw, c93.xxyy`) dotted with the
 *     texture matrix rows swizzled .xywz: u m0 + v m1 + m3 + m2 (the translation is m3, written by
 *     003FA8AC; reading it as (u, v, 0, 1) dropped it: texture scroll animations stood still);
 *     otherwise generated: TexcMap 3 = cube reflection vector, 4 = sphere map (n * 0.5 + 0.5),
 *     else projection of the world position (texture 0: + TexTran.xy * q; texture 1: / q +
 *     TexTran.zw), texture 1/2 generation only when b13/b14.
 * Variants (compile-time, like the 15 DVLBs): H3D_SKIN 0 any (b1 smooth 4 bones / rigid / none),
 * 1 -rs (rigid / none only), 2 -ss (smooth 4 always), 3 -s2 (smooth 2 always), 4 -u2 (b1 smooth 2
 * / rigid / none); H3D_LIGHT 0 any (b12 vertex lights, b5 hemisphere), 1 -nl (none), 2 -hs
 * (hemisphere always); H3D_PROG 0..3 (texcoord outputs 3..0).
 * Not reproduced: the normal / tangent quaternion output (o1, consumed only by fragment
 * lighting): the view-space normal, tangent and view vector are passed instead (v_normal,
 * v_tangent, v_view; render/fraglight.c rebuilds the tangent frame per fragment).
 * PICA clip space (z in [-w, 0]) -> GL as in render.c (pica_out).
 */
#define _GNU_SOURCE
#include "h3d.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../platform/platform.h"
#include "fraglight.h"

const char RENDER_VS_H3D[] =
    "layout(location = 0) in vec4 v0; layout(location = 1) in vec4 v1; layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3; layout(location = 4) in vec4 v4; layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6; layout(location = 7) in vec4 v7; layout(location = 8) in vec4 v8;\n"
    "uniform vec4 c[96]; uniform int b; uniform ivec4 i0;\n"
    "out vec4 v_color; out vec4 v_tc0; out vec2 v_tc1; out vec2 v_tc2; out vec3 v_normal; out vec3 v_view;\n"
    "out vec3 v_tangent;\n"
    "bool B(int n){ return ((b >> n) & 1) != 0; }\n"
    "vec4 pica_out(vec4 p){ return vec4(p.x, p.y, -(2.0 * p.z + p.w), p.w); }\n"
    "vec4 p = vec4(0.0), r10 = vec4(0.0), r15 = vec4(0.0);\n"
    "vec3 n = vec3(0.0), t = vec3(0.0), r12 = vec3(0.0), r11 = vec3(0.0), r14 = vec3(0.0), r13 = vec3(0.0);\n"
    "bool hasN = false, hasT = false;\n"
    /* one bone of a smooth skin: a0 = idx (3 * bone), weight w */
    "void bone_add(float idx, float w){\n"
    "  int a = clamp(int(idx), 0, 57);\n"
    "  vec4 R0 = c[25 + a], R1 = c[26 + a], R2 = c[27 + a];\n"
    "  r10.xyz += w * vec3(dot(R0, p), dot(R1, p), dot(R2, p));\n"
    "  if (hasN) r12 += w * vec3(dot(R0.xyz, n), dot(R1.xyz, n), dot(R2.xyz, n));\n"
    "  if (hasN && hasT) r11 += w * vec3(dot(R0.xyz, t), dot(R1.xyz, t), dot(R2.xyz, t));\n"
    "}\n"
    "void skin_smooth(bool four){\n"
    "  r10 = vec4(0.0); r12 = vec3(0.0); r11 = vec3(0.0);\n"
    "  vec4 idx = v7 * 3.0; float s = c[8].w;\n"
    "  bone_add(idx.x, s * v8.x); bone_add(idx.y, s * v8.y);\n"
    "  if (four) {\n"
    "    if (v8.z != 0.0) bone_add(idx.z, s * v8.z);\n"
    "    if (B(8) && v8.w != 0.0) bone_add(idx.w, s * v8.w);\n"
    "  }\n"
    "  r10.w = 1.0;\n"
    "}\n"
    "void skin_rigid(){\n"
    "  int a = B(2) ? clamp(int(3.0 * v7.x), 0, 57) : 0;\n"
    "  vec4 R0 = c[25 + a], R1 = c[26 + a], R2 = c[27 + a];\n"
    "  r10 = vec4(dot(R0, p), dot(R1, p), dot(R2, p), 1.0);\n"
    "  r12 = vec3(dot(R0.xyz, n), dot(R1.xyz, n), dot(R2.xyz, n));\n"
    "  r11 = vec3(dot(R0.xyz, t), dot(R1.xyz, t), dot(R2.xyz, t));\n"
    "}\n"
    "vec3 nrm3(vec3 v){ return vec3(dot(c[3].xyz, v), dot(c[4].xyz, v), dot(c[5].xyz, v)); }\n"
    "vec4 r9 = vec4(0.0); bool lit = false;\n"
    "void hemi(){\n"
    "  float k = dot(c[24].xyz, r14) * c[24].w + c[24].w;\n"
    "  vec4 h = (c[23] - c[22]) * k + c[22];\n"
    "  if (B(6)) h *= r9.w;\n"
    "  r9.xyz = h.xyz * c[21].xyz + r9.xyz;\n"
    "  lit = true;\n"
    "}\n"
    "void vertex_lights(){\n"
    "  int a = 0;\n"
    "  for (int k = 0; k <= min(i0.x, 7); k++, a -= 6) {\n"
    "    vec4 L = c[81 + a]; float ndl, att = 1.0;\n"
    "    bool dirl = L.w == 0.0, att_on = c[83 + a].w == 1.0;\n"
    "    if (dirl && !att_on) {\n"
    "      ndl = dot(L.xyz, r14);\n"
    "    } else {\n"
    "      vec3 d = L.xyz - r15.xyz;\n"
    "      if (att_on) { float d2 = dot(d, d); att = 1.0 / dot(c[83 + a].xyz, vec3(1.0, d2 * d2, d2)); }\n"
    "      vec4 sp = c[82 + a];\n"
    "      d = normalize(d);\n"
    "      if (sp.w == 1.0) {\n"
    "        float cs = dot(sp.xyz, -d);\n"
    "        att *= cs < c[84 + a].y ? 0.0 : exp2(c[84 + a].x * log2(cs));\n"
    "      }\n"
    "      ndl = dot(r14, d);\n"
    "    }\n"
    "    if (att > 0.0) {\n"
    "      vec4 ld = c[80 + a] * c[21];\n"
    "      r9.xyz += c[20].xyz * c[79 + a].xyz + max(ndl, 0.0) * ld.xyz * att;\n"
    "      r9.w += ld.w;\n"
    "    }\n"
    "  }\n"
    "  lit = true;\n"
    "}\n"
    "vec2 uv_src(float map){\n"
    "  vec2 r = (map != 1.0 && map != 2.0) ? c[8].x * v4.xy : (map == 1.0) ? c[8].y * v5.xy : c[8].z * v6.xy;\n"
    "#ifdef H3D_FLIPV\n  r.y = 1.0 - r.y;\n#endif\n"
    "  return r;\n"
    "}\n"
    "vec4 sphere(){ return vec4(r14.xy * 0.5 + 0.5, 1.0, 1.0); }\n"
    "vec4 reflection(){ vec3 e = normalize(-r15.xyz); return vec4(2.0 * dot(e, r14) * r14 - e, 0.0); }\n"
    "void main(){\n"
    "  p = vec4(v0.xyz * c[7].x + c[6].xyz, 1.0); n = v1.xyz * c[7].y; t = v2.xyz * c[7].z;\n"
    "  hasN = c[7].y != 0.0; hasT = c[7].z != 0.0;\n"
    "#if H3D_SKIN == 0\n"
    "  if (B(1)) skin_smooth(true); else skin_rigid();\n"
    "#elif H3D_SKIN == 1\n"
    "  skin_rigid();\n"
    "#elif H3D_SKIN == 2\n"
    "  skin_smooth(true);\n"
    "#elif H3D_SKIN == 3\n"
    "  skin_smooth(false);\n"
    "#else\n"
    "  if (B(1)) skin_smooth(false); else skin_rigid();\n"
    "#endif\n"
    "  r15 = vec4(dot(c[90], r10), dot(c[91], r10), dot(c[92], r10), 1.0);\n"
    "  r14 = hasN ? normalize(nrm3(r12)) : n;\n"
    "  r13 = (hasN && hasT) ? normalize(nrm3(r11)) : t;\n"
    "  gl_Position = pica_out(vec4(dot(c[86], r15), dot(c[87], r15), dot(c[88], r15), dot(c[89], r15)));\n"
    "  v_view = -r15.xyz;\n"
    "  v_normal = hasN ? r14 : vec3(0.0, 0.0, 1.0);\n"
    "  v_tangent = (hasN && hasT) ? r13 : vec3(0.0);\n"
    /* colour */
    "  bool hasC = c[7].w != 0.0; vec4 vc = c[7].w * v3; lit = false;\n"
    "#if H3D_LIGHT == 1\n"
    "  r9 = c[21];\n"
    "  if (hasC) { if (B(7)) r9.w *= vc.w; r9.xyz = c[20].w * vc.xyz; }\n"
    "#else\n"
    "  r9 = vec4(0.0, 0.0, 0.0, c[21].w);\n"
    "  if (hasC) { if (B(7)) r9.w *= vc.w; r9.xyz = c[20].w * vc.xyz; lit = true; }\n"
    "#if H3D_LIGHT == 2\n"
    "  hemi();\n"
    "#else\n"
    "  if (B(12)) vertex_lights();\n"
    "  if (B(5)) hemi();\n"
    "  if (!lit) r9 = c[21];\n"
    "#endif\n"
    "#endif\n"
    "  v_color = max(r9, 0.0);\n"
    /* texture coordinates */
    "  v_tc0 = vec4(0.0); v_tc1 = vec2(0.0); v_tc2 = vec2(0.0);\n"
    "#if H3D_PROG <= 2\n"
    "  {\n"
    "    float map = c[10].x; vec4 o;\n"
    "    if (B(9)) {\n"
    "      vec4 r6 = vec4(uv_src(map), 1.0, 1.0);\n"
    "      o = vec4(dot(c[11].xywz, r6), dot(c[12].xywz, r6), 0.0, 0.0);\n"
    "    } else if (map != 3.0 && map != 4.0) {\n"
    "      vec3 q = vec3(dot(c[11], r10), dot(c[12], r10), dot(c[13], r10));\n"
    "      o = vec4(q.xy + c[19].xy * q.z, q.z, 0.0);\n"
    "    } else if (map == 3.0) {\n"
    "      vec3 r6 = reflection().xyz;\n"
    "      o = vec4(dot(c[11].xyz, r6), dot(c[12].xyz, r6), dot(c[13].xyz, r6), 0.0);\n"
    "    } else {\n"
    "      vec4 r6 = sphere();\n"
    "      o = vec4(dot(c[11], r6), dot(c[12], r6), 0.0, 0.0);\n"
    "    }\n"
    "    v_tc0 = vec4(o.xy, o.z, o.z);\n"
    "  }\n"
    "#endif\n"
    "#if H3D_PROG <= 1\n"
    "  {\n"
    "    float map = c[10].y;\n"
    "    if (B(10)) {\n"
    "      vec4 r6 = vec4(uv_src(map), 1.0, 1.0);\n"
    "      v_tc1 = vec2(dot(c[14].xywz, r6), dot(c[15].xywz, r6));\n"
    "    } else if (B(13)) {\n"
    "      if (map != 3.0 && map != 4.0) {\n"
    "        vec3 q = vec3(dot(c[14], r10), dot(c[15], r10), dot(c[16], r10));\n"
    "        v_tc1 = q.xy / q.z + c[19].zw;\n"
    "      } else {\n"
    "        vec4 r6 = sphere();\n"
    "        v_tc1 = vec2(dot(c[14], r6), dot(c[15], r6));\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "#endif\n"
    "#if H3D_PROG == 0\n"
    "  {\n"
    "    float map = c[10].z;\n"
    "    if (B(11)) {\n"
    "      vec4 r6 = vec4(uv_src(map), 1.0, 1.0);\n"
    "      v_tc2 = vec2(dot(c[17].xywz, r6), dot(c[18].xywz, r6));\n"
    "    } else if (B(14)) {\n"
    "      vec4 r6 = sphere();\n"
    "      v_tc2 = vec2(dot(c[17], r6), dot(c[18], r6));\n"
    "    }\n"
    "  }\n"
    "#endif\n"
    "}\n";

void h3d_variant_defines(int variant, char *buf, size_t n) {
    static int fv = -1; /* experiment: ALBW_H3D_FLIPV=1 flips the v of the model texture coordinates */
    if (fv < 0) fv = getenv("ALBW_H3D_FLIPV") != NULL;
    snprintf(buf, n, "#define H3D_SKIN %d\n#define H3D_LIGHT %d\n#define H3D_PROG %d\n%s", variant & 7,
             (variant >> 3) & 3, (variant >> 5) & 3, fv ? "#define H3D_FLIPV 1\n" : "");
}

int h3d_variant_of_name(const char *name) {
    if (!name || name[0] < '0' || name[0] > '3' || name[1] != '@' || strncmp(name + 2, "DefaultShader", 13))
        return -1;
    int prog = name[0] - '0', skin = H3D_SKIN_ANY, light = H3D_LIGHT_ANY;
    for (const char *s = name + 15; *s == '-'; s += 3) {
        if (!strncmp(s, "-rs", 3)) skin = H3D_SKIN_RS;
        else if (!strncmp(s, "-ss", 3)) skin = H3D_SKIN_SS;
        else if (!strncmp(s, "-s2", 3)) skin = H3D_SKIN_S2;
        else if (!strncmp(s, "-u2", 3)) skin = H3D_SKIN_U2;
        else if (!strncmp(s, "-nl", 3)) light = H3D_LIGHT_NL;
        else if (!strncmp(s, "-hs", 3)) light = H3D_LIGHT_HS;
        else return -1;
    }
    return H3D_VARIANT(skin, light, prog);
}

/* ================================================================== registry */

typedef struct {
    uintptr_t addr;
    uint32_t bytes;
    int n;
    ROp *ops;
} Blob;   /* layout kept: the table lives in pmem, which save states restore by value */



typedef struct {
    uintptr_t addr;
    uint32_t count;
    uint8_t u16;
    unsigned ibo;
    uint32_t max_index;
    int dirty;
} Ibuf;

static pthread_mutex_t g_mx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static Blob *g_blob; static size_t g_nblob, g_capblob;     /* open addressing on addr */
static H3dVtx **g_vtx; static size_t g_nvtx, g_capvtx;
static Ibuf **g_ib; static size_t g_nib, g_capib;   /* all index buffers */
static Ibuf **g_ibh; static size_t g_capibh;         /* open-addressing index on (addr, count) */
static H3dVtx *g_cur;
static int g_mat_variant = H3D_VARIANT(H3D_SKIN_ANY, H3D_LIGHT_NL, 0); /* last material's shader */
static H3dStats g_st;

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "h3d: out of memory\n"); abort(); }
    return q;
}

/* The registry the recorded ops point into (vertex layouts, blob ops) lives in the fixed arena
 * (runtime/pmem.c, persistent save states); weak: tests and tools without it use the C heap. */
extern void *pmem_calloc(size_t a, size_t b) __attribute__((weak));
extern void *pmem_realloc(void *p, size_t n) __attribute__((weak));
extern void pmem_free(void *p) __attribute__((weak));
static void *pc(size_t a, size_t b) {
    void *p = pmem_calloc ? pmem_calloc(a ? a : 1, b) : calloc(a ? a : 1, b);
    if (!p) { fprintf(stderr, "h3d: out of memory\n"); abort(); }
    return p;
}
static void *pr(void *p, size_t n) {
    void *q = pmem_realloc ? pmem_realloc(p, n ? n : 1) : realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "h3d: out of memory\n"); abort(); }
    return q;
}
static void pf(void *p) { if (pmem_free) pmem_free(p); else free(p); }

static size_t bhash(uintptr_t a, size_t cap) { return (size_t)((a >> 2) * 2654435761u) & (cap - 1); }

static void blob_rehash(size_t cap) {
    Blob *old = g_blob;
    size_t oldcap = g_capblob;
    g_blob = pc(cap, sizeof *g_blob);
    g_capblob = cap;
    for (size_t i = 0; i < oldcap; i++) {
        if (!old[i].addr) continue;
        size_t h = bhash(old[i].addr, cap);
        while (g_blob[h].addr) h = (h + 1) & (cap - 1);
        g_blob[h] = old[i];
    }
    pf(old);
}

static Blob *blob_find(uintptr_t addr) {
    if (!g_capblob || !addr) return NULL;
    for (size_t h = bhash(addr, g_capblob);; h = (h + 1) & (g_capblob - 1)) {
        if (!g_blob[h].addr) return NULL;
        if (g_blob[h].addr == addr) return &g_blob[h];
    }
}

void h3d_blob_drop(uintptr_t addr) {   /* remove a decode: CALLs then run the words in place */
    pthread_mutex_lock(&g_mx);
    Blob *b = blob_find(addr);
    if (b) {
        /* open addressing: re-insert the rest of the cluster after removing */
        size_t cap = g_capblob, h = (size_t)(b - g_blob);
        pf(b->ops); b->ops = NULL; b->addr = 0; b->n = 0; g_nblob--;
        for (size_t j = (h + 1) & (cap - 1); g_blob[j].addr; j = (j + 1) & (cap - 1)) {
            Blob e = g_blob[j];
            g_blob[j].addr = 0;
            size_t k = bhash(e.addr, cap);
            while (g_blob[k].addr) k = (k + 1) & (cap - 1);
            g_blob[k] = e;
        }
    }
    pthread_mutex_unlock(&g_mx);
}
int h3d_blob_known(uintptr_t addr) {
    pthread_mutex_lock(&g_mx);
    int k = blob_find(addr) != NULL;
    pthread_mutex_unlock(&g_mx);
    return k;
}
void h3d_blob_set(uintptr_t addr, uint32_t bytes, const ROp *ops, int n) {
    if (!addr) return;
    h3d_init();
    pthread_mutex_lock(&g_mx);
    if ((g_nblob + 1) * 2 > g_capblob) blob_rehash(g_capblob ? g_capblob * 2 : 4096);
    Blob *b = blob_find(addr);
    if (!b) {
        size_t h = bhash(addr, g_capblob);
        while (g_blob[h].addr) h = (h + 1) & (g_capblob - 1);
        b = &g_blob[h];
        b->addr = addr;
        g_nblob++;
        g_st.blobs++;
    } else {
        pf(b->ops);
    }
    b->bytes = bytes;
    b->n = n;
    b->ops = n ? pc((size_t)n, sizeof *ops) : NULL;
    if (n) memcpy(b->ops, ops, sizeof *ops * (size_t)n);
    pthread_mutex_unlock(&g_mx);
}

/* the decoded blob whose words contain game address a (linear search: rare callers cache it) */
int h3d_blob_containing(uintptr_t a, uintptr_t *start, uint32_t *bytes) {
    int found = 0;
    pthread_mutex_lock(&g_mx);
    for (size_t i = 0; i < g_capblob; i++)
        if (g_blob[i].addr && a >= g_blob[i].addr && a < g_blob[i].addr + g_blob[i].bytes) {
            *start = g_blob[i].addr; *bytes = g_blob[i].bytes; found = 1; break;
        }
    pthread_mutex_unlock(&g_mx);
    return found;
}
/* replace the texture-unit op (H3D_TEXADDR) of one unit in a decoded blob; returns ops replaced */
int h3d_blob_set_texaddr(uintptr_t blob, const ROp *texop) {
    int n = 0;
    pthread_mutex_lock(&g_mx);
    Blob *b = blob_find(blob);
    for (int i = 0; b && i < b->n; i++)
        if (b->ops[i].type == ROP_EXT && b->ops[i].u.ext.kind == H3D_TEXADDR && b->ops[i].unit == texop->unit) {
            b->ops[i] = *texop;
            n++;
        }
    pthread_mutex_unlock(&g_mx);
    return n;
}

H3dVtx *h3d_vtx_new(void) {
    H3dVtx *v = pc(1, sizeof *v);
    pthread_mutex_lock(&g_mx);
    if (g_nvtx == g_capvtx) { g_capvtx = g_capvtx ? g_capvtx * 2 : 256; g_vtx = pr(g_vtx, g_capvtx * sizeof *g_vtx); }
    g_vtx[g_nvtx++] = v;
    pthread_mutex_unlock(&g_mx);
    return v;
}

void h3d_forget_range(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_mx);
    size_t keep = 0;
    for (size_t i = 0; i < g_capblob; i++)
        if (g_blob[i].addr >= lo && g_blob[i].addr < hi) { pf(g_blob[i].ops); g_blob[i].ops = NULL; g_blob[i].addr = 0; }
        else if (g_blob[i].addr) keep++;
    g_nblob = keep;
    if (g_capblob) blob_rehash(g_capblob);
    /* vertex layouts and GL buffers of that memory are re-uploaded on their next use (they may be
       referenced by pooled ops, so the objects themselves stay) */
    for (size_t i = 0; i < g_nvtx; i++)
        if (g_vtx[i]->addr >= lo && g_vtx[i]->addr < hi) g_vtx[i]->dirty = 1;
    for (size_t i = 0; i < g_nib; i++)
        if (g_ib[i]->addr >= lo && g_ib[i]->addr < hi) g_ib[i]->dirty = 1;
    pthread_mutex_unlock(&g_mx);
}

H3dStats h3d_stats(void) { return g_st; }

/* save states: the registry statics (the objects are in the fixed arena, saved by savestate.c) */
typedef struct {
    Blob *blob; size_t nblob, capblob;
    H3dVtx **vtx; size_t nvtx, capvtx;
    int mat_variant;
    H3dStats st;
} H3dSsState;
size_t h3d_ss_state(void *buf, size_t n, int restore) {
    if (!buf) return sizeof(H3dSsState);
    if (n != sizeof(H3dSsState)) return 0;
    H3dSsState *x = buf;
    pthread_mutex_lock(&g_mx);
    if (!restore) {
        *x = (H3dSsState){g_blob, g_nblob, g_capblob, g_vtx, g_nvtx, g_capvtx, g_mat_variant, g_st};
    } else {
        g_blob = x->blob; g_nblob = x->nblob; g_capblob = x->capblob;
        g_vtx = x->vtx; g_nvtx = x->nvtx; g_capvtx = x->capvtx;
        g_mat_variant = x->mat_variant;
        g_st = x->st;
        g_cur = NULL;
        for (size_t i = 0; i < g_nvtx; i++) {   /* GL names of the saving run: made again on use */
            g_vtx[i]->vbo = g_vtx[i]->vao = 0;
            g_vtx[i]->verts = 0;
            g_vtx[i]->dirty = 1;
        }
        g_nib = 0;                                /* index buffers: GL caches, rebuilt on use */
        if (g_ibh) memset(g_ibh, 0, g_capibh * sizeof *g_ibh);
    }
    pthread_mutex_unlock(&g_mx);
    return sizeof(H3dSsState);
}

/* ================================================================== GL side */

#define GL_TRIANGLE_FAN_ 0x0006
#define GL_BYTE_ 0x1400
#define GL_SHORT_ 0x1402
static void (*pVertexAttrib4f)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (*pDisableVertexAttribArray)(GLuint);

static int gl_ok(void) {
    if (pVertexAttrib4f) return 1;
    pVertexAttrib4f = (void (*)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat))SDL_GL_GetProcAddress("glVertexAttrib4f");
    pDisableVertexAttribArray = (void (*)(GLuint))SDL_GL_GetProcAddress("glDisableVertexAttribArray");
    return pVertexAttrib4f && pDisableVertexAttribArray;
}

static const GLenum ATYPE[4] = {GL_BYTE_, GL_UNSIGNED_BYTE, GL_SHORT_, GL_FLOAT};
static const GLenum PRIM[3] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN_};

static size_t ihash(uintptr_t a, uint32_t n, size_t cap) { return (size_t)(((a >> 1) ^ n * 0x9E3779B1u) * 2654435761u) & (cap - 1); }

static void ib_index(Ibuf *b) {
    size_t h = ihash(b->addr, b->count, g_capibh);
    while (g_ibh[h]) h = (h + 1) & (g_capibh - 1);
    g_ibh[h] = b;
}

static Ibuf *ibuf_get(uintptr_t addr, uint32_t count, int u16) {
    if (g_capibh)
        for (size_t h = ihash(addr, count, g_capibh); g_ibh[h]; h = (h + 1) & (g_capibh - 1))
            if (g_ibh[h]->addr == addr && g_ibh[h]->count == count && g_ibh[h]->u16 == u16) return g_ibh[h];
    if (g_nib == g_capib) { g_capib = g_capib ? g_capib * 2 : 1024; g_ib = xrealloc(g_ib, g_capib * sizeof *g_ib); }
    Ibuf *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->addr = addr; b->count = count; b->u16 = (uint8_t)u16; b->dirty = 1;
    g_ib[g_nib++] = b;
    if (g_nib * 2 > g_capibh) {
        free(g_ibh);
        g_capibh = g_capibh ? g_capibh * 2 : 2048;
        g_ibh = calloc(g_capibh, sizeof *g_ibh);
        if (!g_ibh) abort();
        for (size_t i = 0; i < g_nib; i++) ib_index(g_ib[i]);
    } else {
        ib_index(b);
    }
    return b;
}

static void ibuf_upload(Ibuf *b) {
    const uint8_t *src = (const uint8_t *)b->addr;
    uint32_t mx = 0;
    for (uint32_t i = 0; i < b->count; i++) {
        uint32_t v = b->u16 ? (uint32_t)(src[2 * i] | src[2 * i + 1] << 8) : src[i];
        if (v > mx) mx = v;
    }
    b->max_index = mx;
    if (!b->ibo) glGenBuffers(1, &b->ibo);
    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->count << b->u16, src, GL_STATIC_DRAW);
    b->dirty = 0;
    g_st.ibo_uploads++;
}

static void vtx_upload(H3dVtx *v, uint32_t verts) {
    if (!v->vao) glGenVertexArrays(1, &v->vao);
    if (!v->vbo) glGenBuffers(1, &v->vbo);
    glBindVertexArray(v->vao);
    glBindBuffer(GL_ARRAY_BUFFER, v->vbo);
    /* the game's vertex buffers, one after the other in the GL buffer (usually just one) */
    int nb = h3d_vtx_ext(v) && v->nbuffers > 1 ? (v->nbuffers < H3D_MAX_VBUF ? v->nbuffers : H3D_MAX_VBUF) : 1;
    int dyn = h3d_vtx_ext(v) && v->dynamic;
    size_t off[H3D_MAX_VBUF] = {0}, total = 0;
    for (int k = 0; k < nb; k++) {
        uint32_t st = nb > 1 ? v->bstride[k] : v->stride;
        off[k] = total;
        total += (size_t)verts * st;
    }
    if (nb == 1) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)total, (const void *)(uintptr_t)v->addr,
                     dyn ? GL_STREAM_DRAW : GL_STATIC_DRAW);
    } else {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)total, NULL, dyn ? GL_STREAM_DRAW : GL_STATIC_DRAW);
        for (int k = 0; k < nb; k++)
            if (v->baddr[k] && v->bstride[k])
                glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)off[k], (GLsizeiptr)verts * v->bstride[k],
                                (const void *)(uintptr_t)v->baddr[k]);
    }
    for (GLuint r = 0; r < 12; r++) pDisableVertexAttribArray(r);
    for (int i = 0; i < v->nattr; i++) {
        const H3dAttr *a = &v->attr[i];
        if (a->fixed || a->reg >= 12) continue;
        int k = nb > 1 && a->buf < nb ? a->buf : 0;
        uint32_t st = nb > 1 ? v->bstride[k] : v->stride;
        /* PICA attributes are converted to float without normalisation; the program applies the
           IrScale factors (c7 / c8) itself */
        glVertexAttribPointer(a->reg, a->comps, ATYPE[a->type & 3], GL_FALSE, st,
                              (const void *)(uintptr_t)(off[k] + a->offset));
        glEnableVertexAttribArray(a->reg);
    }
    glBindVertexArray(0);
    v->verts = verts;
    v->dirty = 0;
    g_st.vbo_uploads++;
}

/* Index buffer address of a draw: the PICA index word (0x227) holds a 28-bit offset from the
 * attribute base (0x200, here the VRAM start 0x1F000000) and the 16-bit flag in bit 31. On the 3DS
 * every buffer lies within 256 MB of that base; in the port the game heap can be anywhere in the
 * 32-bit space (0xD... on i386), so the relocated offset's top bits do not fit and only its low
 * 28 bits are kept. The full address is the one with those low 28 bits that is nearest the
 * mesh's vertex buffer (same file, same heap block; its address register 0x203 is a full word).
 * Pure address arithmetic, decided once per draw op. */
void albw_gfx_vram(uint8_t **base, size_t *size);   /* native/sdk/gfx.c */
static int index_region_ok(uint32_t a) {
    /* VRAM (0x1F000000 on the PC, a block in libmain.so on Android), or the fixed game memory areas
     * (runtime/pmem.c: heap, device, arena 0x40000000..) */
    static uint32_t vlo, vhi;
    if (!vhi) { uint8_t *b; size_t n; albw_gfx_vram(&b, &n); vlo = (uint32_t)(uintptr_t)b; vhi = vlo + (uint32_t)n; }
    return (a >= vlo && a < vhi) || (a >= 0x40000000u && a < 0x58000000u);
}
static uintptr_t index_addr(const H3dVtx *v, uint32_t word) {
    /* the game memory areas now sit at fixed addresses below 0x5C000000, so the whole 31-bit
     * offset is right: vertices copied to VRAM, indices left in device memory (Hyrule Castle
     * entrance: base 0x1F000000 + 0x268C433C = 0x458C433C; the 28-bit form gave 0x258C433C,
     * unmapped -> crash) */
    uint32_t full = v->base + (word & 0x7FFFFFFFu);
    if (index_region_ok(full)) {
        static int tr = -1; static unsigned n;
        if (tr < 0) tr = getenv("ALBW_IDX_TRACE") != NULL;
        if (tr) {
            uint32_t lo = (v->base + (word & 0x0FFFFFFFu)) & 0x0FFFFFFFu, b = (v->addr & 0xF0000000u) | lo;
            for (int k = -1; k <= 1; k += 2) {
                uint32_t c = (uint32_t)((v->addr & 0xF0000000u) + (uint32_t)k * 0x10000000u) | lo;
                if (llabs((int64_t)c - (int64_t)v->addr) < llabs((int64_t)b - (int64_t)v->addr)) b = c;
            }
            if (b != full && n++ < 20) fprintf(stderr, "idx-diff: full %#x old %#x vtx %#x\n", full, b, (unsigned)v->addr);
        }
        return (uintptr_t)full;
    }
    uint32_t low = (v->base + (word & 0x0FFFFFFFu)) & 0x0FFFFFFFu;
    uint32_t best = (v->addr & 0xF0000000u) | low;
    int64_t bd = (int64_t)best - (int64_t)v->addr;
    for (int k = -1; k <= 1; k += 2) {
        uint32_t c = (uint32_t)((v->addr & 0xF0000000u) + (uint32_t)k * 0x10000000u) | low;
        int64_t d = (int64_t)c - (int64_t)v->addr;
        if ((d < 0 ? -d : d) < (bd < 0 ? -bd : bd)) { best = c; bd = d; }
    }
    return (uintptr_t)best;
}

/* ALBW_H3D_DUMP=<n>: log the first n draws of frame ALBW_H3D_DUMP_AT (default: any) with the
 * uniforms that place them and their first vertices (debug only) */
static void dump_draw(const H3dVtx *v, const Ibuf *ib, const ROp *op, const uint8_t *idx) {
    static long left = -2;
    if (left == -2) { const char *e = getenv("ALBW_H3D_DUMP"); left = e ? atol(e) : -1; }
    if (left <= 0) return;
    { /* ALBW_H3D_DUMP_FROM=<presented frame>: only dump from that frame on */
        static long from = -2;
        extern long g_plat_frames;
        if (from == -2) { const char *e = getenv("ALBW_H3D_DUMP_FROM"); from = e ? atol(e) : -1; }
        if (from > 0 && g_plat_frames < from) return;
    }
    uint16_t b;
    const float *c = render_vs_floats(&b);
    int var = 0, prog = render_current_program(&var);
    static int only = -2;
    if (only == -2) { const char *e = getenv("ALBW_H3D_DUMP_VAR"); only = e ? atoi(e) : -1; }
    if (only >= 0 && var != only) return;
    static long only_tex = -2; /* ALBW_H3D_DUMP_TEX=<hex>: only draws sampling that texture on unit 0 */
    if (only_tex == -2) { const char *e = getenv("ALBW_H3D_DUMP_TEX"); only_tex = e ? (long)strtoul(e, NULL, 16) : -1; }
    if (only_tex >= 0 && render_texunit(0)->addr != (uintptr_t)only_tex) return;
    left--;
    { /* ALBW_H3D_DUMP_BIN=<dir>: raw vertices / indices / uniforms of the dumped draw */
        const char *dir = getenv("ALBW_H3D_DUMP_BIN");
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/draw%ld.bin", dir, left);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint32_t hdr[8] = {op->u.ext.c, op->u.ext.b, (uint32_t)ib->u16, ib->max_index, v->stride, b, (uint32_t)var,
                                   (uint32_t)v->nattr};
                fwrite(hdr, 4, 8, f);
                for (int i = 0; i < v->nattr; i++) {
                    uint32_t a[4] = {v->attr[i].reg, v->attr[i].type | (uint32_t)v->attr[i].fixed << 8, v->attr[i].comps,
                                     v->attr[i].offset};
                    fwrite(a, 4, 4, f);
                }
                fwrite(c, 4, 96 * 4, f);
                fwrite(idx, ib->u16 ? 2 : 1, op->u.ext.b, f);
                fwrite((const void *)(uintptr_t)v->addr, v->stride, ib->max_index + 1, f);
                fclose(f);
                fprintf(stderr, "h3d-dump: wrote %s\n", path);
            }
        }
    }
    fprintf(stderr, "h3d-dump: draw prim %u count %u u16 %d idx %p max %u vtx %#x stride %u base %#x prog %d/%d b %#x\n",
            op->u.ext.c, op->u.ext.b, ib->u16, (const void *)idx, ib->max_index, v->addr, v->stride, v->base, prog, var, b);
    for (int i = 0; i < v->nattr; i++)
        fprintf(stderr, "h3d-dump:   attr %d reg v%u type %u comps %u fixed %u off %u val %g %g %g %g\n", i, v->attr[i].reg,
                v->attr[i].type, v->attr[i].comps, v->attr[i].fixed, v->attr[i].offset, v->attr[i].value[0],
                v->attr[i].value[1], v->attr[i].value[2], v->attr[i].value[3]);
    const RTev *t = render_tev_state();
    for (int k = 0; k < 6; k++) {
        const RTevStage *g = &t->stage[k];
        fprintf(stderr, "h3d-dump:   tev%d rgb src %u %u %u op %u %u %u cmb %u x%u | a src %u %u %u op %u %u %u cmb %u x%u | k %u %u %u %u\n",
                k, g->src_rgb[0], g->src_rgb[1], g->src_rgb[2], g->op_rgb[0], g->op_rgb[1], g->op_rgb[2], g->combine_rgb,
                g->scale_rgb, g->src_a[0], g->src_a[1], g->src_a[2], g->op_a[0], g->op_a[1], g->op_a[2], g->combine_a,
                g->scale_a, g->konst[0], g->konst[1], g->konst[2], g->konst[3]);
    }
    fprintf(stderr, "h3d-dump:   tevbuf upd %#x/%#x col %u %u %u %u\n", t->buf_update_rgb, t->buf_update_a,
            t->buf_color[0], t->buf_color[1], t->buf_color[2], t->buf_color[3]);
    for (int u = 0; u < 3; u++) {
        const RTexUnit *x = render_texunit(u);
        fprintf(stderr, "h3d-dump:   tex%d addr %#lx %ux%u fmt %u type %u\n", u, (unsigned long)x->addr, x->w, x->h, x->fmt, x->type);
    }
    const FLEnv *fe = fraglight_env();
    fprintf(stderr, "h3d-dump:   fraglight enable %u n %u amb %u %u %u config %u lut_off %#x\n", fe->enable, fe->nlights,
            fe->ambient[0], fe->ambient[1], fe->ambient[2], fe->config, fe->lut_off);
    fraglight_debug_print(stderr);
    if (getenv("ALBW_H3D_DUMP_ALL")) {
        for (int r = 0; r < 96; r++)
            fprintf(stderr, "h3d-dump:   C%d = %g %g %g %g\n", r, c[4 * r], c[4 * r + 1], c[4 * r + 2], c[4 * r + 3]);
        const RFragState *f = render_frag_state();
        fprintf(stderr, "h3d-dump:   frag blend %u logic %u/%u atest %u/%u/%u depth %u/%u/%u cmask %x stencil %u cull %u range %g %g poly %u %g\n",
                f->blend_enable, f->logic_enable, f->logic_op, f->alpha_test, f->alpha_func, f->alpha_ref, f->depth_test,
                f->depth_func, f->depth_write, f->color_mask, f->stencil_test, f->cull, f->depth_near, f->depth_far,
                f->poly_offset_enable, f->poly_offset_units);
    }
    static const int regs[] = {3, 6, 7, 8, 10, 11, 20, 21, 25, 26, 27, 86, 87, 88, 89, 90, 91, 92};
    for (unsigned k = 0; k < sizeof regs / sizeof *regs; k++) {
        const float *r = c + 4 * regs[k];
        fprintf(stderr, "h3d-dump:   c%d = %g %g %g %g\n", regs[k], r[0], r[1], r[2], r[3]);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t i = ib->u16 ? (uint32_t)(idx[2 * k] | idx[2 * k + 1] << 8) : idx[k];
        const uint8_t *vp = (const uint8_t *)(uintptr_t)v->addr + (size_t)i * v->stride;
        for (int a = 0; a < v->nattr; a++) {
            const H3dAttr *at = &v->attr[a];
            if (at->fixed) continue;
            float f[4] = {0};
            for (int j = 0; j < at->comps; j++) {
                const uint8_t *q = vp + at->offset;
                switch (at->type & 3) {
                case 0: f[j] = (float)((const int8_t *)q)[j]; break;
                case 1: f[j] = (float)q[j]; break;
                case 2: { int16_t t; memcpy(&t, q + 2 * j, 2); f[j] = t; break; }
                default: memcpy(&f[j], q + 4 * j, 4); break;
                }
            }
            fprintf(stderr, "h3d-dump:   vert %u v%u = %g %g %g %g\n", i, at->reg, f[0], f[1], f[2], f[3]);
        }
    }
}

/* ALBW_H3D_SNAP=<k>[,<k>...]: after the k-th h3d draw, write the colour target (the shared render
 * buffer 0x1F000080) as ALBW_SHOT_DIR/h3d_<k>.ppm, i.e. what the 3D pass drew before any transfer
 * or later pass (debug only) */
static void snap_maybe_write(const char *tag, int var) {
    int w = 0, h = 0;
    if (!render_read_surface(0x1F000080u, NULL, &w, &h) || w <= 0 || h <= 0) return;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px || !render_read_surface(0x1F000080u, px, &w, &h)) { free(px); return; }
    const char *dir = getenv("ALBW_SHOT_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/h3dv_%s_v%d.ppm", dir ? dir : ".", tag, var);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(px);
}

static void snap_maybe(void) {
    static const char *spec;
    static int init;
    if (!init) { init = 1; spec = getenv("ALBW_H3D_SNAP"); }
    static long from = -2;
    static int last_var = -1, nsaved;
    if (from == -2) { const char *e = getenv("ALBW_H3D_SNAPFROM"); from = e ? atol(e) : -1; }
    static int every = -1;
    if (every < 0) every = getenv("ALBW_H3D_SNAPEVERY") != NULL;
    if (from >= 0 && (long)g_st.draws >= from && nsaved < (every ? 200 : 24)) { /* save at every variant change */
        int wv = 0;
        render_current_program(&wv);
        if (wv != last_var || every) {
            last_var = wv;
            nsaved++;
            char buf[32];
            snprintf(buf, sizeof buf, "%u", g_st.draws);
            static char spec2[64];
            snprintf(spec2, sizeof spec2, "%s", buf);
            const char *keep = spec;
            spec = spec2;
            snap_maybe_write(buf, wv);
            spec = keep;
        }
    }
    if (!spec) return;
    char want[32];
    snprintf(want, sizeof want, "%u", g_st.draws);
    const char *q = spec;
    int hit = 0;
    size_t n = strlen(want);
    while (*q) {
        if (!strncmp(q, want, n) && (q[n] == ',' || q[n] == 0)) { hit = 1; break; }
        while (*q && *q != ',') q++;
        if (*q) q++;
    }
    if (!hit) return;
    int w = 0, h = 0;
    if (!render_read_surface(0x1F000080u, NULL, &w, &h) || w <= 0 || h <= 0) return;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px || !render_read_surface(0x1F000080u, px, &w, &h)) { free(px); return; }
    const char *dir = getenv("ALBW_SHOT_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/h3d_%s.ppm", dir ? dir : ".", want);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
        fprintf(stderr, "h3d: wrote %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

static void draw(const ROp *op) {
    { /* debug (ALBW_H3D_VTXTRACE): draws whose layout has a colour attribute but colour scale c7.w = 0 */
        static int vt = -1; static unsigned with_col, zero_scale, total;
        if (vt < 0) vt = getenv("ALBW_H3D_VTXTRACE") != NULL;
        { /* ALBW_VTX_FOR_TEX=<hex>: layout + bools of the draws sampling that texture on unit 0 */
            static long want = -2;
            if (want == -2) { const char *e = getenv("ALBW_VTX_FOR_TEX"); want = e ? (long)strtoul(e, NULL, 16) : -1; }
            const RTexUnit *u0 = render_texunit(0);
            static unsigned shown;
            if (want >= 0 && g_cur && u0->addr == (uintptr_t)want && shown++ < 8) {
                uint16_t bools = 0; render_vs_floats(&bools);
                char line[512]; int o = 0;
                for (int i = 0; i < g_cur->nattr && i < 12; i++) {
                    const H3dAttr *a = &g_cur->attr[i];
                    o += snprintf(line + o, sizeof line - (size_t)o, a->fixed ? " v%u=const" : " v%u:t%u x%u@%u", a->reg, a->type, a->comps, a->offset);
                }
                fprintf(stderr, "vtx-for-tex: draw count %u stride %u bools %#x prog variant %d:%s\n", (unsigned)op->u.ext.b, g_cur->stride, bools,
                        g_mat_variant, line);
            }
        }
        if (vt && g_cur) {
            int col = 0;
            for (int i = 0; i < g_cur->nattr && i < 12; i++) if (g_cur->attr[i].reg == 3 && !g_cur->attr[i].fixed) col = 1;
            const float *c = render_vs_floats(NULL);
            total++;
            if (col) { with_col++; if (c[7 * 4 + 3] == 0.0f) zero_scale++; }
            if (total % 20000 == 0)
                fprintf(stderr, "h3d-vtx: draws %u, with a colour attribute %u, of those colour scale 0: %u\n", total, with_col, zero_scale);
        }
    }
    {   /* debug: ALBW_SKIP_TEX=<hex>: drop draws sampling that texture on unit 0 (find a draw on screen) */
        static long skip = -2;
        if (skip == -2) { const char *e = getenv("ALBW_SKIP_TEX"); skip = e ? (long)strtoul(e, NULL, 16) : -1; }
        if (skip >= 0 && render_texunit(0)->addr == (uintptr_t)skip) return;
        static int skeft = -1;   /* ALBW_SKIP_EFT=1: drop the draws of live (nw::eft) blocks */
        if (skeft < 0) skeft = getenv("ALBW_SKIP_EFT") != NULL;
        if (skeft && (op->flags & 2)) return;
    }
    H3dVtx *v = g_cur;
    if (!v || !v->addr || !v->stride || !op->u.ext.b) {
        if (getenv("ALBW_H3D_DROPTRACE")) fprintf(stderr, "h3d-drop: no vertex buffer (v %p addr %#lx stride %u count %u)\n", (void *)v, v ? (unsigned long)v->addr : 0ul, v ? (unsigned)v->stride : 0u, (unsigned)op->u.ext.b);
        g_st.dropped++; return; }
    uintptr_t ia = op->flags & 1 ? (uintptr_t)op->u.ext.a : index_addr(v, op->u.ext.a);
    int u16 = op->flags & 1 ? op->unit : (int)(op->u.ext.a >> 31);
    /* an index buffer outside VRAM and the game memory areas is a bad address: drop the draw,
     * never read it (was a SIGSEGV in ibuf_upload, Hyrule Castle entrance) */
    if (!(op->flags & 1)) {
        if (!index_region_ok((uint32_t)ia)) {
            static unsigned told;
            if (told++ < 5)
                fprintf(stderr, "h3d: draw dropped: index buffer %#lx outside game memory (vertices %#lx, index word %#x)\n",
                        (unsigned long)ia, (unsigned long)v->addr, (unsigned)op->u.ext.a);
            g_st.dropped++;
            return;
        }
    }
    pthread_mutex_lock(&g_mx);
    Ibuf *ib = ibuf_get(ia, op->u.ext.b, u16);
    int dyn = h3d_vtx_ext(v) && v->dynamic;   /* the CPU rewrites its buffers every frame */
    if (ib->dirty || dyn) ibuf_upload(ib);
    if (v->dirty || dyn || v->verts < ib->max_index + 1)
        vtx_upload(v, v->verts > ib->max_index + 1 && !v->dirty && !dyn ? v->verts : ib->max_index + 1);
    /* the model's own program: the material's DefaultShader variant, unless a shader bind of an
       h3d program (ROP_PROGRAM with a registered DVLE) selected one since */
    /* flag 2: a draw of a live-translated block (nw::eft, sdk/h3d.c h3d_translate_live) keeps the
       program its block bound (RPROG_EFT) */
    if (!(op->flags & 2) && render_current_program(NULL) != RPROG_H3D_DEFAULT)
        render_set_program_variant(RPROG_H3D_DEFAULT, g_mat_variant);
    static int noat = -2;
    if (noat == -2) { const char *e = getenv("ALBW_H3D_NOATEST"); noat = e ? atoi(e) : -1; }
    if (noat >= 0) { /* debug: alpha test off for one variant */
        int wv = 0;
        int pr = render_current_program(&wv);
        if (pr == RPROG_H3D_DEFAULT && wv == noat) { render_frag_state()->alpha_test = 0; render_frag_dirty(); }
        (void)pr;
    }
    if (!render_draw_bind()) {
        if (getenv("ALBW_H3D_DROPTRACE")) fprintf(stderr, "h3d-drop: render_draw_bind failed\n");
        pthread_mutex_unlock(&g_mx); g_st.dropped++; return; }
    glBindVertexArray(v->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->ibo);
    /* constant attributes, and (0, 0, 0, 1) for inputs the mesh does not provide */
    uint16_t have = 0;
    for (int i = 0; i < v->nattr; i++)
        if (!v->attr[i].fixed && v->attr[i].reg < 12) have |= (uint16_t)(1u << v->attr[i].reg);
    for (GLuint r = 0; r < 12; r++)
        if (!(have >> r & 1)) pVertexAttrib4f(r, 0, 0, 0, 1);
    for (int i = 0; i < v->nattr; i++) {
        const H3dAttr *a = &v->attr[i];
        if (a->fixed && a->reg < 12) pVertexAttrib4f(a->reg, a->value[0], a->value[1], a->value[2], a->value[3]);
    }
    dump_draw(v, ib, op, (const uint8_t *)ia);
    static int white = -1, white_var = -1;
    int white_now = 1;
    if (white < 0) {
        const char *e = getenv("ALBW_H3D_WHITE");
        white = e != NULL;
        if (e && strcmp(e, "1")) white_var = atoi(e); /* =<variant>: only that variant, white */
    }
    if (white && white_var >= 0) {
        int wv = 0;
        render_current_program(&wv);
        if (wv != white_var) {
            if (!getenv("ALBW_H3D_WHITE_KEEP")) { render_draw_done(); pthread_mutex_unlock(&g_mx); return; }
            white_now = 0;
        }
    }
    static int skipv = -2;
    if (skipv == -2) { const char *e = getenv("ALBW_H3D_SKIPVAR"); skipv = e ? atoi(e) : -1; }
    if (skipv >= 0) {
        int wv = 0;
        render_current_program(&wv);
        int sk = wv == skipv;
        const RFragState *ff = render_frag_state();
        const char *only = getenv("ALBW_H3D_SKIPKIND"); /* dome: depth always+write; stencil: stencil test; fade: blend 6/7 no depth */
        if (sk && only) {
            if (!strcmp(only, "dome")) sk = ff->depth_test && ff->depth_func == 1 && ff->depth_write;
            else if (!strcmp(only, "stencil")) sk = ff->stencil_test && !(ff->depth_test && ff->depth_func == 1);
            else if (!strcmp(only, "fade")) sk = !ff->depth_test && !ff->stencil_test;
        }
        if (sk) { render_draw_done(); pthread_mutex_unlock(&g_mx); return; }
    }
    static int nod = -2, noa = -2;
    if (nod == -2) { const char *e = getenv("ALBW_H3D_NODEPTH"); nod = e ? atoi(e) : -1; }
    if (noa == -2) { const char *e = getenv("ALBW_H3D_NOBLENDVAR"); noa = e ? atoi(e) : -1; }
    {
        static int nc = -2;
        if (nc == -2) { const char *e = getenv("ALBW_H3D_NOCULL"); nc = e && strcmp(e, "1") ? atoi(e) : -1; }
        if (nc >= 0) {
            int wv = 0;
            render_current_program(&wv);
            if (wv == nc) { glDisable(GL_CULL_FACE); render_frag_dirty(); }
        }
    }
    {
        static int dv = -2; static unsigned df;
        if (dv == -2) { const char *e = getenv("ALBW_H3D_DFUNC_VAR"); dv = e ? atoi(e) : -1; const char *f = getenv("ALBW_H3D_DFUNC"); df = f ? (unsigned)strtoul(f, NULL, 0) : 0x0204; }
        if (dv >= 0) {
            int wv = 0;
            render_current_program(&wv);
            if (wv == dv) {
                static void (*dfn)(GLenum);
                if (!dfn) dfn = (void (*)(GLenum))SDL_GL_GetProcAddress("glDepthFunc");
                dfn(df); render_frag_dirty();
            }
        }
    }
    if (nod >= 0 || noa >= 0) { /* debug: no depth test / no blending for one variant */
        int wv = 0;
        render_current_program(&wv);
        if (wv == nod) glDisable(GL_DEPTH_TEST);
        if (wv == noa) glDisable(GL_BLEND);
        if (wv == nod && getenv("ALBW_H3D_NOCULL") && !strcmp(getenv("ALBW_H3D_NOCULL"), "1")) glDisable(GL_CULL_FACE);
        if (wv == nod && getenv("ALBW_H3D_NOSTENCIL")) glDisable(0x0B90 /* GL_STENCIL_TEST */);
        if (wv == nod && getenv("ALBW_H3D_NOLOGIC")) glDisable(0x0BF2);
        if (wv == nod && getenv("ALBW_H3D_FULLMASK")) {
            static void (*cm)(GLboolean, GLboolean, GLboolean, GLboolean);
            if (!cm) cm = (void (*)(GLboolean, GLboolean, GLboolean, GLboolean))SDL_GL_GetProcAddress("glColorMask");
            if (cm) cm(1, 1, 1, 1);
        }
    }
    if (white && white_now) { /* debug: geometry coverage only (logic op SET, no depth test / culling) */
        static void (*logic)(GLenum);
        if (!logic) logic = (void (*)(GLenum))SDL_GL_GetProcAddress("glLogicOp");
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glEnable(0x0BF2 /* GL_COLOR_LOGIC_OP */); if (logic) logic(getenv("ALBW_H3D_LOGICCOPY") ? 0x1503 /* GL_COPY */ : 0x150F /* GL_SET */);
    }
    static int qvar = -2;
    static unsigned qn;
    static GLuint qid;
    static void (*pGenQ)(GLsizei, GLuint *), (*pBeginQ)(GLenum, GLuint), (*pEndQ)(GLenum), (*pGetQ)(GLuint, GLenum, GLuint *);
    int qon = 0;
    const uint8_t *q_before = NULL;
    if (qvar == -2) { const char *e = getenv("ALBW_H3D_QUERY"); qvar = e ? atoi(e) : -1; }
    if (qvar >= 0 && qn < 4000) {
        int wv = 0;
        render_current_program(&wv);
        if (wv == qvar) {
            if (!pGenQ) {
                pGenQ = (void (*)(GLsizei, GLuint *))SDL_GL_GetProcAddress("glGenQueries");
                pBeginQ = (void (*)(GLenum, GLuint))SDL_GL_GetProcAddress("glBeginQuery");
                pEndQ = (void (*)(GLenum))SDL_GL_GetProcAddress("glEndQuery");
                pGetQ = (void (*)(GLuint, GLenum, GLuint *))SDL_GL_GetProcAddress("glGetQueryObjectuiv");
                pGenQ(1, &qid);
            }
            pBeginQ(0x8914 /* GL_SAMPLES_PASSED */, qid);
            qon = 1;
            static void (*rp)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
            if (!rp) rp = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))SDL_GL_GetProcAddress("glReadPixels");
            static uint8_t before[240 * 400 * 4];
            rp(0, 0, 240, 400, 0x1908, 0x1401, before);
            q_before = before;
        }
    }
    { extern unsigned long g_render_draws; g_render_draws++; }
    glDrawElements(PRIM[op->u.ext.c % 3], (GLsizei)op->u.ext.b, u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, 0);
    if (qon) {
        pEndQ(0x8914);
        GLuint r = 0;
        pGetQ(qid, 0x8866 /* GL_QUERY_RESULT */, &r);
        uint16_t bb;
        const float *cc = render_vs_floats(&bb);
        const RFragState *ff = render_frag_state();
        fprintf(stderr, "h3d-query: draw %u count %u samples %u atest %u/%u/%u depth %u/%u stencil %u cull %u blend %u logic %u cmask %x\n",
                g_st.draws, op->u.ext.b, r, ff->alpha_test, ff->alpha_func, ff->alpha_ref, ff->depth_test, ff->depth_func,
                ff->stencil_test, ff->cull, ff->blend_enable, ff->logic_enable, ff->color_mask);
        (void)cc;
        {
            static void (*rpd)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
            if (!rpd) rpd = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))SDL_GL_GetProcAddress("glReadPixels");
            float dz[5];
            static const int px[5][2] = {{120, 200}, {60, 100}, {180, 300}, {60, 300}, {180, 100}};
            for (int k = 0; k < 5; k++) rpd(px[k][0], px[k][1], 1, 1, 0x1902 /* DEPTH_COMPONENT */, 0x1406, &dz[k]);
            fprintf(stderr, "h3d-query:   depth %.6f %.6f %.6f %.6f %.6f\n", dz[0], dz[1], dz[2], dz[3], dz[4]);
        }
        if (q_before) {
            const uint8_t *before = q_before;
            static void (*rp)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
            if (!rp) rp = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))SDL_GL_GetProcAddress("glReadPixels");
            static uint8_t after[240 * 400 * 4];
            rp(0, 0, 240, 400, 0x1908, 0x1401, after);
            unsigned ch = 0, fx = 0, fy = 0;
            for (unsigned i = 0; i < 240 * 400; i++)
                if (memcmp(before + 4 * i, after + 4 * i, 4)) { if (!ch) { fx = i % 240; fy = i / 240; } ch++; }
            float fd = -1;
            if (ch) {
                static void (*rpd2)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
                if (!rpd2) rpd2 = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))SDL_GL_GetProcAddress("glReadPixels");
                rpd2((GLint)fx, (GLint)fy, 1, 1, 0x1902, 0x1406, &fd);
            }
            fprintf(stderr, "h3d-query:   depth-at-first %.6f\n", fd);
            fprintf(stderr, "h3d-query:   readback changed %u first (%u,%u) before %u %u %u %u after %u %u %u %u\n", ch, fx, fy,
                    before[4 * (fy * 240 + fx)], before[4 * (fy * 240 + fx) + 1], before[4 * (fy * 240 + fx) + 2], before[4 * (fy * 240 + fx) + 3],
                    after[4 * (fy * 240 + fx)], after[4 * (fy * 240 + fx) + 1], after[4 * (fy * 240 + fx) + 2], after[4 * (fy * 240 + fx) + 3]);
        }
        {
            static void (*gb)(GLenum, GLboolean *), (*gi)(GLenum, GLint *);
            if (!gb) { gb = (void (*)(GLenum, GLboolean *))SDL_GL_GetProcAddress("glGetBooleanv"); gi = (void (*)(GLenum, GLint *))SDL_GL_GetProcAddress("glGetIntegerv"); }
            GLboolean m[4] = {0}; GLint fbo = 0, bl = 0, bs = 0, bd = 0, lo = 0, dt = 0, st = 0, prog = 0, db = 0;
            gb(0x0C23 /* COLOR_WRITEMASK */, m); gi(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fbo);
            gi(0x0BE2, &bl); gi(0x80C9 /* BLEND_SRC_RGB */, &bs); gi(0x80C8 /* BLEND_DST_RGB */, &bd); gi(0x0BF2, &lo);
            gi(0x0B71, &dt); gi(0x0B90, &st); gi(0x8B8D /* CURRENT_PROGRAM */, &prog); gi(0x0C01 /* DRAW_BUFFER */, &db);
            fprintf(stderr, "h3d-query:   gl mask %d%d%d%d fbo %d blend %d %x/%x logic %d depth %d stencil %d prog %d drawbuf %x\n",
                    m[0], m[1], m[2], m[3], fbo, bl, bs, bd, lo, dt, st, prog, db);
        }
        qn++;
    }
    if (white && white_now) glDisable(0x0BF2);
    if (white || nod >= 0 || noa >= 0) render_frag_dirty();
    render_draw_done();
    pthread_mutex_unlock(&g_mx);
    g_st.draws++;
    snap_maybe();
}

static void ext_op(const ROp *op) {
    if (!gl_ok()) return;
    const uint32_t a = op->u.ext.a, b = op->u.ext.b, c = op->u.ext.c;
    switch (op->u.ext.kind) {
    case H3D_VTX:
        g_cur = (H3dVtx *)op->u.ext.ptr;
        { /* debug: ALBW_H3D_VTXTRACE=1: each vertex layout once (buffers, attributes, constants) */
            static int vt = -1;
            static const void *seen[4096]; static unsigned nseen;
            if (vt < 0) vt = getenv("ALBW_H3D_VTXTRACE") != NULL;
            int known = 0;
            for (unsigned i = 0; vt && i < nseen; i++) if (seen[i] == g_cur) { known = 1; break; }
            if (vt && !known && g_cur && nseen < 4096) {
                seen[nseen++] = g_cur;
                char line[512]; int o = 0;
                for (int i = 0; i < g_cur->nattr && i < 12; i++) {
                    const H3dAttr *a = &g_cur->attr[i];
                    if (a->fixed)
                        o += snprintf(line + o, sizeof line - (size_t)o, " v%u=const(%g %g %g %g)", a->reg, (double)a->value[0],
                                      (double)a->value[1], (double)a->value[2], (double)a->value[3]);
                    else
                        o += snprintf(line + o, sizeof line - (size_t)o, " v%u:t%u x%u@%u", a->reg, a->type, a->comps, a->offset);
                }
                fprintf(stderr, "h3d-vtx: %p buffers %u stride %u attrs %u:%s\n", (void *)g_cur, g_cur->nbuffers, g_cur->stride, g_cur->nattr, line);
            }
        }
        break;
    case H3D_DRAW: draw(op); break;
    case H3D_TEXADDR: {
        RTexUnit *u = render_texunit(op->unit);
        u->addr = (uintptr_t)a;
        u->w = (uint16_t)(b & 0xFFFF);
        u->h = (uint16_t)(b >> 16);
        u->fmt = (uint8_t)(c & 0xFF);
        u->max_level = (uint8_t)((c >> 8) & 0xF);
        u->min_level = (uint8_t)((c >> 12) & 0xF);
        break;
    }
    case H3D_TEXPARAM: {
        RTexUnit *u = render_texunit(op->unit);
        u->wrap_s = a & 3;
        u->wrap_t = (a >> 4) & 3;
        u->mag = (a >> 8) & 1;
        u->min = (a >> 12) & 7;
        u->type = (a >> 16) & 7;
        memcpy(u->border, &b, 4);
        u->lod_bias = op->u.ext.f[0];
        break;
    }
    case H3D_TEXCFG: render_set_texconfig((int)a, (int)b); break;
    case H3D_PROGRAM:
        g_mat_variant = (int)a;
        render_set_program_variant(RPROG_H3D_DEFAULT, (int)a);
        break;
    case H3D_BLENDCOLOR: memcpy(render_frag_state()->blend_color, &a, 4); render_frag_dirty(); break;
    case H3D_TEXBORDER: memcpy(render_texunit(op->unit)->border, &a, 4); break;
    default: break;
    }
}

int h3d_translate_call(uint32_t addr, uint32_t bytes, ROp **out) __attribute__((weak));
static int ext_call(uintptr_t addr, uint32_t size) {
    {   /* debug: ALBW_H3D_CALL_TRACE=<lo>-<hi> (hex addresses): log every executed blob CALL in range */
        static uintptr_t lo = 1, hi = 0;
        if (lo == 1) { const char *e = getenv("ALBW_H3D_CALL_TRACE"); lo = 0; if (e) { char *x; lo = strtoul(e, &x, 16); hi = *x == '-' ? strtoul(x + 1, NULL, 16) : lo; } }
        if (hi && addr >= lo && addr <= hi) { extern long g_plat_frames __attribute__((weak));
            fprintf(stderr, "h3d-call: f=%ld blob %#lx size %u %s\n", &g_plat_frames ? g_plat_frames : 0L, (unsigned long)addr, size, blob_find(addr) ? "" : "(never decoded)"); }
    }
    pthread_mutex_lock(&g_mx);
    Blob *b = blob_find(addr);
    if (!b) {
        pthread_mutex_unlock(&g_mx);
        {   /* a command block the game built at run time (the wall painting strip): translated
               now, at every CALL (sdk/h3d.c h3d_translate_call) */
            ROp *tops = NULL;
            int tn = h3d_translate_call ? h3d_translate_call((uint32_t)addr, size, &tops) : 0;
            if (tn > 0) {
                for (int i = 0; i < tn; i++) render_exec_op(&tops[i]);
                free(tops);
                return 1;
            }
            free(tops);
        }
        static int mt = -1;   /* ALBW_H3D_MISS_TRACE=1: CALLs of blobs that were never decoded */
        if (mt < 0) mt = getenv("ALBW_H3D_MISS_TRACE") != NULL;
        if (mt) { extern long g_plat_frames __attribute__((weak));
                  fprintf(stderr, "h3d-miss: f=%ld blob %#lx size %u\n", &g_plat_frames ? g_plat_frames : 0L, (unsigned long)addr, size); }
        return 0;
    }
    int n = b->n;
    ROp *ops = n ? xrealloc(NULL, sizeof *ops * (size_t)n) : NULL; /* the table may change while running */
    if (n) memcpy(ops, b->ops, sizeof *ops * (size_t)n);
    pthread_mutex_unlock(&g_mx);
    g_st.blob_calls++;
    for (int i = 0; i < n; i++) render_exec_op(&ops[i]);
    free(ops);
    (void)size;
    return 1;
}

void h3d_mem_written(uintptr_t addr, size_t size) __attribute__((weak)); /* sdk/h3d.c: pending LUTs */
static void ext_mem(uintptr_t addr, size_t size) {
    if (h3d_mem_written) h3d_mem_written(addr, size);
    uintptr_t e = addr + size;
    pthread_mutex_lock(&g_mx);
    for (size_t i = 0; i < g_nvtx; i++) {
        H3dVtx *v = g_vtx[i];
        if (v->addr < e && (uintptr_t)v->addr + (uintptr_t)v->verts * v->stride > addr) v->dirty = 1;
        for (int k = 1; h3d_vtx_ext(v) && k < v->nbuffers && k < H3D_MAX_VBUF; k++)   /* the other vertex buffers */
            if (v->baddr[k] < e && (uintptr_t)v->baddr[k] + (uintptr_t)v->verts * v->bstride[k] > addr) v->dirty = 1;
    }
    for (size_t i = 0; i < g_nib; i++)
        if (g_ib[i]->addr < e && g_ib[i]->addr + ((uintptr_t)g_ib[i]->count << g_ib[i]->u16) > addr) g_ib[i]->dirty = 1;
    pthread_mutex_unlock(&g_mx);
}

static void init_once(void) {
    RenderExt e = {ext_op, ext_call, ext_mem, RENDER_VS_H3D, h3d_variant_defines};
    render_set_ext(&e);
}

void h3d_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_once);
}
