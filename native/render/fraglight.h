/* 3DS fragment lighting on the native renderer (no emulation).
 *
 * The game's materials get their colour from the PICA fragment-lighting unit: per-fragment
 * lights (up to 8), lookup tables (LUTs) indexed by dot products (N.H, V.H, N.V, L.N, spot,
 * tangent cos phi), distance attenuation, Fresnel, bump mapping. The results are the combiner
 * sources FRAGMENT_PRIMARY (diffuse) and FRAGMENT_SECONDARY (specular).
 *
 * Producers (nn::gr LightEnv / FragmentLight MakeCommand, the h3d material light paths,
 * native/sdk/light.c) record ROP_FL ops with the STRUCTURED values they were given (colours,
 * view-space light vectors, LUT input selections...). LUT contents come from the LUT files
 * (BCH LUT sections), decoded once when the file is set up (fraglight_lut_set) and referenced
 * by their address. The evaluation is hand-written GLSL (RENDER_FS_FRAGLIGHT, fraglight.c) with
 * the LUTs in one 256 x 24 float texture (one row per PICA LUT id; 256 entries, value + delta).
 *
 * LUT ids (rows): 0 D0, 1 D1, 3 FR, 4 RB, 5 RG, 6 RR, 8+i SP of light i, 16+i DA of light i.
 * Input selectors: 0 N.H, 1 V.H, 2 N.V, 3 L.N, 4 spot (L . spot direction), 5 cos phi.
 * Scale codes: 0 x1, 1 x2, 2 x4, 3 x8, 6 x0.25, 7 x0.5.
 * LUT configs (which LUTs a config provides): 0 D0 RR SP, 1 RR FR SP, 2 RR D0 D1, 3 D0 D1 FR,
 * 4 RR RG RB D0 D1 SP, 5 RR RG RB D0 FR SP, 6 RR D0 D1 FR SP, 8 all. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "render.h"

/* ROP_FL kinds (op.u.fl.kind) */
enum {
    FL_ENV = 1,      /* FLEnv: enable, light count / permutation, config, ambient, per-light masks */
    FL_LIGHT,        /* FLLight: one light's colours (+ config, distance attenuation) */
    FL_POS,          /* FLPos: view-space positions / spot directions of some lights */
    FL_LUTCFG,       /* FLLutCfg: LUT input abs flags / selectors / scales */
    FL_LUT,          /* FLLutBind: LUT id <- decoded LUT table */
    FL_POLYOFFSET,   /* FLPoly: depth offset of the material (polygon offset) */
};

/* LUT order of FLLutCfg arrays and abs bits */
enum { FLI_D0 = 0, FLI_D1, FLI_SP, FLI_FR, FLI_RB, FLI_RG, FLI_RR, FLI_COUNT };

typedef struct {
    uint8_t enable;          /* fragment lighting on (else primary = secondary = 0) */
    uint8_t nlights;         /* 1..8 when enabled */
    uint8_t perm[8];         /* slot k -> light id */
    uint8_t ambient[3];      /* global ambient RGB */
    uint8_t config;          /* LUT config 0..8 */
    uint8_t fresnel_sel;     /* bit 0: Fresnel -> primary alpha, bit 1: -> secondary alpha */
    uint8_t clamp_highlights;
    uint8_t bump_mode;       /* 0 none, 1 normal map, 2 tangent map */
    uint8_t bump_sel;        /* texture unit 0..2 */
    uint8_t bump_renorm;     /* recompute z of the perturbation */
    uint8_t shadow;          /* bit 0 enable, 1 primary, 2 secondary, 3 invert, 4 alpha (no shadow
                                maps on this path: recorded only) */
    uint8_t lut_off;         /* LUTs disabled by the environment: bit FLI_* (D0 D1 FR RR RG RB) */
    uint8_t spot_mask;       /* per light id: spot attenuation on */
    uint8_t da_mask;         /* per light id: distance attenuation on */
    uint8_t shadow_mask;     /* per light id: shadowed */
} FLEnv;

enum { FLL_COLOR = 1, FLL_CONFIG = 2, FLL_DIST = 4 };
typedef struct {
    uint8_t id, parts;       /* light id; FLL_* parts present */
    uint8_t directional, two_sided, geo0, geo1;
    uint8_t diffuse[3], ambient[3], spec0[3], spec1[3];
    float da_bias, da_scale;
} FLLight;

typedef struct {
    uint8_t n;
    struct { uint8_t id, directional, pad[2]; float pos[3], spot[3]; } l[8];
} FLPos;

typedef struct {
    uint8_t abs;             /* bit FLI_*: absolute input */
    uint8_t sel[FLI_COUNT];  /* input selector */
    uint8_t scale[FLI_COUNT];/* scale code */
    uint8_t set;             /* bit FLI_*: selector/scale given by this op (others kept) */
} FLLutCfg;

typedef struct { uint8_t id; uint8_t pad[3]; uint32_t lut; } FLLutBind; /* lut: fraglight_lut_find, 0 none */

typedef struct { uint8_t enable; uint8_t pad[3]; float units; } FLPoly;

/* ---- load time (any thread): decoded LUT tables, keyed by the address of their source blob */
/* entries[i] = (value, delta) for LUT index i (0..255) */
void fraglight_lut_set(uintptr_t blob, const float entries[256][2]);
uint32_t fraglight_lut_find(uintptr_t blob);          /* handle (> 0) or 0 */
/* decoded LUT input words of a material (selector / scale per LUT), keyed by the material */
void fraglight_material_set(uintptr_t key, const FLLutCfg *cfg);
int fraglight_material_get(uintptr_t key, FLLutCfg *cfg);

/* ---- op helpers (any thread) */
ROp fraglight_op(int kind, const void *payload, size_t n);

/* ---- execution (GL thread, render.c) */
void fraglight_exec(const ROp *op);
void fraglight_reset(void);
size_t fraglight_export(void **out);           /* save states: LUT / material registries */
int fraglight_import(const void *in, size_t n);

/* the hand-written GLSL (fragment program part, inside #if FL; see fraglight.c) */
extern const char RENDER_FS_FRAGLIGHT[];

/* per-program uniform locations + upload (render.c) */
typedef struct { int cfg, in0, scale, amb, light, lut, row; uint32_t gen; } FLLoc;
void fraglight_locations(unsigned prog, FLLoc *loc);
/* uploads the state if it changed since this program last got it; binds the LUT texture on
 * texture unit `unit` */
void fraglight_upload(FLLoc *loc, int unit);
/* specialised programs (render.c): the lighting configuration as 4 key words, and the GLSL of the
 * uniforms, LUT helpers and an fl_compute with that configuration baked in (malloc'd) */
void fraglight_spec_key(uint32_t k[4]);
char *fraglight_spec_glsl(const uint32_t k[4]);

/* debugging */
const FLEnv *fraglight_env(void);
#include <stdio.h>
void fraglight_debug_print(FILE *f);   /* lights + LUT bindings (ALBW_H3D_DUMP) */
uint64_t fraglight_state_hash(void); /* debug: ALBW_LIGHT_TRACE */
