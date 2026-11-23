/* USA 0056F090 (EU 0056F068): curve value = weight(byte +0xC) * scale(+0) * curve(t).
 * The curve function from the table at DAT_0056f0fc (index byte +0xD) takes and returns a float in
 * s0 (EU 0056F0B4: vldr s0, [r4, #20]; blx r0 / 0056F0D8: vstr s0, [r4, #24]); the Ghidra text
 * lost both (`fVar3 = param_1[5]; (*pcVar1)();`). On ARM the callee then read r0 (= its own
 * address) as its argument: f2u16 -> func_003E21B4 indexed its sine table far out of range and the
 * sound thread crashed after a save-state load (README: Android stop A13).
 * ARM (softfp): exact, float argument and result in r0 (the table's f2u16 returns the float's bits
 * as u32 in r0 too). Other targets: the previous behaviour (the curve passes t through), because on
 * i386 the table function's u32 return is in eax, not the x87 st(0) a float call would read.
 * Confidence: high (EU disassembly). */
#include "ghidra_types.h"

extern float DAT_0056f0f8;   /* 0.0f */
extern int DAT_0056f0fc;     /* curve function table */
extern float DAT_0056f100;   /* 1.0f: no curve function */

float func_0056F090(float *param_1) {
    float zero = DAT_0056f0f8;
    if (param_1[0] == zero) return zero;
    if ((uint)ALBW_AS_U32(param_1[2]) > (uint)ALBW_AS_U32(param_1[4])) return zero;
    uint8_t kind = *(uint8_t *)((uint8_t *)param_1 + 0xd);
    void *fn = *(void **)(uintptr_t)(DAT_0056f0fc + (uint)kind * 4);
    float v = DAT_0056f100;
    if (fn) {
#if defined(__arm__)
        float (*curve)(float) = (float (*)(float))fn;
        if (kind == 4) {
            if (*(uint8_t *)((uint8_t *)param_1 + 0x1d) == 0) v = param_1[6];
            else { v = curve(param_1[5]); param_1[6] = v; }
        } else {
            v = curve(param_1[5]);
        }
#else
        if (kind == 4 && *(uint8_t *)((uint8_t *)param_1 + 0x1d) == 0) v = param_1[6];
        else {
            v = param_1[5];
            ((void (*)(void))fn)();
            if (kind == 4) param_1[6] = v;
        }
#endif
    }
    float w = (float)*(uint8_t *)((uint8_t *)param_1 + 0xc);
    return w * (param_1[0] * v);
}
