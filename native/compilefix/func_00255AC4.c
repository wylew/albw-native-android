/* compilefix: USA 00255AC4, the Ghidra text unchanged except for the cheat "Money doesn't decrease"
 * (Action Replay EUR: E0255B14 00000014 + five nops = the subtract branch of the rupee adder EU
 * 00255AEC): with the cheat on, a negative amount leaves the rupees (param_1 + 0xf0) as they are; the
 * counter-and-sound call before it and everything else run as before (platform/cheats.c).
 * The sibling 00255AB4 / 00255AC4 (same branch through a wrapper) is hooked the same way. */
// MAPFIX-INTEGRATED v1
#include "ghidra_types.h"
#include <stdint.h>
#define CHEAT_MONEY 2u
/* cheat switches (platform/cheats.c, Esc menu > Cheats); weak: builds without the platform layer */
extern uint32_t albw_cheat_code __attribute__((weak));
#define ALBW_CHEAT(bit) (&albw_cheat_code && (albw_cheat_code & (bit)))

extern int func_001E34F0(int param_1,undefined4 param_2,int param_3);
extern int *DAT_00255ba4;
extern uint DAT_00255ba8;
extern uint DAT_00255bac;

// USA 00255AC4 <- EU 00255AEC (Ghidra, non-matching)
// MAPFIX: tools/ghidra_name_fixes.json applied (do not apply again)
// GLOBAL DAT_00255ba4 int *
// GLOBAL DAT_00255ba8 uint
// GLOBAL DAT_00255bac uint

undefined4 func_00255AC4(int param_1,int param_2,int param_3)

{
  int iVar1;
  uint uVar2;
  uint uVar3;
  uint uVar4;
  
  if (param_2 != 0) {
    func_001E34F0(*(int *)(*DAT_00255ba4 + 0x130),param_2,param_3);
    uVar2 = DAT_00255ba8;
    if (param_2 < 0) {
      if (ALBW_CHEAT(CHEAT_MONEY)) goto cheat_skip;   /* "Money doesn't decrease" */
      iVar1 = (uint)*(ushort *)(param_1 + 0xf0) + param_2;
      if (iVar1 < 1) {
        iVar1 = 0;
      }
      *(short *)(param_1 + 0xf0) = (short)iVar1;
    }
    else {
      uVar3 = (uint)*(ushort *)(param_1 + 0xf0) + param_2;
      if (uVar3 < DAT_00255ba8) {
        uVar4 = *(int *)(param_1 + 0x108) + param_2;
        uVar2 = DAT_00255bac;
        if (uVar4 <= DAT_00255bac) {
          uVar2 = uVar4;
        }
        *(uint *)(param_1 + 0x108) = uVar4;
        *(uint *)(param_1 + 0x108) = uVar2;
        *(short *)(param_1 + 0xf0) = (short)uVar3;
      }
      else {
        uVar4 = (DAT_00255ba8 - *(ushort *)(param_1 + 0xf0)) + *(int *)(param_1 + 0x108);
        uVar3 = DAT_00255bac;
        if (uVar4 <= DAT_00255bac) {
          uVar3 = uVar4;
        }
        *(uint *)(param_1 + 0x108) = uVar4;
        *(uint *)(param_1 + 0x108) = uVar3;
        *(short *)(param_1 + 0xf0) = (short)uVar2;
      }
    }
cheat_skip:
    if (*(char *)(param_1 + 0xf8) != '\0') {
      *(int *)(param_1 + 0xf4) = *(int *)(param_1 + 0xf4) + param_2;
    }
  }
  return 1;
}

