/* USA 00149BD4 (EU 00149BD4): add `delta` to a u8 counter with a u8 limit (Link's hearts: status
 * block +0x18C / +0x18D, quarter hearts), clamped to 0..limit. Same as the decompiled source
 * (gpt15 v1). Cheat "Invincible" (Action Replay EUR: 00149BEC E1510002 / 00149BF0 B1A01002, the
 * clamp rewritten to "below the limit -> the limit"): the counter is set to its limit on every
 * change, so damage never lowers it. Confidence: high (EU 00149BD4..00149C04). */
#include <stdint.h>
/* cheat switches (platform/cheats.c, Esc menu > Cheats); weak: builds without the platform layer */
extern uint32_t albw_cheat_code __attribute__((weak));
#define ALBW_CHEAT(bit) (&albw_cheat_code && (albw_cheat_code & (bit)))
#define CHEAT_INVINCIBLE 1u

typedef struct { uint8_t value, limit; } Counter149BD4;
/* returns p: on the returns-this list (r0 is untouched on ARM; callers may use it) */
Counter149BD4 *func_00149BD4(Counter149BD4 *p, int32_t delta) {
    if (!delta) return p;
    int32_t n = p->value + delta;
    if (ALBW_CHEAT(CHEAT_INVINCIBLE)) n = p->limit;
    else if (n < 0) n = 0;
    else if (n > p->limit) n = p->limit;
    p->value = (uint8_t)n;
    return p;
}
