/* sead PrimitiveRenderer (World/Shader/primitive_renderer_ctr.bsm) on the native renderer:
 * the hand-written GLSL of its vertex program (render.c RPROG_PRIMITIVE). Producers: native/sdk/prim.c.
 *
 * ---------------------------------------------------------------- RENDER_VS_PRIMITIVE
 * Written by hand from the program's symbol table (DVLE) and a disassembly listing used as a
 * reading aid (20 instructions; nothing is translated). Interface = the game's registers:
 *   c0-c3 user (4x4 rows; the shape draws write rows 0-2, 004979CC row 3 = (0, 0, 0, 1))
 *   c4-c7 wvp (projection x view, 4x4 rows)    c8 color0   c9 color1   c10 uv_src   c11 uv_size
 *   v0 Vertex.xyz   v1 TexCoord0.xy   v2 ColorRate.x
 * Program: position = wvp * (user * (Vertex, 1)); colour = color0 * (1 - rate) + color1 * rate;
 * texcoord 0 = uv_src.xy + TexCoord0 * uv_size.xy (w = 1).
 * The second program (+ the DMP line geometry shader, dmp_Line.width = c67) expands lines to
 * quads; the binary's PrimitiveRenderer entry points that survive (00496A28 quad, 00496ABC
 * textured quad, 00496F50 sphere) never select it (sdk/prim.c records its state only).
 * PICA clip space -> GL: pica_out (render.c conventions). */
#include "prim.h"
#include "../platform/gl.h"   /* GLSL_HDR */

const char RENDER_VS_PRIMITIVE[] =
    GLSL_HDR
    "layout(location = 0) in vec4 v0; layout(location = 1) in vec4 v1; layout(location = 2) in vec4 v2;\n"
    "uniform vec4 c[96]; uniform int b;\n"
    "out vec4 v_color; out vec4 v_tc0; out vec2 v_tc1; out vec2 v_tc2; out vec3 v_normal; out vec3 v_view;\n"
    "vec4 pica_out(vec4 p){ return vec4(p.x, p.y, -(2.0 * p.z + p.w), p.w); }\n"
    "void main(){\n"
    "  vec4 p = vec4(v0.xyz, 1.0);\n"
    "  vec4 w = vec4(dot(c[0], p), dot(c[1], p), dot(c[2], p), dot(c[3], p));\n"
    "  gl_Position = pica_out(vec4(dot(c[4], w), dot(c[5], w), dot(c[6], w), dot(c[7], w)));\n"
    "  float r = v2.x;\n"
    "  v_color = c[8] * (1.0 - r) + c[9] * r;\n"
    "  vec2 uv = c[10].xy + v1.xy * c[11].xy;\n"
    "  v_tc0 = vec4(uv, 0.0, 1.0); v_tc1 = uv; v_tc2 = uv;\n"
    "  v_normal = vec3(0.0, 0.0, 1.0); v_view = vec3(0.0);\n"
    "}\n";
