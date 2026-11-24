/* compilefix: func_002D3CD4, the converted stage-2 text (build/decomp/ghidra_fallback/ghidra_analyzed5_v4/002/func_002D3CD4.c) with its inlined two-sided approach
 * restored by hand (Ghidra dropped the approach-from-above branch; BOOT_LOG 150). */
#include "ghidra_types.h"

#define CHEAT_PAINT_ENERGY 8u
/* cheat switches (platform/cheats.c, Esc menu > Cheats); weak: builds without the platform layer */
extern uint32_t albw_cheat_code __attribute__((weak));
#define ALBW_CHEAT(bit) (&albw_cheat_code && (albw_cheat_code & (bit)))
extern int func_00587B50(int param_1,int *param_2,int param_3);
extern int func_00587F74(int param_1,int *param_2,int param_3);
extern int *DAT_002d3fa4;
extern float DAT_002d3fa8;
extern float DAT_002d3fac;
extern int DAT_002d3fb0;
extern float DAT_002d3fb4;
extern undefined DAT_002d3fb8;
extern float DAT_002d3fe8;
extern float DAT_002d3fec;
extern float DAT_002d3ff0;
extern int DAT_002d3ff4;
extern undefined DAT_002d3ff8;
extern char s_SE_S_MAGIC_RECOVER_002d3fd4[];
extern char s_SE_S_MAGIC_WARNING_002d4014[];
extern char s__SE_S_MAGIC_RECOVER_FIN_002d3fbb[];
extern char s__SE_S_MAGIC_WARNING_FAST_002d3ffb[];
extern char s_cf_002d3fbb[];
extern char s_cf_002d3ffb[];
extern undefined4 _DAT_002d3fb8;
extern undefined4 _DAT_002d3ff8;

// USA 002D3CD4 <- EU 002D3CB0 (Ghidra, non-matching)
// GLOBAL DAT_002d3fa4 int *
// GLOBAL DAT_002d3fa8 float
// GLOBAL DAT_002d3fac float
// GLOBAL DAT_002d3fb0 int
// GLOBAL DAT_002d3fb4 float
// GLOBAL DAT_002d3fb8 undefined
// GLOBAL DAT_002d3fe8 float
// GLOBAL DAT_002d3fec float
// GLOBAL DAT_002d3ff0 float
// GLOBAL DAT_002d3ff4 int
// GLOBAL DAT_002d3ff8 undefined
// GLOBAL s_cf_002d3fbb string
// GLOBAL s_cf_002d3ffb string
// GLOBAL s_SE_S_MAGIC_RECOVER_002d3fd4 string
// GLOBAL s_SE_S_MAGIC_WARNING_002d4014 string

/* WARNING: Removing unreachable block (ram,0x002d3d64) */
/* WARNING: Removing unreachable block (ram,0x002d3d68) */
/* WARNING: Removing unreachable block (ram,0x002d3d78) */
/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

#ifndef ALBW_APPROACH_DEFINED
#define ALBW_APPROACH_DEFINED
/* inlined two-sided approach (tools/approach_fix.py, BOOT_LOG 150): v -> t by s, no overshoot */
static float albw_approach(float v, float t, float s) {
    float n;
    if (v < t) { n = v + s; return (t <= n || v > n) ? t : n; }
    if (v > t) { n = v - s; if (n <= t) return t; return n <= v ? n : t; }
    return v;
}
#endif
undefined1 func_002D3CD4(int param_1,int param_2,int param_3,int param_4)

{
  float fVar1;
  int iVar2;
  undefined1 uVar3;
  float fVar4;
  float fVar5;
  float fVar6;
  float fVar7;
  _Alignas(8) unsigned char albw_frame[0x20];
  #define local_18 (*(int *)(albw_frame + 0x8))
  #define local_14 (*(char * *)(albw_frame + 0xc))
  fVar1 = DAT_002d3ff0;
  fVar4 = DAT_002d3fec;
  fVar7 = DAT_002d3fe8;
  local_18 = DAT_002d3fb0;
  fVar6 = DAT_002d3fac;
  fVar5 = DAT_002d3fa8;
  if (param_4 == 0) {
    iVar2 = *DAT_002d3fa4;
    if (*(int *)(param_1 + 0x20) < 1) {
      if ((*(int *)(param_2 + 4) == 0x3f800000) && (*(char *)(param_1 + 0x39) != '\0')) {
        fVar5 = *(float *)(param_1 + 0x10) - *(float *)(param_1 + 0x14);
        *(float *)(param_1 + 0x10) = fVar5;
        if (fVar6 < fVar5) {
          fVar6 = fVar5;
        }
        *(float *)(param_1 + 0x10) = fVar6;
        return 0;
      }
      if (*(int *)(param_2 + 4) != 0x3f800000) {
        uVar3 = 0;
        if (*(char *)(param_2 + 1) == '\0') {
          if (*(float *)(param_1 + 0x14) < *(float *)(param_1 + 0x10)) {
            /* cheat "Painting costs no energy" (Action Replay EUR: nop at EU 002D3ECC, this vsub) */
            if (!ALBW_CHEAT(CHEAT_PAINT_ENERGY))
              *(float *)(param_1 + 0x10) = *(float *)(param_1 + 0x10) - *(float *)(param_1 + 0x14);
          }
          else if (param_3 != 0) {
            *(float *)(param_1 + 0x10) = DAT_002d3fac;
            uVar3 = 1;
            goto LAB_002d3eec;
          }
          uVar3 = 0;
        }
LAB_002d3eec:
        if (*(char *)(param_1 + 0x38) == '\0') {
          fVar7 = fVar4;
        }
        fVar7 = fVar7 * fVar1;
        fVar6 = *(float *)(param_1 + 0x10);
        if (fVar7 < *(float *)(param_1 + 0x10)) {
          fVar6 = fVar7;
        }
        fVar6 = (fVar5 / fVar7) * fVar6;
        if ((int)ALBW_AS_U32(fVar6) <= DAT_002d3ff4) {
          if (_DAT_002d3ff8 < (int)ALBW_AS_U32(fVar6)) {
            if ((*(uint *)(param_1 + 0x18) & 0xf) == 0) {
              local_14 = s_SE_S_MAGIC_WARNING_002d4014;
              func_00587F74(iVar2,&local_18,0);
            }
          }
          else if ((*(uint *)(param_1 + 0x18) & 7) == 0) {
            local_14 = s__SE_S_MAGIC_WARNING_FAST_002d3ffb + 1;
            func_00587F74(iVar2,&local_18,0);
          }
        }
        *(int *)(param_1 + 0x18) = *(int *)(param_1 + 0x18) + 1;
        return uVar3;
      }
      iVar2 = *(int *)(param_1 + 0x1c);
      if (0 < iVar2) {
        *(int *)(param_1 + 0x1c) = iVar2 + -1;
      }
      if (iVar2 == 0) {
        fVar5 = *(float *)(param_1 + 0x10) + fVar5;
        *(float *)(param_1 + 0x10) = fVar5;
        fVar6 = fVar7;
        if (*(char *)(param_1 + 0x38) == '\0') {
          fVar6 = fVar4;
        }
        if (fVar6 * fVar1 < fVar5) {
          if (*(char *)(param_1 + 0x38) == '\0') {
            fVar7 = fVar4;
          }
          *(float *)(param_1 + 0x10) = fVar7 * fVar1;
        }
        *(undefined4 *)(param_1 + 0x18) = 0;
      }
    }
    else {
      fVar5 = *(float *)(param_1 + 0x2c);
      fVar7 = *(float *)(param_1 + 0x28);
      fVar6 = (fVar7 - fVar5) * ALBW_AS_F32(_DAT_002d3fb8);
      if (fVar6 <= DAT_002d3fac) {
        fVar6 = -fVar6;
      }
      fVar4 = DAT_002d3fa8;
      if ((DAT_002d3fa8 <= fVar6) && (fVar4 = fVar6, DAT_002d3fb4 < fVar6)) {
        fVar4 = DAT_002d3fb4;
      }
      fVar4 = albw_approach(fVar5, fVar7, fVar4);   /* two-sided (BOOT_LOG 150); reached = at target */
      if (fVar4 == fVar7) {
        *(float *)(param_1 + 0x2c) = fVar7;
        *(undefined4 *)(param_1 + 0x20) = 0;
        local_14 = s__SE_S_MAGIC_RECOVER_FIN_002d3fbb + 1;
        func_00587F74(iVar2,&local_18,0);
      }
      else {
        *(float *)(param_1 + 0x2c) = fVar4;
        *(float *)(param_1 + 0x10) = *(float *)(param_1 + 0x10) + (fVar4 - fVar5);
        local_14 = s_SE_S_MAGIC_RECOVER_002d3fd4;
        func_00587B50(iVar2,&local_18,0);
      }
    }
  }
  return 0;
}

