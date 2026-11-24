/* Cheats (included by platform.c after hud_overlay.c): Esc menu > Cheats.
 *
 * Value cheats, after the EUR Action Replay list (JourneyOver/CTRPF-AR-CHEAT-CODES, title
 * 00040000000EC400). Those codes write absolute 3DS addresses (linear heap 0x148B72B8 + offset);
 * the port's memory is laid out differently, but the fields are the same: they are Link's status
 * block, embedded in the player actor at +0x40C (item pickups call the rupee adder
 * 00255AC4(link + 0x40C, amount); 00255AC4 keeps the rupees at +0xF0, clamped to 9999).
 * Offsets in the block (AR address - 0x148B72B8; checked on the user's save: 22 rupees, 6 / 12
 * quarter hearts, power 600.0):
 *   +0x24  u8  small keys          +0xF0  u16 rupees (max 9999)
 *   +0xF9  u8  monster tails       +0xFA  u8  monster guts      +0xFB  u8 monster horns (max 99)
 *   +0xFC  u8  Maiamais (100)      +0x18C u8  hearts (quarters) +0x18D u8 heart maximum
 *   +0x4110 f32 power meter (600.0 full without the upgrade; the AR code wrote its upper half)
 * Link: the player position pointer *(*data_0070FB60 + 0x1C) (00520FC0, as the route autopilot)
 * is the actor's +0x3D0; the actor is accepted only if its profile name (+0x2D0 -> +0x10) is
 * "PlayerObject". ALBW_CHEATS=<key,...> (debug: rupees materials maiamai hearts keys power
 * invincible money items painting) turns cheats on at the start. Applied once per presented frame (as the AR engine), on the game thread,
 * between frames. Session only: every start has all cheats off (layout.cfg keeps none).
 * Code cheats (the AR codes rewrite ARM instructions; here the recompiled functions check
 * albw_cheat_code, set from the toggles every frame):
 *   1 Invincible        native/game/func_00149BD4.c (hearts adder: always the maximum)
 *   2 Money ...         native/compilefix/func_00255AC4.c / func_00255AB4.c (subtract branch skipped)
 *   4 Items: no energy  native/game/func_002D3B08.c .. func_002D3C54.c (ten item costs)
 *   8 Painting: no energy  native/compilefix/func_002D3CD4.c (the wall-merge drain) */
enum { CH_RUPEES, CH_MATERIALS, CH_MAIAMAI, CH_HEARTS, CH_KEYS, CH_POWER,
       CH_INVINCIBLE, CH_MONEY, CH_ITEM_ENERGY, CH_PAINT_ENERGY, CH_COUNT };
uint32_t albw_cheat_code;   /* code cheats: bits read by the hooked game functions (above) */
static int g_cheat[CH_COUNT];
static int g_cheat_reset_rupees;     /* one-shot */
static float g_cheat_power_max = 600.0f;

const float *albw_debug_player_pos(void);   /* runtime/debug_spawn.c */

static uint8_t *cheat_status_block(void) {
    const float *pos = albw_debug_player_pos();
    if (!pos) return NULL;
    uint8_t *link = (uint8_t *)pos - 0x3D0;
    uint32_t prof = *(uint32_t *)(link + 0x2D0);
    if (!prof) return NULL;
    const char *name = (const char *)(uintptr_t)*(uint32_t *)(uintptr_t)(prof + 0x10);
    if (!name || strcmp(name, "PlayerObject")) return NULL;
    return link + 0x40C;
}

static const char *const g_cheat_keys[CH_COUNT] = {"rupees", "materials", "maiamai", "hearts", "keys", "power",
                                                  "invincible", "money", "items", "painting"};
static __attribute__((noinline)) void cheats_apply(void) {
    static int env_done;
    if (!env_done) {   /* ALBW_CHEATS=<key>[,<key>...] (debug, tests): cheats on from the start */
        env_done = 1;
        const char *e = getenv("ALBW_CHEATS");
        for (int i = 0; e && i < CH_COUNT; i++)
            if (strstr(e, g_cheat_keys[i])) { g_cheat[i] = 1; fprintf(stderr, "cheats: %s on (ALBW_CHEATS)\n", g_cheat_keys[i]); }
    }
    albw_cheat_code = (g_cheat[CH_INVINCIBLE] ? 1u : 0) | (g_cheat[CH_MONEY] ? 2u : 0) |
                      (g_cheat[CH_ITEM_ENERGY] ? 4u : 0) | (g_cheat[CH_PAINT_ENERGY] ? 8u : 0);
    int any = g_cheat_reset_rupees;
    for (int i = 0; i < CH_INVINCIBLE; i++) any |= g_cheat[i];
    if (!any) return;
    uint8_t *s = cheat_status_block();
    if (!s) return;
    if (g_cheat_reset_rupees) { *(uint16_t *)(s + 0xF0) = 0; g_cheat_reset_rupees = 0; }
    if (g_cheat[CH_RUPEES]) *(uint16_t *)(s + 0xF0) = 9999;
    if (g_cheat[CH_MATERIALS]) s[0xF9] = s[0xFA] = s[0xFB] = 99;
    if (g_cheat[CH_MAIAMAI]) s[0xFC] = 100;
    if (g_cheat[CH_HEARTS] && s[0x18D]) s[0x18C] = s[0x18D];
    if (g_cheat[CH_KEYS]) s[0x24] = 5;
    if (g_cheat[CH_POWER]) {   /* the highest value seen (full), at least 600.0 */
        float *p = (float *)(s + 0x4110);
        if (*p == *p && *p > g_cheat_power_max && *p < 100000.0f) g_cheat_power_max = *p;
        *p = g_cheat_power_max;
    }
}

static const char *const g_cheat_names[CH_COUNT] = {
    "Max rupees (9999)", "Max monster tails / guts / horns", "Max Maiamais (100)",
    "Full hearts", "5 small keys", "Infinite power meter",
    "Invincible", "Money doesn't decrease", "Items cost no energy", "Painting costs no energy"};
