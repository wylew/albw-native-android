/* nwfont2_RectDrawerShader on OpenGL: the hand-written GLSL of the game's RectDrawer vertex program
 * (used by nw::font text and by every nw::lyt pane) and the recording helpers of the native
 * RectDrawer producers (native/sdk/lyt.c).
 *
 * The program (DVLB in RegionBoot.szs, DVLE symbol "nwfont2_rectdrawershader_version_3002",
 * 228 instruction words, main at 109) was read as disassembly and its computation rewritten by
 * hand below; nothing of the bytecode is translated mechanically or used at run time.
 *
 * Uniform interface (register numbers from the program's code; its DVLE uniform table only names
 * the version constant):
 *   c0-c3   projection rows                           c4  stereo parallax (eye offset, k, depth, -)
 *   c5      DVLE constant (0, 1, 2, 0.5)              c6-c8 per-texture vec4 (texcoord subroutine)
 *   c9-c31  rect records, one per rect slot:
 *             x = modelview index (pool) + flags/256    y = geometry index + flags/16
 *             z = texcoord index + flags/64            w = alpha (<= 1) or colour index + flags/4
 *   c32-c95 vec4 pool indexed by the records (3x4 modelview rows, geometry (size.xy, pos.zw),
 *           texcoord sets, corner colours, glyph scale/offset/italic)
 *   b0      glyph (text) path, b1..b6 texcoord 0..2 rotate / flip, b2 also "per-draw colours" in
 *           the glyph path
 * Every vec4 the game uploads is copied from memory in its natural order m0..m3 into the uniform
 * port, which takes the w component first, so register = (m3, m2, m1, m0) and the program reads
 * all of them with a .wzyx swizzle. c[] keeps the register values (render.h convention) and the
 * GLSL reads M(r) = c[r].wzyx, exactly like the original.
 *
 * Vertex input v0 = (rect slot, vertex index, corner x in {0,1}, corner y in {0,-1}).
 * Outputs: o0 position, o1 colour, o2/o3/o4 texcoord 0/1/2 (FS_TEV inputs v_color, v_tc0..2). */
#include "font.h"
#include "../platform/gl.h"   /* GLSL_HDR */

#include <string.h>

const char RENDER_VS_FONT_RECT[] =
    GLSL_HDR
    "layout(location = 0) in vec4 v0; layout(location = 1) in vec4 v1; layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3; layout(location = 4) in vec4 v4; layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6; layout(location = 7) in vec4 v7;\n"
    "uniform vec4 c[96]; uniform int b; uniform float u_ws;\n"
    "out vec4 v_color; out vec4 v_tc0; out vec2 v_tc1; out vec2 v_tc2; out vec3 v_normal; out vec3 v_view;\n"
    "vec4 M(int r){ return c[clamp(r, 0, 95)].wzyx; }\n"            /* game memory order */
    "bool B(int i){ return ((b >> i) & 1) != 0; }\n"
    /* flag bits are stored as binary fractions of the record components: bit(v, k) tests the
     * k-th bit after the point (k = 1: 0.5, 2: 0.25 ...) like the shader's floor/fract/compare
     * chains (the shader shifts the fraction left by doubling and tests >= 0.5) */
    "bool bit(float v, int k){ return fract(v * exp2(float(k - 1))) >= 0.5; }\n"
    "vec4 pica_out(vec4 p){ return vec4(p.x, p.y, -(2.0 * p.z + p.w), p.w); }\n"
    "vec4 r0; vec4 pos;\n"            /* rect record; pane-space position (x, y, 0, 1) */
    "int ia, ib;\n"                   /* a0.x (modelview index), a0.y (geometry index) */
    /* modelview (3x4 rows at pool[ia]) + stereo parallax from c4 (shader lines 97-108) */
    "vec4 transform(vec4 p){\n"
    "  vec4 q = vec4(dot(M(32 + ia), p), dot(M(33 + ia), p), dot(M(34 + ia), p), p.w);\n"
    "  vec4 s = M(4);\n"
    "  float d = s.z - M(34 + ia).w;\n"
    "  if (s.x != 0.0 && d > 0.0) q.x = q.x + s.x - s.y / d;\n"
    "  return q;\n"
    "}\n"
    /* texture coordinate of one texture (shader subroutine 0-96). flags = the two bits selecting
     * the mode, t = index of the texcoord vec4s (advanced by 1 or 2), k = per-texture vec4
     * (c6/c7/c8), g = rect geometry (size, pos) */
    "vec2 texcoord(vec2 flags, inout float t, vec4 k, vec4 g){\n"
    "  int it = int(t);\n"
    "  vec2 n = vec2(0.0); vec2 o;\n"
    "  bool fit = bit(r0.x, 1);\n"
    "  if (fit) {\n"                  /* lines 5-49: corner position inside the texture window */
    "    n = vec2(v0.z, 1.0 + v0.w);\n"
    "    vec2 w = M(6).xy;\n"
    "    if (bit(r0.x, 2)) w = w.yx;\n"
    "    w *= g.xy;\n"
    "    if (bit(r0.x, 3)) n.x *= w.x;\n"
    "    if (bit(r0.x, 4)) n.y = n.y * w.y + 1.0 - w.y;\n"
    "    w = 1.0 - w;\n"
    "    if (bit(r0.x, 5)) n.x += w.x;\n"
    "    if (bit(r0.x, 6)) n.y -= w.y;\n"
    "    if (bit(r0.x, 7)) n.x = 1.0 - n.x;\n"
    "    if (bit(r0.x, 8)) n.y = 1.0 - n.y;\n"
    "    if (bit(r0.x, 2)) n = 1.0 - n.yx;\n"
    "    n.y = 1.0 - n.y;\n"
    "  }\n"
    "  if (flags.y < 1.0) {\n"        /* lines 52-68: one vec4 (left, top, right, bottom) or window */
    "    vec4 m = M(32 + it);\n"
    "    if (fit) { o = n * m.xy + m.zw; o *= k.zw; o.y = 1.0 - o.y; }\n"
    "    else { o.x = v0.z == 0.0 ? m.x : m.z; o.y = v0.w == 0.0 ? m.y : m.w; }\n"
    "    t += 1.0;\n"
    "  } else {\n"                    /* lines 69-95 */
    "    if (flags.x < 1.0) {\n"
    "      if (fit) {\n"              /* 2x4 texture matrix */
    "        vec4 q = vec4(n, 0.0, 1.0);\n"
    "        o = vec2(dot(M(32 + it), q), dot(M(33 + it), q)) * k.zw; o.y = 1.0 - o.y;\n"
    "      } else {\n"                /* four corner texcoords: (TL, BR) (BL, TR) */
    "        vec4 m0 = M(32 + it), m1 = M(33 + it);\n"
    "        if (v0.w == 0.0) o = v0.z == 0.0 ? m0.xy : m1.zw;\n"
    "        else o = v0.z == 0.0 ? m1.xy : m0.zw;\n"
    "      }\n"
    "    } else {\n"                  /* projection: 2x4 matrix applied to the pane-space position */
    "      o = vec2(dot(M(32 + it), pos), dot(M(33 + it), pos));\n"
    "    }\n"
    "    t += 2.0;\n"
    "  }\n"
    "  return o;\n"
    "}\n"
    "vec2 rotflip(vec2 o, int bs){\n"     /* lines 157-161 (b1/b2, b3/b4, b5/b6) */
    "  if (B(bs)) { o = o.yx; o.y = 1.0 - o.y; }\n"
    "  if (B(bs + 1)) o = 1.0 - o.yx;\n"
    "  return o;\n"
    "}\n"
    "void main(){\n"
    "  r0 = M(9 + int(v0.x));\n"
    "  ia = int(r0.x); ib = int(r0.y);\n"
    "  vec4 g = M(32 + ib);\n"           /* (size.x, size.y, pos.x, pos.y) */
    "  pos = vec4(v0.zw, 0.0, 1.0);\n"
    "  v_normal = vec3(0.0, 0.0, 1.0);\n"
    "  if (!B(0)) {\n"
    /* ---- pane path (nw::lyt) ---- */
    "    vec4 bound = M(31 + ia);\n"       /* gradient frame: the vec4 before the modelview */
    "    pos.xy = pos.xy * g.xy + g.zw;\n"
    "    vec4 q = transform(pos);\n"
    "    gl_Position = pica_out(vec4(dot(M(0), q), dot(M(1), q), dot(M(2), q), dot(M(3), q)));\n"
    /* widescreen (port, not the 3DS program): the viewport is stretched by u_ws along clip y (the
     * top screen's long side). A pane spanning the whole width keeps the stretch (fades,
     * backgrounds); any other pane is drawn 1:1 around the centre (HUD not stretched) */
    "    if (u_ws > 1.0) {\n"
    "      float lo = 1e9, hi = -1e9;\n"
    "      for (int k = 0; k < 4; k++) {\n"
    "        vec4 qc = transform(vec4(vec2(float(k & 1), -float(k >> 1)) * g.xy + g.zw, 0.0, 1.0));\n"
    "        vec4 cc = vec4(dot(M(0), qc), dot(M(1), qc), dot(M(2), qc), dot(M(3), qc));\n"
    "        float n = cc.y / cc.w; lo = min(lo, n); hi = max(hi, n);\n"
    "      }\n"
    "      if (!(lo <= -0.97 && hi >= 0.97)) gl_Position.y /= u_ws;\n"
    "    }\n"
    "    v_view = -q.xyz;\n"
    "    if (r0.w <= 1.0) v_color = vec4(1.0, 1.0, 1.0, r0.w);\n"
    "    else {\n"                         /* bilinear corner colours (TL, TR, BL, BR) */
    "      int ic = int(r0.w);\n"
    "      vec2 f = v0.zw;\n"
    "      if (bit(r0.y, 1)) f = (pos.xy - bound.zw) / bound.xy;\n"
    "      f = abs(f);\n"
    "      vec4 top = mix(M(32 + ic), M(33 + ic), f.x), bot = mix(M(34 + ic), M(35 + ic), f.x);\n"
    "      v_color = mix(top, bot, f.y);\n"
    "    }\n"
    /* texture coordinate flags: two bits per texture taken from the fraction of r0.z */
    "    float t = r0.z;\n"
    "    vec2 fl = 2.0 * vec2(fract(2.0 * r0.z), fract(r0.z));\n"
    "    vec2 tc0 = rotflip(texcoord(fl, t, M(6), g), 1);\n"
    "    float z1 = fl.x; fl = 2.0 * vec2(fract(2.0 * z1), fract(z1));\n"
    "    vec2 tc1 = rotflip(texcoord(fl, t, M(7), g), 3);\n"
    "    float z2 = fl.x; fl = 2.0 * vec2(fract(2.0 * z2), fract(z2));\n"
    "    vec2 tc2 = rotflip(texcoord(fl, t, M(8), g), 5);\n"
    "    v_tc0 = vec4(tc0, 0.0, 1.0); v_tc1 = tc1; v_tc2 = tc2;\n"
    "  } else {\n"
    /* ---- glyph path (nw::font): scale/offset (pool ia+4), italic (ia+3) ---- */
    "    vec4 so = M(36 + ia), it = M(35 + ia);\n"
    "    pos.xy *= so.xy;\n"
    "    pos.xy = pos.xy * g.xy + g.zw;\n"
    "    pos.x += it.x * (1.0 + v0.w);\n"
    "    pos.xy += so.zw;\n"
    "    pos.y += g.y * so.y - g.y;\n"
    "    vec4 q = transform(pos);\n"
    "    gl_Position = pica_out(vec4(dot(M(0), q), dot(M(1), q), dot(M(2), q), dot(M(3), q)));\n"
    "    if (u_ws > 1.0) gl_Position.y /= u_ws;\n"       /* text: always 1:1 (widescreen) */
    "    v_view = -q.xyz;\n"
    "    vec2 a = abs(v0.zw);\n"
    "    int ix = int(r0.z);\n"            /* texcoords (u0, v0', u1, v1') */
    "    int ic = B(2) ? int(r0.x) + 5 : int(r0.w);\n"   /* top / bottom colour pair */
    "    vec4 m = M(32 + ix);\n"
    "    vec2 uv = m.zw * a + vec2(lessThan(a, vec2(1.0))) * m.xy;\n"
    "    uv.y = 1.0 - uv.y;\n"
    "    v_color = mix(M(32 + ic), M(33 + ic), a.y);\n"
    "    v_tc0 = vec4(uv, 0.0, 1.0); v_tc1 = uv; v_tc2 = uv;\n"
    "  }\n"
    "}\n";

void font_build_vertices(float *dst) {
    static const int corner[6] = {1, 0, 3, 3, 0, 2};
    for (int r = 0; r < FONT_RECTS; r++)
        for (int j = 0; j < 6; j++) {
            float *v = dst + (r * 6 + j) * 4;
            int k = corner[j];
            v[0] = (float)r;
            v[1] = (float)k;
            v[2] = (k & 1) ? 1.0f : 0.0f;
            v[3] = (k + 1 >= 3) ? -1.0f : 0.0f;
        }
}

static ROp op_new(ROpType t) {
    ROp op;
    memset(&op, 0, sizeof op); /* zeroed padding: identical ops share one pool entry */
    op.type = (uint16_t)t;
    return op;
}

static uint32_t *rec(uint32_t *p, const ROp *op, int pinned) {
    return pinned ? render_rec_pinned(p, op) : render_rec(p, op);
}

uint32_t *font_rec_uniforms(uint32_t *p, int reg, const void *mem, int count) {
    const float *m = mem;
    while (count > 0) {
        int n = count > 16 ? 16 : count;
        ROp op = op_new(ROP_VS_FLOAT);
        op.unit = (uint8_t)n;
        op.u.vsf.reg = (uint8_t)reg;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) op.u.vsf.v[i * 4 + k] = m[i * 4 + 3 - k]; /* x = m3 .. w = m0 */
        p = render_rec(p, &op);
        m += n * 4;
        reg += n;
        count -= n;
    }
    return p;
}

uint32_t *font_rec_bind(uint32_t *p, const void *vtx, int pinned) {
    ROp op = op_new(ROP_PROGRAM);
    op.u.program.id = RPROG_FONT_RECT;
    op.u.program.key = (uintptr_t)vtx;
    p = rec(p, &op, pinned);
    op = op_new(ROP_ATTRIB);
    op.unit = 0;
    op.u.attrib.ptr = (uintptr_t)vtx;
    op.u.attrib.type = RATTR_FLOAT;
    op.u.attrib.count = 4;
    op.u.attrib.stride = 16;
    p = rec(p, &op, pinned);
    for (int r = 1; r < 8; r++) { /* the program reads v0 only: no stale arrays of other programs */
        op = op_new(ROP_ATTRIB);
        op.unit = (uint8_t)r;
        op.u.attrib.konst[3] = 1.0f;
        p = rec(p, &op, pinned);
    }
    return p;
}

uint32_t *font_rec_draw(uint32_t *p, int rects) {
    ROp op = op_new(ROP_DRAW);
    op.u.draw.prim = RPRIM_TRIANGLES;
    op.u.draw.first = 0;
    op.u.draw.count = (uint32_t)rects * 6u;
    return render_rec(p, &op);
}

uint32_t *font_rec_bools(uint32_t *p, uint32_t value) {
    ROp op = op_new(ROP_VS_BOOL);
    op.u.vsb.mask = (uint16_t)value;
    return render_rec(p, &op);
}

uint32_t *font_rec_texconfig(uint32_t *p, int enable_mask) {
    ROp op = op_new(ROP_TEXCONFIG);
    op.u.texcfg.enable_mask = (uint8_t)(enable_mask & 7);
    return render_rec(p, &op);
}

uint32_t *font_rec_texunit(uint32_t *p, int unit, uintptr_t addr, int w, int h, int fmt, int wrap_s,
                           int wrap_t, int mag_linear, int min_linear) {
    ROp op = op_new(ROP_TEXUNIT);
    op.unit = (uint8_t)unit;
    RTexUnit *t = &op.u.tex;
    t->addr = addr;
    t->w = (uint16_t)w;
    t->h = (uint16_t)h;
    t->fmt = (uint8_t)fmt;
    t->type = 0;
    t->wrap_s = (uint8_t)wrap_s;
    t->wrap_t = (uint8_t)wrap_t;
    t->mag = (uint8_t)(mag_linear != 0);
    t->min = min_linear ? 3 : 0;
    return render_rec(p, &op);
}

uint32_t *font_rec_tev(uint32_t *p, int stage_mask, const RTev *t, int flags, int pinned) {
    ROp op = op_new(ROP_TEV_STAGES);
    op.flags = (uint8_t)(stage_mask & 0x3F);
    op.unit = (uint8_t)flags;
    for (int i = 0; i < 6; i++)
        if (stage_mask >> i & 1) op.u.tev.stage[i] = t->stage[i];
    if (flags & 1) { op.u.tev.buf_update_rgb = t->buf_update_rgb; op.u.tev.buf_update_a = t->buf_update_a; }
    if (flags & 2) memcpy(op.u.tev.buf_color, t->buf_color, 4);
    return rec(p, &op, pinned);
}

uint32_t *font_rec_tev_const(uint32_t *p, int stage, const uint8_t rgba[4]) {
    ROp op = op_new(ROP_TEV_CONST);
    op.u.tev_const.stage_mask = (uint8_t)(1 << stage);
    memcpy(op.u.tev_const.rgba, rgba, 4);
    return render_rec(p, &op);
}

void font_color_pack(const float *rgba, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) {
        float f = rgba[i];
        if (!(f <= 1.0f)) f = 1.0f; /* 00420CB8: clamp to [0, 1], then 0.5 + f * 255 truncated */
        if (f < 0.0f) f = 0.0f;
        out[i] = (uint8_t)(0.5f + f * 255.0f);
    }
}
