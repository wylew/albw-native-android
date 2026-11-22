/* nw::eft particle vertex programs: hand-written GLSL (RPROG_EFT).
 *
 * ---------------------------------------------------------------- where the programs come from
 * The eft shaders are not DVLB files: JackAll.ptcl (EU/RegionBoot.szs) stores 23 vertex programs
 * as baked PICA command streams (code 0x2CB/0x2CC, operand descriptors 0x2D5/0x2D6, entry 0x2BA,
 * output map 0x4F-0x56, constants c0 / c81 (or c75) / c93-c95) in the section at file offset
 * 0x3A03A0. 003F0080 (emitter draw setup) copies the emitter's program stream (+0x254/+0x25C)
 * into the command list: that copy is the bind site. The programs were READ with a disassembly
 * listing as a reading aid and rewritten by hand below; nothing is translated.
 *
 * ---------------------------------------------------------------- RENDER_VS_EFT interface
 *   v0       corner (x, y in +-0.5) and z = register offset of the particle (10 n)
 *   c1-c10   particle block (+ 10 n when instanced): c1 colour (clamped), c2 (scale x, y, texture
 *            rotation 0, 1), c3 / c4 texture 0 / 1 (offset x, y, flip terms z, w), c5 position,
 *            c6 velocity (directional), c7 rotation (x, y, z), c8-c10 emitter rotation rows
 *   c82 emitter colour, c83 texture repeat (u0, v0, u1, v1), c84 (pivot x, y, depth offset toward
 *   the eye), c85 eye position, c86-c89 projection, c90-c92 view (3x4)
 *   constants (program data): c0 = (0, 0.5, 1, 2), c81 = (pi, 2 pi, 1, 256), c93-c95 sine /
 *   cosine polynomial (sin / cos used directly here)
 * Per vertex: corner = (v0.xy + c84.xy) * c2.xy; [rotation: corner = R(-c7) corner, the game's
 * Euler order]; orientation: billboard (view rotation transposed), Y billboard (vertical quad
 * facing the eye), polygon XY / XZ (emitter rows c8-c10, translation c5), directional (x along
 * cross(axis, velocity), y along the velocity; axis = view z or the emitter's y column);
 * world += normalize(eye - c5) * c84.z; clip = proj * view * world.
 * Colour = clamp(c1) * c82. Texcoord t (t = 0, 1; angle a = c2.z / c2.w wrapped to [-pi, pi]):
 * r = rotate(v0.xy, a); u = (0.5 + r.x) c83.x + c3.x - (1 - c3.z) r.x;
 * v = 1 - ((-0.5 + r.y) (-c83.y) + c3.y + (1 - c3.w) r.y)  (c4 / c83.zw for texture 1).
 * Not written: the stripe / complex-shape family (3 programs, entry 0x79). */
#include "effects.h"
#include "../platform/gl.h"   /* GLSL_HDR */

#include <string.h>

const char RENDER_VS_EFT[] =
    GLSL_HDR
    "layout(location = 0) in vec4 v0;\n"
    "uniform vec4 c[96]; uniform int b;\n"
    "out vec4 v_color; out vec4 v_tc0; out vec2 v_tc1; out vec2 v_tc2; out vec3 v_normal; out vec3 v_view;\n"
    "vec4 pica_out(vec4 p){ return vec4(p.x, p.y, -(2.0 * p.z + p.w), p.w); }\n"
    "const float PI = 3.14159265;\n"
    "float wrap_pi(float a){ return a - 2.0 * PI * floor((a + PI) / (2.0 * PI)); }\n"
    /* the game's rotation from Euler angles (sub-routine at 33): angles = -c7 */
    "mat3 euler(vec3 e){\n"
    "  vec3 a = vec3(wrap_pi(-e.x), wrap_pi(-e.y), wrap_pi(-e.z)), C = cos(a), S = sin(a);\n"
    "  vec3 r0 = vec3(C.y * C.z, S.z, -S.y * C.z);\n"
    "  vec3 r1 = vec3(-C.y * S.z * C.x + S.y * S.x, C.z * C.x, S.y * S.z * C.x + C.y * S.x);\n"
    "  vec3 r2 = vec3(C.y * S.z * S.x + S.y * C.x, -C.z * S.x, -S.y * S.z * S.x + C.y * C.x);\n"
    "  return transpose(mat3(r0, r1, r2));\n"           /* rows r0..r2 */
    "}\n"
    "vec2 tex_uv(vec2 corner, float ang, vec4 t, vec2 rep){\n"
    "  float a = wrap_pi(ang), cs = cos(a), sn = sin(a);\n"
    "  vec2 r = vec2(corner.x * cs - corner.y * sn, corner.x * sn + corner.y * cs);\n"
    "  float u = (0.5 + r.x) * rep.x + t.x - (1.0 - t.z) * r.x;\n"
    "  float v = (-0.5 + r.y) * -rep.y + t.y + (1.0 - t.w) * r.y;\n"
    "  return vec2(u, 1.0 - v);\n"
    "}\n"
    "void main(){\n"
    "  int type = b & 7; bool rot = (b & 8) != 0; int a = (b & 16) != 0 ? int(v0.z + 0.5) : 0;\n"
    "  vec4 P1 = c[1 + a], P2 = c[2 + a], P3 = c[3 + a], P4 = c[4 + a], P5 = c[5 + a], P6 = c[6 + a], P7 = c[7 + a];\n"
    "  vec3 E0 = c[8 + a].xyz, E1 = c[9 + a].xyz, E2 = c[10 + a].xyz;\n"
    "  vec3 q = vec3((v0.xy + c[84].xy) * P2.xy, 0.0);\n"
    "  if (type == 3) q = vec3(q.x, 0.0, -q.y);\n"
    "  if (rot) q = euler(P7.xyz) * q;\n"
    "  vec3 w;\n"
    "  if (type == 0) {\n"
    "    w = P5.xyz + vec3(dot(vec3(c[90].x, c[91].x, c[92].x), q), dot(vec3(c[90].y, c[91].y, c[92].y), q),\n"
    "                      dot(vec3(c[90].z, c[91].z, c[92].z), q));\n"
    "  } else if (type == 1) {\n"
    "    vec3 d = c[85].xyz - P5.xyz; d.y = 0.0; d = dot(d, d) > 0.0 ? normalize(d) : vec3(0.0, 0.0, 1.0);\n"
    "    vec3 s = normalize(cross(d, vec3(0.0, 1.0, 0.0)));\n"
    "    w = P5.xyz + vec3(dot(-s, q), q.y, dot(-d, q));\n"
    "  } else if (type == 2 || type == 3) {\n"
    "    w = vec3(dot(E0, q) + P5.x, dot(E1, q) + P5.y, dot(E2, q) + P5.z);\n"
    "  } else {\n"
    "    vec3 ax = type == 4 ? c[92].xyz : vec3(E0.y, E1.y, E2.y);\n"
    "    vec3 vel = dot(P6.xyz, P6.xyz) > 0.0 ? normalize(P6.xyz) : vec3(0.0, 1.0, 0.0);\n"
    "    vec3 x = cross(ax, vel); x = dot(x, x) > 0.0 ? normalize(x) : vec3(1.0, 0.0, 0.0);\n"
    "    vec3 z = cross(x, vel);\n"
    "    w = P5.xyz + x * q.x + vel * q.y + z * q.z;\n"
    "  }\n"
    "  vec3 toeye = c[85].xyz - P5.xyz;\n"
    "  if (dot(toeye, toeye) > 0.0) w += normalize(toeye) * c[84].z;\n"
    "  vec4 vw = vec4(dot(c[90], vec4(w, 1.0)), dot(c[91], vec4(w, 1.0)), dot(c[92], vec4(w, 1.0)), 1.0);\n"
    "  gl_Position = pica_out(vec4(dot(c[86], vw), dot(c[87], vw), dot(c[88], vw), dot(c[89], vw)));\n"
    "  v_color = clamp(P1, 0.0, 1.0) * c[82];\n"
    "  v_tc0 = vec4(tex_uv(v0.xy, P2.z, P3, c[83].xy), 0.0, 1.0);\n"
    "  v_tc1 = tex_uv(v0.xy, P2.w, P4, c[83].zw); v_tc2 = v_tc1;\n"
    "  v_normal = vec3(0.0, 0.0, 1.0); v_view = -vw.xyz;\n"
    "}\n";

/* FNV-1a of a program's code words (identifies a PTCL program; data, not translation) */
uint32_t effects_program_hash(const uint32_t *code, int words) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < words; i++)
        for (int k = 0; k < 4; k++) h = (h ^ ((code[i] >> (8 * k)) & 0xFF)) * 16777619u;
    return h;
}

/* JackAll.ptcl programs, classified by hand from their listings (program index in the shader
 * section: orientation, rotation, instanced). Hashes are filled in by the PTCL loader the first
 * time it sees the section in that order (effects_variant_of falls back to -1 = unknown). */
static const struct { uint8_t index; uint8_t variant; } TABLE[] = {
    {0, EFT_POLY_XZ | EFT_ROT | EFT_INST}, {1, EFT_POLY_XY | EFT_ROT}, {2, EFT_BILLBOARD | EFT_ROT | EFT_INST},
    {3, EFT_POLY_XY}, {4, EFT_BILLBOARD | EFT_INST}, {5, EFT_DIR_VIEW | EFT_ROT | EFT_INST},
    {6, EFT_DIR_VIEW | EFT_INST}, {7, EFT_BILLBOARD | EFT_ROT}, {8, EFT_DIR_EMITTER | EFT_INST},
    {9, EFT_DIR_EMITTER | EFT_ROT | EFT_INST}, {10, EFT_POLY_XZ | EFT_INST}, {11, EFT_POLY_XY | EFT_ROT | EFT_INST},
    {12, EFT_POLY_XZ | EFT_ROT}, {13, EFT_POLY_XZ}, {15, EFT_YBILLBOARD | EFT_INST}, {16, EFT_POLY_XY | EFT_ROT},
    {18, EFT_DIR_VIEW | EFT_ROT}, {20, EFT_POLY_XY | EFT_INST}, {21, EFT_POLY_XY}, {22, EFT_BILLBOARD},
};

static uint32_t g_hash[23];

int effects_variant_of(uint32_t hash) {
    for (size_t i = 0; i < sizeof TABLE / sizeof *TABLE; i++)
        if (g_hash[TABLE[i].index] == hash && hash) return TABLE[i].variant;
    return -1;
}

/* the PTCL loader registers the program hashes in section order (index 0..22) */
void effects_register_program(int index, uint32_t hash) {
    if (index >= 0 && index < 23) g_hash[index] = hash;
}
