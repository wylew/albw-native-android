/* Native OpenGL 3.3 renderer core (Tier B of native/GRAPHICS_STACK.md).
 *
 * No emulation: no PICA200 command word is ever decoded. The game's GPU-state PRODUCERS (nn::gr
 * MakeCommand functions, engine state wrappers, later h3d/lyt/font/eft draw code) are replaced by
 * native functions that describe the state with the structured values they were given (render
 * target addresses, viewport rectangle, blend factors, texture descriptors, combiner stages...).
 *
 * Deferred execution. On the 3DS those producers write into a command list that the GPU executes
 * later, interleaved with MemoryFill / DisplayTransfer requests (nngx request order). To keep that
 * order the replacements RECORD a native op (render_rec) instead of touching GL: the op is stored
 * in a native op pool and a 2-word native marker {token, RENDER_MARK} is written at the command
 * pointer, so the caller's pointer arithmetic, MoveCommandbufferPointer, memcpy of prebuilt blocks
 * and sub-list appends all keep working and carry the marker along. When gfx.c executes a 3D
 * command chunk of a running list (in request order with fills and transfers), render_exec_cmds()
 * finds the native markers in that range and runs their ops on the GL thread. Only the markers
 * written by our own replacements are recognised; every other word (raw PICA from producers not
 * yet replaced) is skipped without being looked at.
 *
 * Coordinate conventions (see render.c "conventions"): render targets and display images are GL
 * textures whose GL row y = the PICA window y (viewport origin bottom-left); textures decoded from
 * game memory use the same convention; the LCD rotation is applied only at scan-out.
 *
 * Threading: recording (render_rec, render_mem_written) is thread-safe and needs no GL context.
 * Everything that executes (render_exec_cmds, render_fill, render_transfer, render_scanout, the
 * immediate state/draw API) must run on the thread that owns the GL context; when called without
 * a current GL context those functions return 0 / do nothing (callers keep their Tier A fallback).
 *
 * 64-bit: every address is a host address (uintptr_t / pointer). Game structures that hold 32-bit
 * address fields are widened by the caller ((uintptr_t)(uint32_t)field: gfx.c maps all GPU memory
 * below 2 GiB, see gfx.c header). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ formats (PICA codes) */

/* colour buffer / display transfer format (PICA framebuffer format codes) */
enum { RFMT_RGBA8 = 0, RFMT_RGB8 = 1, RFMT_RGB565 = 2, RFMT_RGB5A1 = 3, RFMT_RGBA4 = 4 };
/* depth buffer format (PICA register 0x116) */
enum { RDEPTH_D16 = 0, RDEPTH_D24 = 2, RDEPTH_D24S8 = 3 };

/* ------------------------------------------------------------------ state values */

/* fragment-operation state, field values are the PICA / nn::gr::CTR enum values:
 *  blend eq: 0 add 1 sub 2 revsub 3 min 4 max
 *  blend factor: 0 zero 1 one 2 src 3 1-src 4 dst 5 1-dst 6 srcA 7 1-srcA 8 dstA 9 1-dstA
 *                10 const 11 1-const 12 constA 13 1-constA 14 srcA_saturate
 *  test func: 0 never 1 always 2 eq 3 ne 4 lt 5 le 6 gt 7 ge
 *  stencil op: 0 keep 1 zero 2 replace 3 incr 4 decr 5 invert 6 incr_wrap 7 decr_wrap
 *  logic op: 0 clear 1 and 2 and_rev 3 copy 4 set 5 copy_inv 6 noop 7 invert 8 nand 9 or 10 nor
 *            11 xor 12 equiv 13 and_inv 14 or_rev 15 or_inv
 *  cull: 0 none, 1 cull counter-clockwise (in window space), 2 cull clockwise */
typedef struct {
    uint8_t blend_enable, eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a;
    uint8_t blend_color[4];
    uint8_t logic_enable, logic_op;
    uint8_t alpha_test, alpha_func, alpha_ref;
    uint8_t stencil_test, stencil_func, stencil_ref, stencil_mask, stencil_write;
    uint8_t stencil_fail, stencil_zfail, stencil_zpass;
    uint8_t depth_test, depth_func, depth_write;
    uint8_t color_mask; /* bit 0 R, 1 G, 2 B, 3 A */
    uint8_t cull;
    uint8_t poly_offset_enable, wbuffer;
    float depth_near, depth_far, poly_offset_units;
} RFragState;

/* texture unit (PICA texture unit registers / nn::gr::CTR texture object):
 *  fmt: TEXFMT_* (texdecode.h); type: 0 2D 1 cube 2 shadow2D 3 projection 4 shadowCube 5 disabled
 *  wrap: 0 clamp-to-edge 1 clamp-to-border 2 repeat 3 mirrored-repeat
 *  mag: 0 nearest 1 linear; min: 0 nearest 1 nearest-mip-nearest 2 nearest-mip-linear 3 linear
 *       4 linear-mip-nearest 5 linear-mip-linear */
typedef struct {
    uintptr_t addr;        /* level 0 (cube: +X face) */
    uintptr_t cube[6];     /* cube faces +X -X +Y -Y +Z -Z */
    uint16_t w, h;
    uint8_t fmt, type, wrap_s, wrap_t, mag, min, min_level, max_level;
    uint8_t border[4];
    float lod_bias;
} RTexUnit;

/* texture combiner (TexEnv) stage, PICA values:
 *  source: 0 primary(vertex) colour 1 fragment primary 2 fragment secondary 3 tex0 4 tex1 5 tex2
 *          6 tex3 13 previous buffer 14 constant 15 previous
 *  rgb operand: 0 c 1 1-c 2 a 3 1-a 4 r 5 1-r 8 g 9 1-g 12 b 13 1-b
 *  alpha operand: 0 a 1 1-a 2 r 3 1-r 4 g 5 1-g 6 b 7 1-b
 *  combine: 0 replace 1 modulate 2 add 3 add-signed 4 interpolate 5 subtract 6 dot3-rgb
 *           7 dot3-rgba 8 multiply-add 9 add-multiply;  scale: 0 x1 1 x2 2 x4 */
typedef struct {
    uint8_t src_rgb[3], op_rgb[3], combine_rgb, scale_rgb;
    uint8_t src_a[3], op_a[3], combine_a, scale_a;
    uint8_t konst[4];
} RTevStage;

typedef struct {
    RTevStage stage[6];
    uint8_t buf_update_rgb, buf_update_a; /* bit i (i = 0..3): stage i's output is written to the
                                             combiner buffer read by the following stages */
    uint8_t buf_color[4];                 /* initial combiner buffer colour */
} RTev;

/* vertex attribute array: type 0 s8, 1 u8, 2 s16, 3 f32; 1-4 components */
enum { RATTR_BYTE = 0, RATTR_UBYTE = 1, RATTR_SHORT = 2, RATTR_FLOAT = 3 };
enum { RPRIM_TRIANGLES = 0, RPRIM_STRIP = 1, RPRIM_FAN = 2 };
#define RENDER_MAX_ATTRIBS 12   /* PICA vertex shader input registers v0..v11 */
#define RENDER_VS_FLOATS 96     /* PICA float uniforms c0..c95 */

/* hand-written GLSL vertex programs (see SHADERS.md). Each implements one game vertex shader with
 * the SAME uniform register interface (c[], b, i[]) and input registers (v0..v11), so producers
 * only have to say which program and pass the uniform values the game computed. */
enum {
    RPROG_CLIP = 0,      /* v0 = clip-space position (PICA z convention), v1 colour, v2 uv0: tests/blits */
    RPROG_GR_DEFAULT,    /* placeholder for Shader/gr_default_shader.shbin (see SHADERS.md) */
    RPROG_FONT_RECT,     /* nwfont2_RectDrawerShader.shbin (nw::font + nw::lyt): GLSL in font.c */
    RPROG_H3D_DEFAULT,   /* placeholder for nwh3d_DefaultShader.bch */
    RPROG_PRIMITIVE,     /* sead PrimitiveRenderer (primitive_renderer_ctr.bsm): GLSL in prim.c */
    RPROG_EFT,           /* nw::eft particle programs (JackAll.ptcl): GLSL in effects.c, variant in b */
    RPROG_COUNT
};

/* hand-written GLSL of the game's vertex programs that live outside render.c */
extern const char RENDER_VS_FONT_RECT[];   /* font.c: nwfont2_RectDrawerShader */

/* ------------------------------------------------------------------ setup */

/* Called lazily by every GL entry point; returns 0 when no GL context is current on this thread
 * (then nothing GL-side happens). Loads the GL functions not in platform/gl.h and compiles the
 * built-in programs on first success. */
int render_ready(void);

/* ------------------------------------------------------------------ memory events (any thread) */

/* the game (CPU, DMA, TextureCopy) wrote [addr, addr+size): cached textures there are re-uploaded
 * on their next use. Colour surfaces are not affected (their contents live in GL). */
void render_mem_written(const void *addr, size_t size);

/* nngx display buffer memory (DisplaybufferStorage); a DisplayTransfer into it fills a GL "display
 * image" that render_scanout shows. fmt: RFMT_*. Thread-safe; GL objects are created lazily. */
void render_display_register(const void *mem, size_t bytes, int w, int h, int fmt);
void render_display_unregister(const void *mem);

/* ------------------------------------------------------------------ requests (GL thread) */

/* MemoryFill of [start, start+size) with a 16/24/32-bit pattern: glClear of the colour or
 * depth/stencil surface at that address (format-aware decoding of the value). Fills of addresses
 * with no surface yet are remembered and applied when one is created there. Returns 1 if a GL
 * surface was cleared. */
int render_fill(const void *start, uint32_t size, uint32_t value, int bits);

/* DisplayTransfer (block -> linear, nngxAddB2LTransferCommand / GX cmd 3): blit of the colour
 * surface at `src` (sw x sh, created lazily if unknown) into the display image or colour surface at
 * `dst` (dw x dh), with format conversion, AA downscale (aa 0x500 none, 0x501 2x horizontal,
 * 0x502 2x2: aa 0 / 1 / 2) and optional vertical flip. `src` may lie inside a larger colour surface
 * (whole rows into it: the screens share one render buffer). Returns 1 if done in GL. */
int render_transfer(const void *src, int sw, int sh, int sfmt, void *dst, int dw, int dh, int dfmt, int flip, int aa);

/* TextureCopy / BlockImageCopy of `size` bytes: if `src` is a colour surface, the destination
 * becomes a GL copy of it (render-to-texture); else just marks dst written. */
void render_block_copy(const void *src, void *dst, uint32_t size);

/* Scan-out: draw the display image held by display buffer `mem` into the bound screen target of
 * `screen` (0 top, 1 bottom; call after plat_begin_screen), applying the LCD rotation. Returns 1
 * when the display buffer has a GL image (else the caller falls back). */
int render_scanout(int screen, const void *mem);

/* ------------------------------------------------------------------ recording (any thread) */

#define RENDER_MARK 0xA1B7C0DEu   /* second word of a native marker (never a PICA header with a
                                     sane register id: bits 20-27 = 0x1B extra words) */
#define RENDER_MARK_WORDS 2

typedef enum {
    ROP_NONE = 0,
    ROP_TARGET,      /* colour/depth buffer bind */
    ROP_VIEWPORT,
    ROP_SCISSOR,
    ROP_FRAG,        /* full fragment-operation state (RenderState::MakeCommand) */
    ROP_BLEND,       /* blend part of RFragState */
    ROP_LOGICOP,
    ROP_ALPHATEST,
    ROP_STENCIL,
    ROP_DEPTH,       /* depth test/func/write + colour mask */
    ROP_COLORMASK,
    ROP_CULL,
    ROP_DEPTHMAP,    /* depth range / polygon offset / w-buffer */
    ROP_TEXUNIT,     /* one texture unit */
    ROP_TEXCONFIG,   /* texture unit enables / coordinate routing */
    ROP_TEV,         /* six combiner stages + buffer config */
    ROP_TEV_CONST,   /* constant colour of the stages in a mask */
    ROP_PROGRAM,
    ROP_VS_FLOAT,    /* c[reg .. reg+count) */
    ROP_VS_BOOL,
    ROP_ATTRIB,      /* one vertex attribute array / constant */
    ROP_DRAW,
    ROP_CALL,        /* execute the markers of another command range (sub-list, prebuilt block) */
    ROP_TEV_STAGES,  /* some combiner stages (flags = stage mask bits 0-5); unit bit 0: also the
                        buffer-update bits, bit 1: also the buffer colour (u.tev) */
    ROP_EXT,         /* executed by the registered extension (render_set_ext; nw::h3d: render/h3d.c) */
    ROP_FL,          /* 3DS fragment lighting state (render/fraglight.h: u.fl.kind FL_*) */
    ROP_FOG,         /* PICA fog (u.fog): mode (0 off, 5 fog), z flip, colour, LUT key (0 = keep) */
    ROP_COUNT
} ROpType;

typedef struct {
    uint16_t type;
    uint8_t unit;      /* ROP_TEXUNIT: unit 0..2; ROP_ATTRIB: register; ROP_VS_FLOAT: count */
    uint8_t flags;     /* op-specific */
    union {
        struct { uintptr_t color, depth; uint16_t w, h; uint8_t cfmt, dfmt, has_color, has_depth; } target;
        struct { int32_t x, y, w, h; uint8_t enable; } rect;   /* viewport / scissor */
        RFragState frag;
        RTexUnit tex;
        struct { uint8_t enable_mask, tex2_from_tc1, tex3_enable, tex3_coord; } texcfg;
        RTev tev;
        struct { uint8_t stage_mask; uint8_t rgba[4]; } tev_const;
        struct { uint32_t id; uintptr_t key; } program;      /* RPROG_*, key = game program identity */
        struct { uint8_t reg; float v[4 * 16]; } vsf;        /* up to 16 registers per op */
        struct { uint16_t mask; } vsb;
        struct { uintptr_t ptr; uint16_t stride; uint8_t type, count; float konst[4]; } attrib;
        struct { uintptr_t indices; uint32_t first, count; uint8_t prim, index_u16, indexed; } draw;
        struct { uintptr_t addr; uint32_t size; } call;
        struct { uint16_t kind; uintptr_t ptr; uint32_t a, b, c; float f[8]; } ext; /* ROP_EXT */
        struct { uint8_t mode, flip, rgb[3]; uint32_t key; } fog;           /* ROP_FOG */
        struct { uint8_t kind; uint8_t pad[3]; uint8_t data[248]; } fl;   /* ROP_FL
                                                                             payloads (fixed-layout
                                                                             structs of those modules) */
    } u;
} ROp;

/* Record `op` and write its 2-word native marker at `p`. Returns p + RENDER_MARK_WORDS (callers
 * continue writing after it, like after the PICA words the original function wrote). p == NULL:
 * the op is executed later only through render_exec_op. Identical ops share one pool entry. */
uint32_t *render_rec(uint32_t *p, const ROp *op);

/* Same, for markers written once into a block that is replayed for the rest of the run (shader /
 * state blocks built at init and copied into every frame's list): the pooled op is never
 * garbage-collected, even if the block is not executed for a long time. */
uint32_t *render_rec_pinned(uint32_t *p, const ROp *op);

/* Execute the native markers found in [addr, addr+size) in address order (gfx.c: a 3D command
 * chunk of a running list; ROP_CALL: a sub-range). GL thread; returns the number of ops run. */
int render_exec_cmds(const void *addr, uint32_t size);

/* Execute one op now (GL thread). */
void render_exec_op(const ROp *op);

/* end of a host frame: op-pool housekeeping (gfx.c calls it at SwapBuffers) */
void render_frame_end(void);
/* PICA fog lookup table (registers 0xE6 / 0xE8..): 128 entries {value, difference} (the factor at
 * depth index i + fraction f is value + difference * f), registered under a content key by the
 * producer (thread-safe); ROP_FOG selects it by key. */
void render_fog_lut_set(uint32_t key, const float lut[128][2]);
void render_ss_loaded(void);   /* save states: game memory was replaced (re-read every texture) */
size_t render_pool_export(void **out);          /* save states: the op pool (malloc'd blob) */
int render_pool_import(const void *in, size_t n);
/* internal resolution: multiples of the 3DS screens (1..8, ALBW_RENDER_SCALE at start), changeable between frames */
int render_get_scale(void);
void render_set_scale(int scale);
void render_set_widescreen(int rows);   /* top screen long side: 400 = off, 427 = 16:9 */
float albw_ws_factor(void);             /* rows / 400: the game widens its projections by it */

/* ------------------------------------------------------------------ immediate API (GL thread)
 * What the ops do; also usable directly by native replacements that run on the GL thread at
 * command-list execution time. */

/* bind the render target (colour and/or depth surface by address; created lazily) */
void render_set_target(uintptr_t color, int cfmt, uintptr_t depth, int dfmt, int w, int h);
void render_set_viewport(int x, int y, int w, int h);
void render_set_scissor(int enable, int x, int y, int w, int h);
RFragState *render_frag_state(void);     /* modify, then render_frag_dirty() */
void render_frag_dirty(void);
void render_set_texunit(int unit, const RTexUnit *t);
void render_set_texconfig(int enable_mask, int tex2_from_tc1);
void render_set_tev(const RTev *tev);
void render_set_tev_const(int stage_mask, const uint8_t rgba[4]);
/* replace the stages in stage_mask (bits 0-5) with t->stage[i]; flags bit 0: also buf_update_rgb/a,
 * bit 1: also buf_color */
void render_set_tev_stages(int stage_mask, const RTev *t, int flags);
void render_set_program(int prog);
void render_set_vs_float(int reg, const float *v, int count);   /* count vec4 registers */
void render_set_vs_bool(uint16_t mask);
const float *render_vs_floats(uint16_t *bools);   /* debug readback (96 vec4 + bool mask) */
const RTev *render_tev_state(void);              /* debug readback of the combiner */
void render_set_attrib(int reg, const void *ptr, int type, int count, int stride);
void render_set_attrib_const(int reg, const float v[4]);
void render_disable_attribs(void);
void render_draw(int prim, uint32_t first, uint32_t count);
void render_draw_indexed(int prim, const void *indices, int index_u16, uint32_t count);

/* ------------------------------------------------------------------ extensions (nw::h3d)
 * A renderer extension (render/h3d.c) owns its own GL buffers and decoded resources. It plugs in
 * through these hooks so render.c keeps no dependency on it (the standalone tests link render.c
 * without it):
 *   op:   executes ROP_EXT ops (GL thread);
 *   call: consulted first for every ROP_CALL target: returns 1 if it ran native ops it holds for
 *         that address (command blobs of model files decoded once at load), else the range is
 *         scanned for markers as usual;
 *   mem:  told about render_mem_written ranges (any thread).
 * vs_src / defines: GLSL body (no #version line) of RPROG_H3D_DEFAULT and the #define block of a
 * variant (render_set_program_variant); NULL keeps the generic placeholder program. */
typedef struct {
    void (*op)(const ROp *op);
    int (*call)(uintptr_t addr, uint32_t size);
    void (*mem)(uintptr_t addr, size_t size);
    const char *h3d_vs_body;
    void (*h3d_defines)(int variant, char *buf, size_t n);
} RenderExt;
void render_set_ext(const RenderExt *ext);

#define RENDER_PROG_VARIANTS 128
/* select a program and a compile-time variant of it (only RPROG_H3D_DEFAULT has variants; built
 * lazily on first use). render_set_program(p) = variant 0. */
void render_set_program_variant(int prog, int variant);
/* current program id (and variant) */
int render_current_program(int *variant);
/* ROP_PROGRAM ops whose key (game program identity, e.g. a DVLE address) is registered here select
 * this program / variant instead of the op's program id. Thread-safe. */
void render_program_alias(uintptr_t key, int prog, int variant);

/* current texture unit state (GL thread), for extensions that set a unit in several parts */
RTexUnit *render_texunit(int unit);

/* 3DS fragment lighting: render/fraglight.h (state set through ROP_FL ops, hand-written GLSL
 * evaluated in the fragment program of the nw::h3d programs). */

/* Draw with GL buffers owned by the caller (GL thread, inside an op): render_draw_bind() binds the
 * render target and applies the pipeline state, program, uniforms and textures like render_draw;
 * the caller then binds its VAO and issues the GL draw, and calls render_draw_done(). Returns 0
 * (draw dropped) when there is no render target. */
int render_draw_bind(void);
void render_draw_done(void);

/* ------------------------------------------------------------------ debugging / tests */

/* read back the colour surface at `color_addr` (GL row order, RGBA8, w*h*4 bytes); 0 if unknown */
int render_read_surface(uintptr_t color_addr, uint8_t *rgba, int *w, int *h);
/* counters for traces */
typedef struct { uint64_t ops_run, ops_recorded, ops_pooled, fills, transfers, draws, tex_uploads, surfaces; } RStats;
RStats render_stats(void);
/* HD texture packs (render/hdtex.c): upload every 2D texture again after a switch / import */
void render_hdtex_refresh(void);
