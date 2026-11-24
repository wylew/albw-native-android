/* USA 002D3B08 (EU 002D3AE4): an item's energy cost on the power meter object (Link's status block +0x4100):
 * timer +0x1C = 60, meter +0x10 -= 300.0 (EU: vldr / vsub / vstr, cost from the literal pool).
 * Cheat "Items cost no energy" (Action Replay EUR: the vsub replaced by a nop at EU 2D3AF4): the
 * timer is still set, the meter is not lowered (platform/cheats.c). Confidence: high. */
#include <stdint.h>
/* cheat switches (platform/cheats.c, Esc menu > Cheats); weak: builds without the platform layer */
extern uint32_t albw_cheat_code __attribute__((weak));
#define ALBW_CHEAT(bit) (&albw_cheat_code && (albw_cheat_code & (bit)))
#define CHEAT_ITEM_ENERGY 4u

/* returns meter: on the returns-this list (r0 is untouched on ARM; callers may use it) */
uint8_t *func_002D3B08(uint8_t *meter) {
    *(int32_t *)(meter + 0x1C) = 60;
    if (!ALBW_CHEAT(CHEAT_ITEM_ENERGY)) *(float *)(meter + 0x10) -= 300.0f;
    return meter;
}
