/* Android only (tools/link_android.sh compiles native/android/): the Ghidra text of USA 001339A4
 * with its hard-coded 3DS VRAM start 0x1f000000 mapped onto the host VRAM block (ALBW_VRAM_ADDR,
 * README: Android boot stop A4). Otherwise unchanged. */
#include "ghidra_types.h"

extern int func_0010BB14(int param_1);
extern longlong func_001104E4(int param_1,int param_2);
extern uint DAT_00133c74;
extern int *DAT_00133c78;

// USA 001339A4 <- EU 001339A4 (Ghidra, non-matching)
// GLOBAL DAT_00133c74 uint
// GLOBAL DAT_00133c78 int *

void func_001339A4(int *param_1,int param_2)

{
  undefined1 uVar1;
  uint uVar2;
  int iVar3;
  uint uVar4;
  int *piVar5;
  int *piVar6;
  uint *puVar7;
  int *piVar8;
  uint unaff_r8;
  int *unaff_r10;
  int iVar9;
  bool bVar10;
  longlong lVar11;
  
  if ((param_1[0x1c] < param_2) && (param_2 != 0)) {
    do {
      uVar2 = param_1[0x1c];
      piVar5 = param_1;
      if (uVar2 < 7) {
        piVar5 = param_1 + uVar2 * 4;
      }
      uVar2 = uVar2 + 1;
      if (uVar2 < 7) {
        unaff_r10 = param_1 + uVar2 * 4;
      }
      param_1[0x1c] = uVar2;
      if (uVar2 >= 7) {
        unaff_r10 = param_1;
      }
      *(undefined4 *)(*piVar5 + 0xc) = *(undefined4 *)(*piVar5 + 8);
      *(undefined4 *)(piVar5[1] + 0xc) = *(undefined4 *)(piVar5[1] + 8);
      piVar8 = (int *)*piVar5;
      iVar9 = piVar8[2];
      if (iVar9 < 0) {
        lVar11 = func_001104E4(iVar9 - 0x7f,0x80);
        iVar3 = (int)lVar11;
      }
      else {
        lVar11 = func_001104E4(iVar9 + 0x7f,0x80);
        iVar3 = (int)lVar11;
      }
      piVar6 = (int *)piVar5[1];
      uVar2 = piVar8[1] + *piVar8;
      bVar10 = (uint)(iVar3 * 0x80) < uVar2;
      if (bVar10) {
        unaff_r8 = uVar2 + iVar3 * -0x80;
      }
      iVar3 = piVar6[2];
      if (!bVar10) {
        unaff_r8 = 0;
      }
      if (iVar3 < 0) {
        lVar11 = func_001104E4(iVar3 - 0x7f,0x80);
        iVar3 = (int)lVar11;
      }
      else {
        lVar11 = func_001104E4(iVar3 + 0x7f,0x80);
        iVar3 = (int)lVar11;
      }
      if ((uint)(iVar3 * 0x80) < (uint)(piVar6[1] + *piVar6)) {
        uVar2 = piVar6[1] + *piVar6 + iVar3 * -0x80;
      }
      else {
        uVar2 = 0;
      }
      if (0 < (int)unaff_r8) {
        puVar7 = (uint *)*unaff_r10;
        if (iVar9 < 0) {
          lVar11 = func_001104E4(iVar9 - 0x7f,0x80);
          iVar9 = (int)lVar11;
        }
        else {
          lVar11 = func_001104E4(iVar9 + 0x7f,0x80);
          iVar9 = (int)lVar11;
        }
        uVar4 = iVar9 * 0x80;
        piVar8[2] = uVar4 + unaff_r8;
        *puVar7 = uVar4;
        if (uVar4 == 0) {
LAB_00133b54:
          uVar1 = 2;
        }
        else {
          puVar7[3] = 0;
          puVar7[2] = uVar4;
          puVar7[1] = unaff_r8;
          if ((*puVar7 < ALBW_VRAM_ADDR(0x1f000000)) ||
             (uVar4 = func_0010BB14(0x20000), uVar4 < (*puVar7 + puVar7[1]) - 1)) {
            if ((*puVar7 < DAT_00133c74) ||
               (uVar4 = func_0010BB14(0x30000), uVar4 < (*puVar7 + puVar7[1]) - 1))
            goto LAB_00133b54;
            uVar1 = 1;
          }
          else {
            uVar1 = 0;
          }
        }
        *(undefined1 *)(puVar7 + 4) = uVar1;
      }
      iVar9 = piVar5[1];
      if (0 < (int)uVar2) {
        iVar3 = *(int *)(iVar9 + 8);
        puVar7 = (uint *)unaff_r10[1];
        if (iVar3 < 0) {
          lVar11 = func_001104E4(iVar3 - 0x7f,0x80);
          iVar3 = (int)lVar11;
        }
        else {
          lVar11 = func_001104E4(iVar3 + 0x7f,0x80);
          iVar3 = (int)lVar11;
        }
        uVar4 = iVar3 * 0x80;
        *(uint *)(iVar9 + 8) = uVar4 + uVar2;
        *puVar7 = uVar4;
        if (uVar4 == 0) {
LAB_00133c20:
          uVar1 = 2;
        }
        else {
          puVar7[3] = 0;
          puVar7[2] = uVar4;
          puVar7[1] = uVar2;
          if ((*puVar7 < ALBW_VRAM_ADDR(0x1f000000)) ||
             (uVar2 = func_0010BB14(0x20000), uVar2 < (*puVar7 + puVar7[1]) - 1)) {
            if ((*puVar7 < DAT_00133c74) ||
               (uVar2 = func_0010BB14(0x30000), uVar2 < (*puVar7 + puVar7[1]) - 1))
            goto LAB_00133c20;
            uVar1 = 1;
          }
          else {
            uVar1 = 0;
          }
        }
        *(undefined1 *)(puVar7 + 4) = uVar1;
      }
      uVar2 = param_1[0x1c];
    } while ((int)uVar2 < param_2);
    piVar5 = param_1;
    if (uVar2 < 7) {
      piVar5 = param_1 + uVar2 * 4;
    }
    iVar9 = *DAT_00133c78;
    *(int *)(iVar9 + 0x20) = *piVar5;
    if ((uint)param_1[0x1c] < 7) {
      param_1 = param_1 + param_1[0x1c] * 4;
    }
    *(int *)(iVar9 + 0x24) = param_1[1];
  }
  return;
}

