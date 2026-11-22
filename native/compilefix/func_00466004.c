/* USA 00466004 <- EU 00465FDC (stage-2 Ghidra text) with a dropped indirect-call argument restored
 * (CLASS indirect call dropped arg, BOOT_LOG 129): vtable +0x18 (allocate size, alignment) at EU 00466444 gets
 * r1 = 0x8000, r2 = 0x80 (mov r2, #128 at 00466430); Ghidra printed (piVar12,0x8000).
 * Also two string-object method calls without `this` (EU 466134, 4662C8: r0 = the object whose vtable
 * was just stored) - the castle door (StageOut) crashed in 00691BD0, BOOT_LOG 147 - and the inline
 * physical -> virtual conversion of entry +8 made identity (as 001139F8 is natively). */
#include "ghidra_types.h"

extern int func_00100174(int *param_1);
extern int func_0010CEE0(undefined4 param_1,int *param_2,undefined4 param_3);
extern undefined8 func_0010E3C4(uint *param_1,uint *param_2,uint param_3);
extern int func_001139F8(int param_1);
extern int func_001164F4(undefined4 *param_1,uint param_2);
extern int func_001E0B94(int param_1,undefined4 param_2,int param_3);
extern int func_00504D48(int param_1,int param_2);
extern undefined DAT_004662f0;
extern int DAT_004662f4;
extern char *DAT_004662f8;
extern int DAT_00466318;
extern int DAT_0046631c;
extern undefined1 DAT_00466320;
extern int *DAT_00466ac4;
extern int *DAT_00466ac8;
extern int DAT_00466acc;
extern uint *DAT_00466ad0;
extern uint *DAT_00466ad4;
extern uint *DAT_00466ad8;
extern uint *DAT_00466adc;
extern uint *DAT_00466ae0;
extern uint *DAT_00466ae4;
extern uint *DAT_00466ae8;
extern uint *DAT_00466aec;
extern char s_Layout_MapTexture__00466304[];
extern char s_World_004662fc[];
extern char s_cf_00466304[];

// USA 00466004 <- EU 00465FDC (Ghidra, non-matching)
// GLOBAL DAT_004662f0 undefined
// GLOBAL DAT_004662f4 int
// GLOBAL DAT_004662f8 char *
// GLOBAL DAT_00466318 int
// GLOBAL DAT_0046631c int
// GLOBAL DAT_00466320 undefined1
// GLOBAL DAT_00466ac4 int *
// GLOBAL DAT_00466ac8 int *
// GLOBAL DAT_00466acc int
// GLOBAL DAT_00466ad0 uint *
// GLOBAL DAT_00466ad4 uint *
// GLOBAL DAT_00466ad8 uint *
// GLOBAL DAT_00466adc uint *
// GLOBAL DAT_00466ae0 uint *
// GLOBAL DAT_00466ae4 uint *
// GLOBAL DAT_00466ae8 uint *
// GLOBAL DAT_00466aec uint *
// GLOBAL EXT_00000004 undefined
// GLOBAL s_cf_00466304 string
// GLOBAL s_World_004662fc string

void func_00466004(int param_1,int *param_2,undefined4 param_3,uint param_4)

{
  char cVar1;
  uint uVar2;
  undefined4 *puVar3;
  undefined4 uVar4;
  char *pcVar5;
  uint *puVar6;
  uint *puVar7;
  uint uVar8;
  uint uVar9;
  int iVar10;
  int iVar11;
  int *piVar12;
  uint uVar13;
  uint *puVar14;
  undefined2 *puVar15;
  int iVar16;
  int iVar17;
  uint *puVar18;
  uint *puVar19;
  bool bVar20;
  bool bVar21;
  _Alignas(8) unsigned char albw_frame[0x2e0];
  #define local_2dc (*(int *)(albw_frame + 0x4))
  #define local_2d8 (*(uint * *)(albw_frame + 0x8))
  #define local_2d4 (*(int *)(albw_frame + 0xc))
  #define local_2d0 ((uint *)(albw_frame + 0x10))
  #define uStack_1d1 (*(undefined1 *)(albw_frame + 0x10f))
  #define local_1c8 (*(int *)(albw_frame + 0x118))
  #define local_1c4 (*(uint * *)(albw_frame + 0x11c))
  #define local_1c0 (*(int *)(albw_frame + 0x120))
  #define local_1bc (*(uint * *)(albw_frame + 0x124))
  #define local_1b8 (*(int *)(albw_frame + 0x128))
  #define local_1b4 ((uint *)(albw_frame + 0x12c))
  #define uStack_b5 (*(undefined1 *)(albw_frame + 0x22b))
  #define local_b0 (*(uint *)(albw_frame + 0x230))
  #define local_a8 (*(uint * *)(albw_frame + 0x238))
  #define local_9c (*(int *)(albw_frame + 0x244))
  #define local_98 (*(int *)(albw_frame + 0x248))
  #define local_94 (*(char * *)(albw_frame + 0x24c))
  #define local_90 (*(undefined4 *)(albw_frame + 0x250))
  #define local_8c (*(undefined4 *)(albw_frame + 0x254))
  #define local_88 (*(undefined4 *)(albw_frame + 0x258))
  #define uStack_84 (*(undefined4 *)(albw_frame + 0x25c))
  #define local_80 (*(undefined4 *)(albw_frame + 0x260))
  #define local_7c (*(undefined4 *)(albw_frame + 0x264))
  #define local_78 (*(undefined4 *)(albw_frame + 0x268))
  #define local_74 (*(undefined4 *)(albw_frame + 0x26c))
  #define local_70 (*(undefined4 *)(albw_frame + 0x270))
  #define local_6c (*(int *)(albw_frame + 0x274))
  #define local_68 (*(char * *)(albw_frame + 0x278))
  #define local_64 (*(undefined1 *)(albw_frame + 0x27c))
  #define local_63 (*(undefined1 *)(albw_frame + 0x27d))
  #define local_62 (*(undefined1 *)(albw_frame + 0x27e))
  #define local_61 (*(undefined1 *)(albw_frame + 0x27f))
  #define local_60 (*(undefined1 *)(albw_frame + 0x280))
  #define local_5c (*(int *)(albw_frame + 0x284))
  #define local_58 (*(uint * *)(albw_frame + 0x288))
  #define local_54 (*(undefined4 *)(albw_frame + 0x28c))
  #define local_50 (*(int * *)(albw_frame + 0x290))
  #define local_4c (*(undefined1 *)(albw_frame + 0x294))
  #define local_48 (*(int *)(albw_frame + 0x298))
  #define local_44 (*(char * *)(albw_frame + 0x29c))
  #define local_40 (*(int *)(albw_frame + 0x2a0))
  #define local_3c (*(uint *)(albw_frame + 0x2a4))
  #define local_38 (*(undefined4 * *)(albw_frame + 0x2a8))
  iVar17 = DAT_00466318;
  local_90 = 0;
  local_8c = 0;
  local_9c = DAT_004662f4;
  local_98 = DAT_004662f4;
  local_94 = DAT_004662f8;
  local_88 = 0x20;
  uStack_84 = 0;
  local_74 = 0;
  local_80 = 0;
  local_7c = 0;
  local_6c = DAT_004662f4;
  local_68 = s_World_004662fc;
  local_78 = 0;
  local_64 = 7;
  local_63 = 4;
  local_60 = 0;
  local_58 = (uint *)&DAT_004662f0;
  local_4c = 1;
  local_70 = 0;
  local_5c = DAT_004662f4;
  local_1c8 = DAT_004662f4;
  local_1c4 = (uint *)s_Layout_MapTexture__00466304;
  local_54 = 0;
  local_2d4 = 0x100;
  local_62 = 0;
  local_61 = 0;
  local_50 = (int *)0x0;
  uStack_1d1 = 0;
  local_2dc = DAT_00466318;
  local_2d8 = local_2d0;
  (**(code **)(DAT_004662f4 + 8))(&local_1c8);
  uVar8 = 0;
  do {
    if (*(char *)((int)local_1c4 + uVar8) == '\0') {
      if ((int)uVar8 < 0x40001) goto LAB_004660e8;
      break;
    }
    uVar8 = uVar8 + 1;
  } while ((int)uVar8 < 0x40001);
  uVar8 = 0;
LAB_004660e8:
  if (local_2d4 <= (int)uVar8) {
    uVar8 = local_2d4 - 1;
  }
  (**(code **)(local_1c8 + 8))(&local_1c8);
  func_0010E3C4(local_2d0,local_1c4,uVar8);
  puVar6 = local_2d8;
  iVar11 = DAT_0046631c;
  *(undefined1 *)((int)local_2d0 + uVar8) = 0;
  local_2dc = iVar11;
  (**(code **)(iVar11 + 8))(&local_2dc);   /* EU 466128: r0 = sp+4, the string just given its vtable (BOOT_LOG 147) */
  iVar16 = 0;
  do {
    if (*(char *)((int)local_2d8 + iVar16) == '\0') {
      if (iVar16 < 0x40001) goto LAB_00466168;
      break;
    }
    iVar16 = iVar16 + 1;
  } while (iVar16 < 0x40001);
  iVar16 = 0;
LAB_00466168:
  iVar10 = iVar16;
  if (iVar16 < 0) {
    iVar10 = 0;
  }
  (**(code **)(*param_2 + 8))(param_2);
  uVar8 = 0;
  do {
    if (*(char *)(param_2[1] + uVar8) == '\0') {
      if ((int)uVar8 < 0x40001) goto LAB_004661b8;
      break;
    }
    uVar8 = uVar8 + 1;
  } while ((int)uVar8 < 0x40001);
  uVar8 = 0;
LAB_004661b8:
  if (local_2d4 - iVar10 <= (int)uVar8) {
    uVar8 = (local_2d4 - iVar10) - 1;
  }
  if (0 < (int)uVar8) {
    local_a8 = (uint *)((int)puVar6 + iVar10);
    (**(code **)(*param_2 + 8))(param_2);
    func_0010E3C4(local_a8,(uint *)param_2[1],uVar8);
    if (iVar16 < (int)(iVar10 + uVar8)) {
      *(undefined1 *)((int)puVar6 + iVar10 + uVar8) = 0;
    }
  }
  local_1c8 = local_9c;
  local_1c4 = (uint *)&DAT_00466320;
  local_1b8 = 0x100;
  uStack_b5 = 0;
  local_1c0 = iVar17;
  local_1bc = local_1b4;
  (**(code **)(local_9c + 8))(&local_1c8);
  uVar8 = 0;
  do {
    if (*(char *)((int)local_1c4 + uVar8) == '\0') {
      if ((int)uVar8 < 0x40001) goto LAB_0046627c;
      break;
    }
    uVar8 = uVar8 + 1;
  } while ((int)uVar8 < 0x40001);
  uVar8 = 0;
LAB_0046627c:
  if (local_1b8 <= (int)uVar8) {
    uVar8 = local_1b8 - 1;
  }
  (**(code **)(local_1c8 + 8))(&local_1c8);
  func_0010E3C4(local_1b4,local_1c4,uVar8);
  puVar6 = local_1bc;
  *(undefined1 *)((int)local_1b4 + uVar8) = 0;
  local_1c0 = iVar11;
  (**(code **)(iVar11 + 8))(&local_1c0);   /* EU 4662BC: r0 = sp+0x120 (BOOT_LOG 147) */
  iVar17 = 0;
  do {
    if (*(char *)((int)local_1bc + iVar17) == '\0') {
      if (iVar17 < 0x40001) goto LAB_00466334;
      break;
    }
    iVar17 = iVar17 + 1;
  } while (iVar17 < 0x40001);
  iVar17 = 0;
LAB_00466334:
  iVar11 = iVar17;
  if (iVar17 < 0) {
    iVar11 = 0;
  }
  (**(code **)(local_2dc + 8))(&local_2dc);
  uVar8 = 0;
  do {
    if (*(char *)((int)local_2d8 + uVar8) == '\0') {
      if ((int)uVar8 < 0x40001) goto LAB_00466384;
      break;
    }
    uVar8 = uVar8 + 1;
  } while ((int)uVar8 < 0x40001);
  uVar8 = 0;
LAB_00466384:
  if (local_1b8 - iVar11 <= (int)uVar8) {
    uVar8 = (local_1b8 - iVar11) - 1;
  }
  if (0 < (int)uVar8) {
    (**(code **)(local_2dc + 8))(&local_2dc);
    func_0010E3C4((uint *)((int)puVar6 + iVar11),local_2d8,uVar8);
    if (iVar17 < (int)(iVar11 + uVar8)) {
      *(undefined1 *)((int)puVar6 + iVar11 + uVar8) = 0;
    }
  }
  piVar12 = DAT_00466ac4;
  local_50 = func_001E0B94(*DAT_00466ac4,&local_1c0,-1);
  if (local_50 != (int *)0x0) {
    local_62 = 0xc;
    local_58 = local_2d8;
    local_4c = 0;
    puVar3 = func_00504D48(*DAT_00466ac8,(int)&local_98);
    *(undefined4 **)(param_1 + 0x210) = puVar3;
    if (puVar3 != (undefined4 *)0x0) {
      uVar8 = 0;
      piVar12 = *(int **)(*piVar12 + 0x1fa8);
      do {
        uVar4 = (**(code **)(*piVar12 + 0x18))(piVar12,0x8000,0x80);
        puVar3 = (undefined4 *)(param_1 + 0x21c);
        if (uVar8 < 2) {
          puVar3 = (undefined4 *)(param_1 + 0x21c + uVar8 * 0x110);
        }
        *puVar3 = uVar4;
        puVar3 = (undefined4 *)(param_1 + 0x21c);
        if (uVar8 < 2) {
          puVar3 = (undefined4 *)(param_1 + 0x21c + uVar8 * 0x110);
        }
        func_001164F4((undefined4 *)*puVar3,0x8000);
        iVar17 = param_1 + 0x21c;
        if (uVar8 < 2) {
          iVar17 = param_1 + 0x21c + uVar8 * 0x110;
        }
        uVar8 = uVar8 + 1;
        *(undefined4 *)(iVar17 + 4) = 0;
      } while ((int)uVar8 < 2);
      local_40 = *(int *)(*(int *)(param_1 + 0x210) + 0xc4);
      uVar8 = (uint)*(ushort *)(*(int *)(local_40 + 0x30) + 0x28);
      if (uVar8 != 0) {
        puVar6 = (uint *)&UndefinedInstruction;
        iVar17 = func_0010CEE0(uVar8 * 0x18,(int *)0x0,4);
        if (iVar17 != 0) {
          *(int *)(param_1 + 0x448) = iVar17;
          *(uint *)(param_1 + 0x444) = uVar8;
          *(undefined4 *)(param_1 + 0x440) = 0;
        }
        uVar8 = (uint)*(ushort *)(*(int *)(local_40 + 0x30) + 0x28);
        local_3c = 0;
        if (uVar8 != 0) {
LAB_004664fc:
          uVar9 = 0;
          local_b0 = 0;
          uVar13 = 0;
          local_38 = *(undefined4 **)(*(int *)(*(int *)(local_40 + 0x30) + 0x24) + local_3c * 4);
          puVar14 = (uint *)*local_38;
          puVar7 = puVar14 + local_38[1];
          if (puVar14 == puVar7) {
            puVar18 = puVar14;
            bVar20 = false;
          }
          else {
            uVar8 = puVar14[1] & 0xffff;
            puVar18 = puVar14 + ((puVar14[1] << 4) >> 0x18) + 2;
            if (puVar7 <= puVar14 + ((puVar14[1] << 4) >> 0x18) + 2) {
              puVar18 = puVar7;
            }
            puVar6 = puVar14;
            bVar20 = true;
            if ((puVar18 != puVar7) &&
               (bVar20 = true, ((int)puVar18 - (int)puVar14) * 0x20000000 < 0)) {
              puVar18 = puVar18 + 1;
            }
          }
          if (bVar20) {
            do {
              if (uVar8 == 0x82) {
                uVar13 = *puVar6 & 0xffff;
                uVar9 = *puVar6 >> 0x10;
              }
              else if (uVar8 == 0x85) {
                local_b0 = *puVar6 << 3;
              }
              uVar2 = local_b0;
              bVar20 = true;
              if (puVar18 == puVar7) {
                bVar20 = false;
                puVar19 = puVar18;
              }
              else {
                uVar8 = puVar18[1] & 0xffff;
                puVar19 = puVar18 + ((puVar18[1] << 4) >> 0x18) + 2;
                if (puVar7 <= puVar18 + ((puVar18[1] << 4) >> 0x18) + 2) {
                  puVar19 = puVar7;
                }
                puVar6 = puVar18;
                if ((puVar19 != puVar7) && (((int)puVar19 - (int)puVar14) * 0x20000000 < 0)) {
                  puVar19 = puVar19 + 1;
                }
              }
              puVar18 = puVar19;
            } while (bVar20);
            if (local_b0 != 0) {
              iVar17 = *(int *)(param_1 + 0x440);
              if (iVar17 < *(int *)(param_1 + 0x444)) {
                puVar15 = (undefined2 *)(*(int *)(param_1 + 0x448) + iVar17 * 0x18);
                *(int *)(param_1 + 0x440) = iVar17 + 1;
              }
              else {
                puVar15 = (undefined2 *)0x0;
              }
              iVar16 = 0;
              *(undefined1 *)((int)puVar15 + 0xf) = 0;
              *(undefined1 *)(puVar15 + 7) = 0;
              *puVar15 = (short)uVar9;
              puVar15[1] = (short)uVar13;
              iVar17 = DAT_00466acc;
              *(uint *)(puVar15 + 2) = local_b0;
              iVar11 = func_001139F8(iVar17);
              if (((*DAT_00466ad0 & 1) == 0) &&
                 (iVar10 = func_00100174((int *)DAT_00466ad0), iVar10 != 0)) {
                uVar8 = func_001139F8(0x14000000);
                *DAT_00466ad4 = uVar8;
              }
              if (((*DAT_00466ad8 & 1) == 0) &&
                 (iVar10 = func_00100174((int *)DAT_00466ad8), iVar10 != 0)) {
                uVar8 = func_001139F8(0x1bffffff);
                *DAT_00466adc = uVar8;
              }
              if ((uVar2 < *DAT_00466ad4) || (*DAT_00466adc < uVar2)) {
                if (((*DAT_00466ae0 & 1) == 0) &&
                   (iVar10 = func_00100174((int *)DAT_00466ae0), iVar10 != 0)) {
                  uVar8 = func_001139F8(ALBW_VRAM_ADDR(0x1f000000));
                  *DAT_00466ae4 = uVar8;
                }
                if (((*DAT_00466ae8 & 1) == 0) &&
                   (iVar10 = func_00100174((int *)DAT_00466ae8), iVar10 != 0)) {
                  uVar8 = func_001139F8(iVar17);
                  *DAT_00466aec = uVar8;
                }
                if (((*DAT_00466ae4 <= uVar2) && (uVar2 <= *DAT_00466aec)) || (iVar11 + 1U == uVar2)
                   ) {
                  iVar16 = uVar2 + 0x7000000;
                }
              }
              else {
                iVar16 = uVar2 + 0xf4000000;
              }
              /* the block above is the inline inverse of GetPhysicalAddr (001139F8): physical FCRAM
               * / VRAM -> virtual. Natively 001139F8 is the identity (sdk/gfx.c), so the inverse is too;
               * its 3DS range checks turned every host pointer into 0, and 00468030 later ORed a
               * 0x8000-byte bitmap from address 0 (crash entering the castle door, BOOT_LOG 147) */
              iVar16 = (int)uVar2;
              *(int *)(puVar15 + 4) = iVar16;
              local_44 = (char *)local_38[7];
              local_48 = local_9c;
              (**(code **)(local_9c + 8))(&local_48);
              iVar17 = 0;
              do {
                pcVar5 = DAT_004662f8;
                if (local_44[iVar17] == '\0') {
                  bVar21 = SBORROW4(iVar17,0x40000);
                  iVar11 = iVar17 + -0x40000;
                  bVar20 = iVar17 == 0x40000;
                  if (iVar17 < 0x40001) {
                    bVar21 = SBORROW4(2,iVar17);
                    iVar11 = 2 - iVar17;
                    bVar20 = iVar17 == 2;
                  }
                  if (bVar20 || iVar11 < 0 != bVar21) {
                    pcVar5 = local_44 + 2;
                  }
                  break;
                }
                iVar17 = iVar17 + 1;
              } while (iVar17 < 0x40001);
              cVar1 = *pcVar5;
              (**(code **)(local_48 + 8))(&local_48);
              iVar17 = 0;
              do {
                pcVar5 = DAT_004662f8;
                if (local_44[iVar17] == '\0') {
                  bVar21 = SBORROW4(iVar17,0x40000);
                  iVar11 = iVar17 + -0x40000;
                  bVar20 = iVar17 == 0x40000;
                  if (iVar17 < 0x40001) {
                    bVar21 = SBORROW4(1,iVar17);
                    iVar11 = 1 - iVar17;
                    bVar20 = iVar17 == 1;
                  }
                  if (bVar20 || iVar11 < 0 != bVar21) {
                    pcVar5 = local_44 + 1;
                  }
                  break;
                }
                iVar17 = iVar17 + 1;
              } while (iVar17 < 0x40001);
              *(char *)(puVar15 + 6) = (*pcVar5 + -0x30) * '\n' + -0x30 + cVar1;
              (**(code **)(local_48 + 8))(&local_48);
              iVar17 = 0;
              do {
                if (local_44[iVar17] == '\0') {
                  iVar11 = iVar17 + -0x40000;
                  if (0x40000 >= iVar17) {
                    iVar11 = iVar17;
                  }
                  pcVar5 = DAT_004662f8;
                  if (iVar11 < 0 != (0x40000 < iVar17 && SBORROW4(iVar17,0x40000)))
                  goto LAB_00466988;
                  break;
                }
                iVar17 = iVar17 + 1;
              } while (iVar17 < 0x40001);
              pcVar5 = local_44;
LAB_00466988:
              if (*pcVar5 == 'B') {
                *(char *)(puVar15 + 6) = -*(char *)(puVar15 + 6);
              }
              (**(code **)(local_48 + 8))(&local_48);
              iVar17 = 0;
              do {
                puVar6 = (uint *)(uint)(byte)local_44[iVar17];
                if (puVar6 == (uint *)0x0) {
                  if ((iVar17 < 0x40001) && (3 < iVar17)) {
                    *(undefined1 *)((int)puVar15 + 0xf) = 1;
                    (**(code **)(local_48 + 8))(&local_48);
                    iVar17 = 0;
                    goto LAB_00466a04;
                  }
                  break;
                }
                iVar17 = iVar17 + 1;
              } while (iVar17 < 0x40001);
            }
          }
          goto LAB_00466a9c;
        }
      }
    }
  }
  return;
  while (iVar17 = iVar17 + 1, iVar17 < 0x40001) {
LAB_00466a04:
    pcVar5 = DAT_004662f8;
    if (local_44[iVar17] == '\0') {
      bVar21 = SBORROW4(iVar17,0x40000);
      iVar11 = iVar17 + -0x40000;
      bVar20 = iVar17 == 0x40000;
      if (iVar17 < 0x40001) {
        bVar21 = SBORROW4(6,iVar17);
        iVar11 = 6 - iVar17;
        bVar20 = iVar17 == 6;
      }
      if (bVar20 || iVar11 < 0 != bVar21) {
        pcVar5 = local_44 + 6;
      }
      break;
    }
  }
  cVar1 = *pcVar5;
  (**(code **)(local_48 + 8))(&local_48);
  iVar17 = 0;
  do {
    puVar6 = (uint *)(uint)(byte)local_44[iVar17];
    pcVar5 = DAT_004662f8;
    if (puVar6 == (uint *)0x0) {
      bVar21 = SBORROW4(iVar17,0x40000);
      iVar11 = iVar17 + -0x40000;
      bVar20 = iVar17 == 0x40000;
      if (iVar17 < 0x40001) {
        bVar21 = SBORROW4(5,iVar17);
        iVar11 = 5 - iVar17;
        bVar20 = iVar17 == 5;
      }
      if (bVar20 || iVar11 < 0 != bVar21) {
        pcVar5 = local_44 + 5;
      }
      break;
    }
    iVar17 = iVar17 + 1;
  } while (iVar17 < 0x40001);
  *(char *)((int)puVar15 + 0xd) = (*pcVar5 + -0x30) * '\n' + -0x30 + cVar1;
LAB_00466a9c:
  local_3c = local_3c + 1;
  uVar8 = (uint)*(ushort *)(*(int *)(local_40 + 0x30) + 0x28);
  if (uVar8 <= local_3c) {
    return;
  }
  goto LAB_004664fc;
}

