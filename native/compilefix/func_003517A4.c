/* USA 003517A4 (EU 00351780), compilefix of the Ghidra text: the call to 005820C8 (area lookup,
 * tail-calls the area's callback 005230E0 -> child vtable +0x10, e.g. 0037DC1C: clamp a vector to
 * the area bounds) passes five arguments on ARM (EU 00351850..00351878: r0 = *DAT_00351930,
 * r1 = &local_64, r2 = afStack_4c, r3 = param_3, [sp] = &local_40); the Ghidra text passed only
 * r0. 0037DC1C then used whatever r2 held (here a float of the vector copy, e.g. 0x41844585 =
 * 16.5) as a vector pointer: SIGBUS / SIGSEGV in 00115C70 when D-pad right was pressed in Hyrule
 * Castle town (README: Android stop A18).
 * Confidence: high (EU disassembly; frame offsets matched through the 00115C70 / 004D7FBC calls). */
#include "ghidra_types.h"

extern int func_00115C54(float *param_1,float *param_2,float *param_3);
extern int func_00115C70(float *param_1,float *param_2,float *param_3);
extern int func_002E12A0(undefined4 *param_1);
extern float func_004D7FBC(float *param_1,float *param_2,uint param_3,uint param_4,float fparam_0);
extern int func_004D838C(float *param_1,float *param_2,float fparam_0,float fparam_1,float fparam_2);
extern int func_00581DAC(int param_1);
extern int func_00581DF8(int param_1,undefined4 param_2,undefined4 param_3);
extern int func_005820C8(int param_1);
extern float DAT_0035192c;
extern int *DAT_00351930;
extern float DAT_00351934;
extern float DAT_00351938;
extern float DAT_0035193c;

// USA 003517A4 <- EU 00351780 (Ghidra, non-matching)
// GLOBAL DAT_0035192c float
// GLOBAL DAT_00351930 int *
// GLOBAL DAT_00351934 float
// GLOBAL DAT_00351938 float
// GLOBAL DAT_0035193c float

undefined4 func_003517A4(int param_1,float *param_2,undefined4 param_3,float *param_4,float fparam_0)

{
  bool bVar1;
  int *piVar2;
  int iVar3;
  float fVar4;
  float fVar5;
  _Alignas(8) unsigned char albw_frame[0x70];
  #define local_64 (*(float *)(albw_frame + 0xc))
  #define local_60 (*(float *)(albw_frame + 0x10))
  #define local_5c (*(float *)(albw_frame + 0x14))
  #define local_58 (*(float *)(albw_frame + 0x18))
  #define local_54 (*(float *)(albw_frame + 0x1c))
  #define local_50 (*(float *)(albw_frame + 0x20))
  #define afStack_4c ((float *)(albw_frame + 0x24))
  #define local_40 (*(float *)(albw_frame + 0x30))
  #define fStack_3c (*(float *)(albw_frame + 0x34))
  #define fStack_38 (*(float *)(albw_frame + 0x38))
  #define local_34 (*(float *)(albw_frame + 0x3c))
  #define fStack_30 (*(float *)(albw_frame + 0x40))
  #define fStack_2c (*(float *)(albw_frame + 0x44))
  local_64 = DAT_0035192c;
  local_60 = DAT_0035192c;
  local_5c = DAT_0035192c;
  fVar4 = DAT_0035192c;
  func_004D7FBC(afStack_4c,param_2,*(uint *)(param_1 + 8),*(uint *)(param_1 + 0xc),fparam_0);
  piVar2 = DAT_00351930;
  iVar3 = func_00581DAC(*DAT_00351930);
  if (((iVar3 == 0) || (*(char *)(param_1 + 0x22) != '\0')) || (*(char *)(param_1 + 0x58) != '\0'))
  {
    bVar1 = false;
  }
  else {
    bVar1 = true;
  }
  if (param_4 != (float *)0x0) {
    func_00115C70(&local_58,param_2,param_4);
    if ((local_58 != fVar4) || (local_54 != fVar4 || local_50 != fVar4)) {
      local_64 = local_58;
      local_60 = local_54;
      local_5c = local_50;
      func_00115C70(&local_34,afStack_4c,&local_58);
      local_40 = local_34;
      fStack_3c = fStack_30;
      fStack_38 = fStack_2c;
      /* EU 00351850..00351878: r1 = &local_64 (the clamped offset), r2 = afStack_4c, r3 = param_3,
       * [sp] = &local_40; Ghidra kept r0 only */
      ((int (*)(int, float *, float *, undefined4, float *))func_005820C8)(*piVar2, &local_64, afStack_4c, param_3, &local_40);
    }
    fVar4 = param_4[1];
    fVar5 = param_4[2];
    *param_2 = *param_4;
    param_2[1] = fVar4;
    param_2[2] = fVar5;
    func_002E12A0(&local_40);
    func_004D838C((float *)(param_1 + 0x290),&local_64,local_40,fStack_3c,fStack_38);
    return func_00115C54(param_2,param_2,(float *)(param_1 + 0x290));
  }
  func_00581DF8(*piVar2,&local_64,afStack_4c);
  if (bVar1) {
    func_004D838C((float *)(param_1 + 0x198),&local_64,DAT_0035193c,DAT_00351938,DAT_00351934);
  }
  else {
    *(float *)(param_1 + 0x198) = local_64;
    *(float *)(param_1 + 0x19c) = local_60;
    *(float *)(param_1 + 0x1a0) = local_5c;
  }
  return func_00115C54(param_2,param_2,(float *)(param_1 + 0x198));
}

