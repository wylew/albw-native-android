/* Android only (tools/link_android.sh compiles native/android/): the Ghidra text of USA 00103D30
 * with its hard-coded 3DS VRAM start 0x1f000000 mapped onto the host VRAM block (ALBW_VRAM_ADDR,
 * README: Android boot stop A4). Otherwise unchanged. */
#include "ghidra_types.h"

extern int func_00109904(int param_1);
extern int func_0010BAB4(undefined4 param_1);
extern int func_0010BB14(int param_1);
extern undefined4 DAT_00103e94;
extern uint DAT_00103e98;

// USA 00103D30 <- EU 00103D30 (Ghidra, non-matching)
// GLOBAL DAT_00103e94 undefined4
// GLOBAL DAT_00103e98 uint

undefined4 * func_00103D30(undefined4 *param_1)

{
  uint uVar1;
  uint uVar2;
  uint *puVar3;
  uint uVar4;
  undefined1 uVar5;
  
  *(undefined1 *)(param_1 + 1) = 0;
  *param_1 = DAT_00103e94;
  param_1[6] = 0;
  param_1[7] = 0;
  param_1[8] = 0;
  param_1[9] = 0;
  uVar1 = func_00109904(0x20000);
  uVar2 = func_00109904(0x30000);
  puVar3 = (uint *)func_0010BAB4(0x14);
  uVar4 = DAT_00103e98;
  if (puVar3 == (uint *)0x0) {
    puVar3 = (uint *)0x0;
  }
  else {
    *puVar3 = ALBW_VRAM_ADDR(0x1f000000);
    puVar3[1] = uVar1;
    puVar3[3] = 0;
    puVar3[2] = ALBW_VRAM_ADDR(0x1f000000);
    uVar1 = func_0010BB14(0x20000);
    if (uVar1 < (puVar3[1] + *puVar3) - 1) {
      if ((*puVar3 < uVar4) || (uVar1 = func_0010BB14(0x30000), uVar1 < (puVar3[1] + *puVar3) - 1))
      {
        uVar5 = 2;
      }
      else {
        uVar5 = 1;
      }
    }
    else {
      uVar5 = 0;
    }
    *(undefined1 *)(puVar3 + 4) = uVar5;
  }
  param_1[6] = puVar3;
  puVar3 = (uint *)func_0010BAB4(0x14);
  if (puVar3 != (uint *)0x0) {
    puVar3[2] = 0;
    puVar3[3] = 0;
    *puVar3 = uVar4;
    puVar3[1] = uVar2;
    puVar3[2] = uVar4;
    uVar1 = func_0010BB14(0x20000);
    if (uVar1 < (puVar3[1] + *puVar3) - 1) {
      if ((*puVar3 < uVar4) || (uVar4 = func_0010BB14(0x30000), uVar4 < (puVar3[1] + *puVar3) - 1))
      {
        uVar5 = 2;
      }
      else {
        uVar5 = 1;
      }
    }
    else {
      uVar5 = 0;
    }
    *(undefined1 *)(puVar3 + 4) = uVar5;
  }
  param_1[7] = puVar3;
  param_1[8] = param_1[6];
  param_1[9] = puVar3;
  return param_1;
}

