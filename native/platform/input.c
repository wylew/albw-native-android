/* Native input: keyboard + SDL3 game controllers -> 3DS inputs. Format and API: input.h. */
#include "input.h"
#include "platform.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CPAD_MAX 156
#define MAX_PADS 16
#define MAX_COMBO 3
#define MAX_ALT 6
#define STICK_PRESS 16384           /* stick half-axis bound to a digital action: past 50% */

/* ------------------------------------------------------------------ bindings */
enum { EL_KEY, EL_BTN, EL_LABEL, EL_AXIS, EL_AUTO };
typedef struct { uint8_t kind; int8_t sign; int16_t idx; } in_Elem;
typedef struct { in_Elem e[MAX_COMBO]; uint8_t n; } in_Alt;
typedef struct { in_Alt a[MAX_ALT]; uint8_t n; } in_Bind;
typedef struct { uint64_t key[(SDL_SCANCODE_COUNT + 63) / 64]; uint64_t btn; } in_Consumed;

static const char *const in_ACTION_NAMES[IN_ACTION_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "START", "SELECT",
    "DUP", "DDOWN", "DLEFT", "DRIGHT",
    "CPAD_UP", "CPAD_DOWN", "CPAD_LEFT", "CPAD_RIGHT",
    "CSTICK_UP", "CSTICK_DOWN", "CSTICK_LEFT", "CSTICK_RIGHT",
    "MENU", "QUIT", "SCREEN",
};
/* 3DS button bit of each action (0: analog / frontend) */
static const uint32_t in_ACTION_BITS[IN_ACTION_COUNT] = {
    KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_START, KEY_SELECT,
    KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT,
};

static const struct { const char *name; SDL_GamepadButton b; } in_BUTTONS[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST}, {"north", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", SDL_GAMEPAD_BUTTON_BACK}, {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", SDL_GAMEPAD_BUTTON_START}, {"leftstick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rightstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"leftshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rightshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"dpup", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpdown", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"dpleft", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpright", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"misc1", SDL_GAMEPAD_BUTTON_MISC1},
    {"paddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"paddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"paddle3", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"paddle4", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD}, {"misc2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", SDL_GAMEPAD_BUTTON_MISC3}, {"misc4", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", SDL_GAMEPAD_BUTTON_MISC5}, {"misc6", SDL_GAMEPAD_BUTTON_MISC6},
};
static const char *const in_AXES[SDL_GAMEPAD_AXIS_COUNT] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};
static const char *const in_LABELS[4] = {"label_a", "label_b", "label_x", "label_y"};

static int in_is_trigger(int axis) {
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
}

static const char *in_button_name(int b) {
    for (size_t i = 0; i < SDL_arraysize(in_BUTTONS); i++)
        if ((int)in_BUTTONS[i].b == b) return in_BUTTONS[i].name;
    return NULL;
}

static void in_elem_name(const in_Elem *e, char *out, size_t n) {
    const char *s = NULL;
    switch (e->kind) {
    case EL_KEY:
        s = e->idx == SDL_SCANCODE_COMMA ? "Comma" : SDL_GetScancodeName((SDL_Scancode)e->idx);
        break;
    case EL_BTN: s = in_button_name(e->idx); break;
    case EL_LABEL: s = in_LABELS[e->idx]; break;
    case EL_AUTO: s = "auto"; break;
    case EL_AXIS:
        if (in_is_trigger(e->idx)) s = in_AXES[e->idx];
        else {
            snprintf(out, n, "%c%s", e->sign > 0 ? '+' : '-', in_AXES[e->idx]);
            return;
        }
        break;
    }
    snprintf(out, n, "%s", s && *s ? s : "?");
}

static void in_bind_text(const in_Bind *b, char *out, size_t n) {
    size_t len = 0;
    out[0] = 0;
    if (!b->n) {
        snprintf(out, n, "none");
        return;
    }
    for (int i = 0; i < b->n; i++)
        for (int k = 0; k < b->a[i].n; k++) {
            char e[48];
            in_elem_name(&b->a[i].e[k], e, sizeof e);
            len += snprintf(out + len, len < n ? n - len : 0, "%s%s", k ? "&" : i ? ", " : "", e);
            if (len >= n) return;
        }
}

static char *in_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int in_parse_elem(const char *t, int keyboard, InAction a, in_Elem *e, char *err, size_t el) {
    memset(e, 0, sizeof *e);
    if (keyboard) {
        SDL_Scancode sc = !strcasecmp(t, "Comma") ? SDL_SCANCODE_COMMA : SDL_GetScancodeFromName(t);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            snprintf(err, el, "unknown key \"%s\"", t);
            return -1;
        }
        e->kind = EL_KEY;
        e->idx = (int16_t)sc;
        return 0;
    }
    if (!strcasecmp(t, "auto")) {
        if (a > IN_Y) {
            snprintf(err, el, "\"auto\" is only valid for A, B, X and Y");
            return -1;
        }
        e->kind = EL_AUTO;
        return 0;
    }
    for (size_t i = 0; i < SDL_arraysize(in_BUTTONS); i++)
        if (!strcasecmp(t, in_BUTTONS[i].name)) {
            e->kind = EL_BTN;
            e->idx = (int16_t)in_BUTTONS[i].b;
            return 0;
        }
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(t, in_LABELS[i])) {
            e->kind = EL_LABEL;
            e->idx = (int16_t)i;
            return 0;
        }
    int sign = 0;
    if (*t == '+' || *t == '-') sign = *t++ == '+' ? 1 : -1;
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
        if (!strcasecmp(t, in_AXES[i])) {
            if (in_is_trigger(i) ? sign < 0 : !sign) break;
            e->kind = EL_AXIS;
            e->idx = (int16_t)i;
            e->sign = (int8_t)(sign ? sign : 1);
            return 0;
        }
    snprintf(err, el, "unknown gamepad element \"%s%s\"", sign > 0 ? "+" : sign < 0 ? "-" : "", t);
    return -1;
}

static int in_parse_bind(const char *text, int keyboard, InAction a, in_Bind *out, char *err, size_t el) {
    char buf[IN_BIND_LEN * 2];
    memset(out, 0, sizeof *out);
    snprintf(buf, sizeof buf, "%s", text ? text : "");
    char *s = in_trim(buf);
    if (!*s || !strcasecmp(s, "none")) return 0;
    char *save = NULL;
    for (char *alt = strtok_r(s, ",", &save); alt; alt = strtok_r(NULL, ",", &save)) {
        alt = in_trim(alt);
        if (!*alt) continue;
        if (out->n == MAX_ALT) {
            snprintf(err, el, "more than %d alternatives", MAX_ALT);
            return -1;
        }
        in_Alt *A = &out->a[out->n++];
        char *save2 = NULL;
        for (char *t = strtok_r(alt, "&", &save2); t; t = strtok_r(NULL, "&", &save2)) {
            t = in_trim(t);
            if (!*t) continue;
            if (A->n == MAX_COMBO) {
                snprintf(err, el, "more than %d elements in a combination", MAX_COMBO);
                return -1;
            }
            if (in_parse_elem(t, keyboard, a, &A->e[A->n], err, el)) return -1;
            A->n++;
        }
        for (int k = 0; k < A->n && A->n > 1; k++)
            if (A->e[k].kind == EL_AUTO) {
                snprintf(err, el, "\"auto\" cannot be combined");
                return -1;
            }
        if (!A->n) out->n--;
    }
    return 0;
}

/* ------------------------------------------------------------------ state */
typedef struct {
    int layout, circle_pad, c_stick, dz, cdz, outer, trig;
    int has_override;
    in_Bind bind[IN_ACTION_COUNT];
} in_Profile;

typedef struct {
    SDL_Gamepad *gp;
    SDL_JoystickID id;
    char name[128], guid[33];
    int fallback, has_accel, has_gyro, sensors_on;
    SDL_GamepadButton label_btn[4];   /* face button printed A, B, X, Y */
    int8_t axis_state[SDL_GAMEPAD_AXIS_COUNT];   /* -1/0/+1: half-axis pressed (in_capture, activity) */
    in_Profile p;
} in_Pad;

static pthread_mutex_t in_mu = PTHREAD_MUTEX_INITIALIZER;
static InputConfig g_input_cfg;
static int in_cfg_ready;
static in_Bind in_kb_bind[IN_ACTION_COUNT];
static in_Pad in_pads[MAX_PADS];
static int in_npads;
static SDL_JoystickID in_active_id;
static SDL_JoystickID in_fallback_ids[MAX_PADS];
static int in_nfallback;
static char in_path_default[512], in_path_loaded[512];
static struct timespec in_mtime_loaded;
static char in_last_err[1024], in_err_ret[1024];
static int in_menu_prev, in_quit_prev;   /* controllers: edge detection on held state */
static int in_kb_menu_ev, in_kb_quit_ev, in_kb_screen_ev, in_screen_prev; /* keyboard: from key-down events (keys a frontend layer
                                      consumed before input_handle_event() never count) */
static unsigned in_frame_count;
static char in_cap_name[48];
static uint32_t in_cap_id;
static int in_cap_new;

static void in_err_add(const char *fmt, ...) {
    size_t len = strlen(in_last_err);
    if (len >= sizeof in_last_err - 2) return;
    if (len) len += snprintf(in_last_err + len, sizeof in_last_err - len, "; ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(in_last_err + len, len < sizeof in_last_err ? sizeof in_last_err - len : 0, fmt, ap);
    va_end(ap);
}

void input_defaults(InputConfig *c) {
    memset(c, 0, sizeof *c);
    c->multi = IN_MULTI_ACTIVE;
    c->motion = 1;
    c->fallback = 1;
    static const char *const KEYS[IN_ACTION_COUNT] = {
        [IN_A] = "X", [IN_B] = "Z", [IN_X] = "S", [IN_Y] = "A", [IN_L] = "Q", [IN_R] = "W",
        [IN_START] = "Return", [IN_SELECT] = "Right Shift, Backspace",
        [IN_DUP] = "I", [IN_DDOWN] = "K", [IN_DLEFT] = "J", [IN_DRIGHT] = "L",
        [IN_CPAD_UP] = "Up", [IN_CPAD_DOWN] = "Down", [IN_CPAD_LEFT] = "Left", [IN_CPAD_RIGHT] = "Right",
#ifdef __ANDROID__
        [IN_MENU] = "F1", [IN_QUIT] = "Escape, AC Back", [IN_SCREEN] = "Tab",   /* Back: the Esc menu */
#else
        [IN_MENU] = "F1", [IN_QUIT] = "Escape", [IN_SCREEN] = "Tab",
#endif
    };
    static const char *const PAD[IN_ACTION_COUNT] = {
        [IN_A] = "auto", [IN_B] = "auto", [IN_X] = "auto", [IN_Y] = "auto",
        [IN_L] = "leftshoulder", [IN_R] = "rightshoulder", [IN_ZL] = "lefttrigger", [IN_ZR] = "righttrigger",
        [IN_START] = "start", [IN_SELECT] = "back",
        [IN_DUP] = "dpup", [IN_DDOWN] = "dpdown", [IN_DLEFT] = "dpleft", [IN_DRIGHT] = "dpright",
        [IN_MENU] = "guide, leftstick&rightstick", [IN_SCREEN] = "rightstick",
    };
    for (int a = 0; a < IN_ACTION_COUNT; a++) {
        snprintf(c->key[a], IN_BIND_LEN, "%s", KEYS[a] ? KEYS[a] : "");
        snprintf(c->pad.bind[a], IN_BIND_LEN, "%s", PAD[a] ? PAD[a] : "");
    }
    c->pad.layout = IN_LAYOUT_POSITIONAL;
    c->pad.circle_pad = IN_STICK_LEFT;
    c->pad.c_stick = IN_STICK_RIGHT;
    c->pad.stick_deadzone = 15;
    c->pad.cstick_deadzone = 15;
    c->pad.stick_outer = 95;
    c->pad.trigger_threshold = 50;
}

static void in_unset_profile(InputPadProfile *p) {
    memset(p, 0, sizeof *p);
    p->layout = p->circle_pad = p->c_stick = IN_UNSET;
    p->stick_deadzone = p->cstick_deadzone = p->stick_outer = p->trigger_threshold = IN_UNSET;
}

static int in_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void in_sanitize(InputConfig *c) {
    InputPadProfile *p = &c->pad;
    if (p->layout != IN_LAYOUT_LABELS) p->layout = IN_LAYOUT_POSITIONAL;
    if (p->circle_pad < 0 || p->circle_pad > 2) p->circle_pad = IN_STICK_LEFT;
    if (p->c_stick < 0 || p->c_stick > 2) p->c_stick = IN_STICK_RIGHT;
    p->stick_deadzone = in_clampi(p->stick_deadzone, 0, 99);
    p->cstick_deadzone = in_clampi(p->cstick_deadzone, 0, 99);
    p->stick_outer = in_clampi(p->stick_outer, 1, 100);
    p->trigger_threshold = in_clampi(p->trigger_threshold, 1, 100);
    c->n_overrides = in_clampi(c->n_overrides, 0, IN_MAX_OVERRIDES);
    for (int i = 0; i < c->n_overrides; i++) {
        InputPadProfile *o = &c->overrides[i];
        if (o->stick_deadzone != IN_UNSET) o->stick_deadzone = in_clampi(o->stick_deadzone, 0, 99);
        if (o->cstick_deadzone != IN_UNSET) o->cstick_deadzone = in_clampi(o->cstick_deadzone, 0, 99);
        if (o->stick_outer != IN_UNSET) o->stick_outer = in_clampi(o->stick_outer, 1, 100);
        if (o->trigger_threshold != IN_UNSET) o->trigger_threshold = in_clampi(o->trigger_threshold, 1, 100);
        for (char *g = o->guid; *g; g++) *g = (char)tolower((unsigned char)*g);
    }
    c->multi = c->multi == IN_MULTI_ANY;
    c->motion = !!c->motion;
    c->fallback = !!c->fallback;
}

static void in_ensure_cfg(void) {
    if (in_cfg_ready) return;
    input_defaults(&g_input_cfg);
    for (int a = 0; a < IN_ACTION_COUNT; a++) in_parse_bind(g_input_cfg.key[a], 1, a, &in_kb_bind[a], in_last_err, 0);
    in_cfg_ready = 1;
}

/* ------------------------------------------------------------------ per-controller compile */
static const InputPadProfile *in_find_override(const InputConfig *c, const char *guid) {
    for (int i = 0; i < c->n_overrides; i++)
        if (!strcasecmp(c->overrides[i].guid, guid)) return &c->overrides[i];
    return NULL;
}

static void in_resolve_labels(in_Pad *p) {
    static const SDL_GamepadButton XBOX[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                              SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; i++) p->label_btn[i] = XBOX[i];
    for (int i = 0; i < 4; i++) {
        switch (SDL_GetGamepadButtonLabel(p->gp, XBOX[i])) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: p->label_btn[0] = XBOX[i]; break;
        case SDL_GAMEPAD_BUTTON_LABEL_B: p->label_btn[1] = XBOX[i]; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: p->label_btn[2] = XBOX[i]; break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y: p->label_btn[3] = XBOX[i]; break;
        default: break;
        }
    }
}

static void in_compile_pad(in_Pad *p) {
    const InputPadProfile *d = &g_input_cfg.pad, *o = in_find_override(&g_input_cfg, p->guid);
    in_Profile *P = &p->p;
#define PICK(field) (o && o->field != IN_UNSET ? o->field : d->field)
    P->layout = PICK(layout);
    P->circle_pad = PICK(circle_pad);
    P->c_stick = PICK(c_stick);
    P->dz = PICK(stick_deadzone);
    P->cdz = PICK(cstick_deadzone);
    P->outer = PICK(stick_outer);
    P->trig = PICK(trigger_threshold);
#undef PICK
    P->has_override = o != NULL;
    in_resolve_labels(p);
    static const SDL_GamepadButton POS[4] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH,
                                             SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST};
    char err[128];
    for (int a = 0; a < IN_ACTION_COUNT; a++) {
        const char *t = o && o->bind[a][0] ? o->bind[a] : d->bind[a];
        if (in_parse_bind(t, 0, a, &P->bind[a], err, sizeof err)) P->bind[a].n = 0;   /* validated before */
        for (int i = 0; i < P->bind[a].n; i++)
            for (int k = 0; k < P->bind[a].a[i].n; k++) {
                in_Elem *e = &P->bind[a].a[i].e[k];
                if (e->kind == EL_AUTO) {
                    e->kind = EL_BTN;
                    e->idx = (int16_t)(P->layout == IN_LAYOUT_LABELS ? p->label_btn[a] : POS[a]);
                } else if (e->kind == EL_LABEL) {
                    e->kind = EL_BTN;
                    e->idx = (int16_t)p->label_btn[e->idx];
                }
            }
    }
}

static void in_pad_sensors(in_Pad *p) {
    int on = g_input_cfg.motion && (p->has_accel || p->has_gyro);
    if (on == p->sensors_on) return;
    if (p->has_accel) SDL_SetGamepadSensorEnabled(p->gp, SDL_SENSOR_ACCEL, on);
    if (p->has_gyro) SDL_SetGamepadSensorEnabled(p->gp, SDL_SENSOR_GYRO, on);
    p->sensors_on = on;
}

static void in_apply_config(void) {
    for (int a = 0; a < IN_ACTION_COUNT; a++) {
        char err[128];
        if (in_parse_bind(g_input_cfg.key[a], 1, a, &in_kb_bind[a], err, sizeof err)) in_kb_bind[a].n = 0;
    }
    for (int i = 0; i < in_npads; i++) {
        in_compile_pad(&in_pads[i]);
        in_pad_sensors(&in_pads[i]);
    }
}

/* ------------------------------------------------------------------ hotplug */
static in_Pad *in_find_pad(SDL_JoystickID id) {
    for (int i = 0; i < in_npads; i++)
        if (in_pads[i].id == id) return &in_pads[i];
    return NULL;
}

/* A joystick without a gamepad mapping: give it a generic one. Button order of most generic USB
 * (DirectInput-style) in_pads: 0 = top, 1 = right, 2 = bottom, 3 = left face button, then L1 R1 L2
 * R2 Select Start L3 R3; axes 0/1 left stick, 2/3 right stick; hat 0 (or axes 4/5) D-pad. */
static void in_add_fallback_mapping(SDL_JoystickID id) {
    if (!g_input_cfg.fallback || SDL_IsGamepad(id)) return;
    switch (SDL_GetJoystickTypeForID(id)) {
    case SDL_JOYSTICK_TYPE_WHEEL: case SDL_JOYSTICK_TYPE_FLIGHT_STICK: case SDL_JOYSTICK_TYPE_THROTTLE:
    case SDL_JOYSTICK_TYPE_GUITAR: case SDL_JOYSTICK_TYPE_DRUM_KIT: case SDL_JOYSTICK_TYPE_DANCE_PAD:
        return;
    default: break;
    }
    SDL_Joystick *j = SDL_OpenJoystick(id);
    if (!j) return;
    int nb = SDL_GetNumJoystickButtons(j), na = SDL_GetNumJoystickAxes(j), nh = SDL_GetNumJoystickHats(j);
    char guid[33], name[128], map[512];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof guid);
    snprintf(name, sizeof name, "%s", SDL_GetJoystickName(j) ? SDL_GetJoystickName(j) : "Joystick");
    SDL_CloseJoystick(j);
    for (char *c = name; *c; c++)
        if (*c == ',') *c = ' ';
    if (nb < 2 && na < 2 && nh < 1) return;      /* not a game pad */
    size_t len = 0;
    if (g_input_cfg.fallback_mapping[0]) {
        len = snprintf(map, sizeof map, "%s", g_input_cfg.fallback_mapping);
    } else {
        /* SDL mapping names: a = south, b = east, x = west, y = north */
        static const char *const B[] = {"y", "b", "a", "x", "leftshoulder", "rightshoulder", "lefttrigger",
                                        "righttrigger", "back", "start", "leftstick", "rightstick", "guide"};
        for (int i = 0; i < nb && i < (int)SDL_arraysize(B); i++)
            len += snprintf(map + len, sizeof map - len, "%s:b%d,", B[i], i);
        if (na >= 2) len += snprintf(map + len, sizeof map - len, "leftx:a0,lefty:a1,");
        if (na >= 4) len += snprintf(map + len, sizeof map - len, "rightx:a2,righty:a3,");
        if (nh >= 1)
            len += snprintf(map + len, sizeof map - len, "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,");
        else if (na >= 6)
            len += snprintf(map + len, sizeof map - len, "dpleft:-a4,dpright:+a4,dpup:-a5,dpdown:+a5,");
    }
    char full[768];
    snprintf(full, sizeof full, "%s,%s,%s", guid, name, map);
    if (SDL_AddGamepadMapping(full) < 0) {
        fprintf(stderr, "input: fallback mapping for \"%s\" rejected: %s\n", name, SDL_GetError());
        return;
    }
    if (in_nfallback < MAX_PADS) in_fallback_ids[in_nfallback++] = id;
    fprintf(stderr, "input: \"%s\" has no SDL gamepad mapping: using a generic one (%d buttons, %d axes, "
            "%d hats; add a line to gamecontrollerdb.txt to fix)\n", name, nb, na, nh);
}

static void in_pad_open(SDL_JoystickID id) {
    if (in_find_pad(id) || in_npads == MAX_PADS || !SDL_IsGamepad(id)) return;
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) {
        fprintf(stderr, "input: cannot open controller %u: %s\n", (unsigned)id, SDL_GetError());
        return;
    }
    in_Pad *p = &in_pads[in_npads++];
    memset(p, 0, sizeof *p);
    p->gp = g;
    p->id = id;
    snprintf(p->name, sizeof p->name, "%s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "Controller");
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), p->guid, sizeof p->guid);
    for (int i = 0; i < in_nfallback; i++) p->fallback |= in_fallback_ids[i] == id;
    p->has_accel = SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL);
    p->has_gyro = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO);
    in_compile_pad(p);
    in_pad_sensors(p);
    if (!in_active_id) in_active_id = id;
    const char *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    fprintf(stderr, "input: controller connected: %s (%s, %04x:%04x, GUID %s%s%s%s)%s\n", p->name,
            type ? type : "unknown", SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), p->guid,
            p->has_accel ? ", accelerometer" : "", p->has_gyro ? ", gyro" : "",
            p->fallback ? ", generic mapping" : "", in_active_id == id ? " [active]" : "");
}

static void in_pad_close(SDL_JoystickID id) {
    in_Pad *p = in_find_pad(id);
    if (!p) return;
    fprintf(stderr, "input: controller disconnected: %s\n", p->name);
    SDL_CloseGamepad(p->gp);
    *p = in_pads[--in_npads];
    for (int i = 0; i < in_nfallback; i++)
        if (in_fallback_ids[i] == id) in_fallback_ids[i--] = in_fallback_ids[--in_nfallback];
    if (in_active_id == id) {
        in_active_id = in_npads ? in_pads[0].id : 0;
        if (in_active_id) fprintf(stderr, "input: active controller: %s\n", in_pads[0].name);
    }
}

static void in_joystick_added(SDL_JoystickID id) {
    if (!SDL_IsGamepad(id)) in_add_fallback_mapping(id);
    in_pad_open(id);
}

/* ------------------------------------------------------------------ init / events */
static void in_load_mapping_db(const char *path) {
    struct stat st;
    if (stat(path, &st)) return;
    int n = SDL_AddGamepadMappingsFromFile(path);
    if (n < 0) fprintf(stderr, "input: %s: %s\n", path, SDL_GetError());
    else fprintf(stderr, "input: %d controller mappings from %s\n", n, path);
}

static const char *in_config_dir(char *buf, size_t n) {
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(buf, n, "%s/albw-port", x);
    else if (h && *h) snprintf(buf, n, "%s/.config/albw-port", h);
    else snprintf(buf, n, ".");
    return buf;
}

int input_init(void) {
    char dir[480], db[512];
    pthread_mutex_lock(&in_mu);
    in_ensure_cfg();
    pthread_mutex_unlock(&in_mu);
    /* the config decides motion / fallback, so load it before opening devices */
    input_load(NULL);
    pthread_mutex_lock(&in_mu);
    snprintf(db, sizeof db, "%s/gamecontrollerdb.txt", in_config_dir(dir, sizeof dir));
    in_load_mapping_db(db);
    in_load_mapping_db("gamecontrollerdb.txt");
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n; i++) in_joystick_added(ids[i]);
    SDL_free(ids);
    if (!in_npads) fprintf(stderr, "input: no controller connected (keyboard only; hotplug is supported)\n");
    pthread_mutex_unlock(&in_mu);
    return 0;
}

void input_shutdown(void) {
    pthread_mutex_lock(&in_mu);
    while (in_npads) in_pad_close(in_pads[in_npads - 1].id);
    pthread_mutex_unlock(&in_mu);
}

/* key-down of sc completes an alternative of b (the other keys of a combination held) */
static int in_key_triggers(const in_Bind *b, SDL_Scancode sc) {
    const bool *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < b->n; i++) {
        const in_Alt *A = &b->a[i];
        int has = 0, others = 1;
        for (int k = 0; k < A->n; k++) {
            if (A->e[k].kind != EL_KEY) others = 0;
            else if (A->e[k].idx == (int)sc) has = 1;
            else others &= ks[A->e[k].idx];
        }
        if (has && others) return 1;
    }
    return 0;
}

static void in_capture(const char *name, uint32_t id) {
    snprintf(in_cap_name, sizeof in_cap_name, "%s", name);
    in_cap_id = id;
    in_cap_new = 1;
}

void input_handle_event(const union SDL_Event *ev) {
    const SDL_Event *e = (const SDL_Event *)ev;
    pthread_mutex_lock(&in_mu);
    switch (e->type) {
    case SDL_EVENT_JOYSTICK_ADDED: in_joystick_added(e->jdevice.which); break;
    case SDL_EVENT_GAMEPAD_ADDED: in_pad_open(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: in_pad_close(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMAPPED: {
        in_Pad *p = in_find_pad(e->gdevice.which);
        if (p) in_compile_pad(p);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const char *n = in_button_name(e->gbutton.button);
        if (n) in_capture(n, e->gbutton.which);
        if (in_active_id != e->gbutton.which && in_find_pad(e->gbutton.which)) {
            in_active_id = e->gbutton.which;
            if (in_npads > 1 && !g_input_cfg.multi) fprintf(stderr, "input: active controller: %s\n", in_find_pad(in_active_id)->name);
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        in_Pad *p = in_find_pad(e->gaxis.which);
        int ax = e->gaxis.axis, v = e->gaxis.value;
        if (!p || ax < 0 || ax >= SDL_GAMEPAD_AXIS_COUNT) break;
        int pressed = in_is_trigger(ax) ? v >= p->p.trig * 32767 / 100 : abs(v) > STICK_PRESS;
        int8_t state = (int8_t)(pressed ? (v < 0 ? -1 : 1) : 0);
        if (state == p->axis_state[ax]) break;
        p->axis_state[ax] = state;     /* only a new crossing counts as a press */
        if (!state) break;
        char n[32];
        in_Elem el = {EL_AXIS, state, (int16_t)ax};
        in_elem_name(&el, n, sizeof n);
        in_capture(n, p->id);
        if (in_active_id != p->id) {
            in_active_id = p->id;
            if (in_npads > 1 && !g_input_cfg.multi) fprintf(stderr, "input: active controller: %s\n", p->name);
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat) {
            in_ensure_cfg();
            in_kb_menu_ev |= in_key_triggers(&in_kb_bind[IN_MENU], e->key.scancode);
            in_kb_quit_ev |= in_key_triggers(&in_kb_bind[IN_QUIT], e->key.scancode);
            in_kb_screen_ev |= in_key_triggers(&in_kb_bind[IN_SCREEN], e->key.scancode);
            in_Elem el = {EL_KEY, 0, (int16_t)e->key.scancode};
            char n[48];
            in_elem_name(&el, n, sizeof n);
            in_capture(n, 0);
        }
        break;
    default: break;
    }
    pthread_mutex_unlock(&in_mu);
}

/* ------------------------------------------------------------------ per-frame state */
static int in_elem_held(const in_Pad *p, const in_Elem *e, const bool *ks, const in_Consumed *c) {
    switch (e->kind) {
    case EL_KEY:
        if (c && c->key[e->idx >> 6] >> (e->idx & 63) & 1) return 0;
        return ks && e->idx < SDL_SCANCODE_COUNT && ks[e->idx];
    case EL_BTN:
        if (c && c->btn >> e->idx & 1) return 0;
        return p && SDL_GetGamepadButton(p->gp, (SDL_GamepadButton)e->idx);
    case EL_AXIS: {
        if (!p) return 0;
        int v = SDL_GetGamepadAxis(p->gp, (SDL_GamepadAxis)e->idx);
        return in_is_trigger(e->idx) ? v >= p->p.trig * 32767 / 100 : v * e->sign > STICK_PRESS;
    }
    default: return 0;
    }
}

/* held? Combinations that are held get their elements added to *take (if given). */
static int in_bind_held(const in_Bind *b, const in_Pad *p, const bool *ks, const in_Consumed *c, in_Consumed *take) {
    int held = 0;
    for (int i = 0; i < b->n; i++) {
        const in_Alt *A = &b->a[i];
        int all = A->n > 0;
        for (int k = 0; k < A->n && all; k++) all = in_elem_held(p, &A->e[k], ks, c);
        if (!all) continue;
        held = 1;
        if (take && A->n > 1)
            for (int k = 0; k < A->n; k++) {
                const in_Elem *e = &A->e[k];
                if (e->kind == EL_KEY) take->key[e->idx >> 6] |= 1ull << (e->idx & 63);
                else if (e->kind == EL_BTN) take->btn |= 1ull << e->idx;
            }
    }
    return held;
}

static void in_read_stick(SDL_Gamepad *g, int which, int dz, int outer, int *ox, int *oy) {
    *ox = *oy = 0;
    if (which == IN_STICK_NONE) return;
    SDL_GamepadAxis ax = which == IN_STICK_LEFT ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
    float x = SDL_GetGamepadAxis(g, ax) / 32767.0f, y = -SDL_GetGamepadAxis(g, ax + 1) / 32767.0f;
    x = SDL_clamp(x, -1.0f, 1.0f);
    y = SDL_clamp(y, -1.0f, 1.0f);
    float m = SDL_sqrtf(x * x + y * y), d = dz / 100.0f, o = outer / 100.0f;
    if (m <= d || m <= 0) return;
    if (o <= d) o = d + 0.01f;
    float s = (m - d) / (o - d);
    if (s > 1) s = 1;
    s *= CPAD_MAX / m;                       /* circular: direction kept, length remapped */
    *ox = (int)SDL_lroundf(x * s);
    *oy = (int)SDL_lroundf(y * s);
}

static void in_digital_stick(const in_Bind *b, int up, const in_Pad *p, const bool *ks, const in_Consumed *c, int *x, int *y) {
    int dx = in_bind_held(&b[up + 3], p, ks, c, NULL) - in_bind_held(&b[up + 2], p, ks, c, NULL);
    int dy = in_bind_held(&b[up], p, ks, c, NULL) - in_bind_held(&b[up + 1], p, ks, c, NULL);
    if (dx) *x = dx * CPAD_MAX;
    if (dy) *y = dy * CPAD_MAX;
}

static void in_pick_larger(int16_t *dx, int16_t *dy, int x, int y) {
    if (x * x + y * y > *dx * *dx + *dy * *dy) {
        *dx = (int16_t)x;
        *dy = (int16_t)y;
    }
}

int input_update(PlatInput *in, uint32_t *kb_out, uint32_t *gp_out, uint32_t *cp_out) {
    /* config file edited outside (or by the frontend): reload, about once a second */
    if (++in_frame_count % 60 == 0) {
        struct stat st;
        pthread_mutex_lock(&in_mu);
        int changed = in_path_loaded[0] && !stat(in_path_loaded, &st) &&
                      (st.st_mtim.tv_sec != in_mtime_loaded.tv_sec || st.st_mtim.tv_nsec != in_mtime_loaded.tv_nsec);
        if (changed) fprintf(stderr, "input: %s changed, reloading\n", in_path_loaded);
        pthread_mutex_unlock(&in_mu);
        if (changed) input_reload();
    }
    pthread_mutex_lock(&in_mu);
    in_ensure_cfg();
    const bool *ks = SDL_GetKeyboardState(NULL);
    uint32_t kb = 0, gp = 0;
    int menu = 0, quit = 0, screen = 0, menu_any = 0;

    /* keyboard */
    in_Consumed kc = {{0}, 0};     /* keys of a held MENU/QUIT combination: hidden from the game */
    in_bind_held(&in_kb_bind[IN_MENU], NULL, ks, NULL, &kc);
    in_bind_held(&in_kb_bind[IN_QUIT], NULL, ks, NULL, &kc);
    in_bind_held(&in_kb_bind[IN_SCREEN], NULL, ks, NULL, &kc);
    for (int a = 0; a <= IN_DRIGHT; a++)
        if (in_bind_held(&in_kb_bind[a], NULL, ks, &kc, NULL)) kb |= in_ACTION_BITS[a];
    int x = 0, y = 0;
    in_digital_stick(in_kb_bind, IN_CPAD_UP, NULL, ks, &kc, &x, &y);
    in->cpad_x = (int16_t)x;
    in->cpad_y = (int16_t)y;
    x = y = 0;
    in_digital_stick(in_kb_bind, IN_CSTICK_UP, NULL, ks, &kc, &x, &y);
    in->cstick_x = (int16_t)x;
    in->cstick_y = (int16_t)y;

    /* controllers */
    const in_Pad *motion_pad = NULL;
    for (int i = 0; i < in_npads; i++) {
        const in_Pad *p = &in_pads[i];
        in_Consumed pc = {{0}, 0};
        menu |= in_bind_held(&p->p.bind[IN_MENU], p, NULL, NULL, &pc);
        quit |= in_bind_held(&p->p.bind[IN_QUIT], p, NULL, NULL, &pc);
        int m = in_bind_held(&p->p.bind[IN_MENU], p, NULL, NULL, NULL);
        menu_any |= m;
        screen |= !m && in_bind_held(&p->p.bind[IN_SCREEN], p, NULL, NULL, &pc);   /* not inside the menu combo */
        if (!g_input_cfg.multi && p->id != in_active_id) continue;
        for (int a = 0; a <= IN_DRIGHT; a++)
            if (in_bind_held(&p->p.bind[a], p, NULL, &pc, NULL)) gp |= in_ACTION_BITS[a];
        in_read_stick(p->gp, p->p.circle_pad, p->p.dz, p->p.outer, &x, &y);
        in_digital_stick(p->p.bind, IN_CPAD_UP, p, NULL, &pc, &x, &y);
        in_pick_larger(&in->cpad_x, &in->cpad_y, x, y);
        in_read_stick(p->gp, p->p.c_stick, p->p.cdz, p->p.outer, &x, &y);
        in_digital_stick(p->p.bind, IN_CSTICK_UP, p, NULL, &pc, &x, &y);
        in_pick_larger(&in->cstick_x, &in->cstick_y, x, y);
        if (!motion_pad && p->sensors_on) motion_pad = p;
    }

    /* motion: SDL gives m/s^2 and rad/s */
    in->motion = 0;
    in->accel[0] = 0, in->accel[1] = 1, in->accel[2] = 0;
    in->gyro[0] = in->gyro[1] = in->gyro[2] = 0;
    if (motion_pad) {
        float v[3];
        if (motion_pad->has_accel && SDL_GetGamepadSensorData(motion_pad->gp, SDL_SENSOR_ACCEL, v, 3)) {
            for (int k = 0; k < 3; k++) in->accel[k] = v[k] / SDL_STANDARD_GRAVITY;
            in->motion = 1;
        }
        if (motion_pad->has_gyro && SDL_GetGamepadSensorData(motion_pad->gp, SDL_SENSOR_GYRO, v, 3)) {
            for (int k = 0; k < 3; k++) in->gyro[k] = v[k] * (180.0f / SDL_PI_F);
            in->motion = 1;
        }
    }
    in->controllers = in_npads;
    in->menu = (menu && !in_menu_prev) || in_kb_menu_ev;
    int quit_now = (quit && !in_quit_prev) || in_kb_quit_ev;
    /* controller SCREEN (right-stick click) fires on release, and only if the menu combo (both
     * sticks) was not held meanwhile: R3 landing a frame before L3 flipped the screen and then
     * opened the menu */
    static int screen_spoiled;
    if (menu_any) screen_spoiled = 1;
    in->screen = (!screen && in_screen_prev && !screen_spoiled) || in_kb_screen_ev;
    if (!screen && !menu_any) screen_spoiled = 0;
    in_screen_prev = screen;
    in_menu_prev = menu;
    in_quit_prev = quit;
    in_kb_menu_ev = in_kb_quit_ev = in_kb_screen_ev = 0;
    pthread_mutex_unlock(&in_mu);

    *kb_out = kb;
    *gp_out = gp;
    *cp_out = (in->cpad_x > 40 ? KEY_CPAD_RIGHT : 0) | (in->cpad_x < -40 ? KEY_CPAD_LEFT : 0) |
              (in->cpad_y > 40 ? KEY_CPAD_UP : 0) | (in->cpad_y < -40 ? KEY_CPAD_DOWN : 0);
    return !quit_now;
}

/* ------------------------------------------------------------------ config file */
const char *input_config_path(void) {
    pthread_mutex_lock(&in_mu);
    if (!in_path_default[0]) {
        const char *env = getenv("ALBW_INPUT_CFG");
        char dir[480];
        if (env && *env) snprintf(in_path_default, sizeof in_path_default, "%s", env);
        else snprintf(in_path_default, sizeof in_path_default, "%s/input.cfg", in_config_dir(dir, sizeof dir));
    }
    pthread_mutex_unlock(&in_mu);
    return in_path_default;
}

static int in_action_index(const char *k) {
    for (int a = 0; a < IN_ACTION_COUNT; a++)
        if (!strcasecmp(k, in_ACTION_NAMES[a])) return a;
    return -1;
}

static int in_parse_bool(const char *v) {
    if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")) return 1;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0")) return 0;
    return -1;
}

static int in_parse_int(const char *v, int *out) {
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (end == v || *in_trim(end) || errno) return -1;
    *out = (int)n;
    return 0;
}

static int in_parse_stick(const char *v) {
    if (!strcasecmp(v, "left")) return IN_STICK_LEFT;
    if (!strcasecmp(v, "right")) return IN_STICK_RIGHT;
    if (!strcasecmp(v, "none")) return IN_STICK_NONE;
    return -1;
}

static int in_valid_guid(const char *g) {
    int n = 0;
    for (; g[n]; n++)
        if (!SDL_isxdigit((unsigned char)g[n])) return 0;
    return n == 32;
}

/* parse a file into *c (starting from defaults); 0 ok, 1 missing, 2 bad lines, -1 unreadable */
static int in_parse_file(const char *path, InputConfig *c) {
    FILE *f = fopen(path, "r");
    input_defaults(c);
    if (!f) return errno == ENOENT ? 1 : -1;
    enum { S_GENERAL, S_KEYBOARD, S_PAD, S_NONE } sec = S_GENERAL;
    InputPadProfile *prof = &c->pad;
    char line[1024];
    int ln = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = in_trim(line);
        if (!*s || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) {
                in_err_add("line %d: missing ']'", ln), bad++;
                continue;
            }
            *e = 0;
            s = in_trim(s + 1);
            if (!strcasecmp(s, "general")) sec = S_GENERAL;
            else if (!strcasecmp(s, "keyboard")) sec = S_KEYBOARD;
            else if (!strcasecmp(s, "gamepad")) sec = S_PAD, prof = &c->pad;
            else if (!strncasecmp(s, "gamepad", 7) && (s[7] == ' ' || s[7] == '\t') && in_valid_guid(in_trim(s + 8))) {
                const char *g = in_trim(s + 8);
                prof = (InputPadProfile *)in_find_override(c, g);
                if (!prof) {
                    if (c->n_overrides == IN_MAX_OVERRIDES) {
                        in_err_add("line %d: more than %d controller sections", ln, IN_MAX_OVERRIDES), bad++;
                        sec = S_NONE;
                        continue;
                    }
                    prof = &c->overrides[c->n_overrides++];
                    in_unset_profile(prof);
                    snprintf(prof->guid, sizeof prof->guid, "%s", g);
                    for (char *q = prof->guid; *q; q++) *q = (char)tolower((unsigned char)*q);
                }
                sec = S_PAD;
            } else {
                in_err_add("line %d: unknown section [%s]", ln, s), bad++;
                sec = S_NONE;
            }
            continue;
        }
        if (sec == S_NONE) continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            in_err_add("line %d: expected key = value", ln), bad++;
            continue;
        }
        *eq = 0;
        char *k = in_trim(s), *v = in_trim(eq + 1), err[160];
        int a = in_action_index(k), n;
        if (sec == S_GENERAL) {
            if (!strcasecmp(k, "controllers") && (!strcasecmp(v, "active") || !strcasecmp(v, "any")))
                c->multi = !strcasecmp(v, "any");
            else if (!strcasecmp(k, "motion") && (n = in_parse_bool(v)) >= 0) c->motion = n;
            else if (!strcasecmp(k, "fallback") && (n = in_parse_bool(v)) >= 0) c->fallback = n;
            else if (!strcasecmp(k, "fallback_mapping")) snprintf(c->fallback_mapping, sizeof c->fallback_mapping, "%s", v);
            else in_err_add("line %d: bad setting \"%s = %s\"", ln, k, v), bad++;
        } else if (a >= 0) {
            in_Bind b;
            if (in_parse_bind(v, sec == S_KEYBOARD, a, &b, err, sizeof err)) {
                in_err_add("line %d: %s: %s", ln, k, err), bad++;
                continue;
            }
            snprintf(sec == S_KEYBOARD ? c->key[a] : prof->bind[a], IN_BIND_LEN, "%s", v);
        } else if (sec == S_KEYBOARD) {
            in_err_add("line %d: unknown action \"%s\"", ln, k), bad++;
        } else if (!strcasecmp(k, "name") && prof != &c->pad) {
            snprintf(prof->name, sizeof prof->name, "%s", v);
        } else if (!strcasecmp(k, "layout") && (!strcasecmp(v, "positional") || !strcasecmp(v, "labels"))) {
            prof->layout = !strcasecmp(v, "labels") ? IN_LAYOUT_LABELS : IN_LAYOUT_POSITIONAL;
        } else if (!strcasecmp(k, "circle_pad") && (n = in_parse_stick(v)) >= 0) {
            prof->circle_pad = n;
        } else if (!strcasecmp(k, "c_stick") && (n = in_parse_stick(v)) >= 0) {
            prof->c_stick = n;
        } else if (!strcasecmp(k, "stick_deadzone") && !in_parse_int(v, &n)) {
            prof->stick_deadzone = n;
        } else if (!strcasecmp(k, "cstick_deadzone") && !in_parse_int(v, &n)) {
            prof->cstick_deadzone = n;
        } else if (!strcasecmp(k, "stick_outer") && !in_parse_int(v, &n)) {
            prof->stick_outer = n;
        } else if (!strcasecmp(k, "trigger_threshold") && !in_parse_int(v, &n)) {
            prof->trigger_threshold = n;
        } else {
            in_err_add("line %d: bad setting \"%s = %s\"", ln, k, v), bad++;
        }
    }
    fclose(f);
    in_sanitize(c);
    return bad ? 2 : 0;
}

int input_load(const char *path) {
    if (!path) path = input_config_path();
    static InputConfig tmp;       /* large: not on the stack */
    pthread_mutex_lock(&in_mu);
    in_last_err[0] = 0;
    int r = in_parse_file(path, &tmp);
    if (r == -1) {
        in_err_add("%s: %s", path, strerror(errno));
        fprintf(stderr, "input: %s\n", in_last_err);
    } else {
        g_input_cfg = tmp;
        in_cfg_ready = 1;
        snprintf(in_path_loaded, sizeof in_path_loaded, "%s", path);
        struct stat st;
        in_mtime_loaded = stat(path, &st) ? (struct timespec){0, 0} : st.st_mtim;
        in_apply_config();
        if (r == 0) fprintf(stderr, "input: config %s\n", path);
        else if (r == 1) fprintf(stderr, "input: no %s, using the default mapping\n", path);
        else fprintf(stderr, "input: config %s: %s\n", path, in_last_err);
    }
    pthread_mutex_unlock(&in_mu);
    return r;
}

int input_reload(void) {
    char p[512];
    pthread_mutex_lock(&in_mu);
    snprintf(p, sizeof p, "%s", in_path_loaded);
    pthread_mutex_unlock(&in_mu);
    return input_load(p[0] ? p : NULL);
}

static int in_mkdirs(const char *file) {
    char d[512];
    snprintf(d, sizeof d, "%s", file);
    char *slash = strrchr(d, '/');
    if (!slash) return 0;
    *slash = 0;
    for (char *q = d + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            if (mkdir(d, 0755) && errno != EEXIST) return -1;
            *q = '/';
        }
    return mkdir(d, 0755) && errno != EEXIST ? -1 : 0;
}

static const char *in_stick_str(int s) { return s == IN_STICK_LEFT ? "left" : s == IN_STICK_RIGHT ? "right" : "none"; }

static void in_write_profile(FILE *f, const InputPadProfile *p, int is_default) {
    if (p->layout != IN_UNSET) fprintf(f, "layout = %s\n", p->layout == IN_LAYOUT_LABELS ? "labels" : "positional");
    if (p->circle_pad != IN_UNSET) fprintf(f, "circle_pad = %s\n", in_stick_str(p->circle_pad));
    if (p->c_stick != IN_UNSET) fprintf(f, "c_stick = %s\n", in_stick_str(p->c_stick));
    if (p->stick_deadzone != IN_UNSET) fprintf(f, "stick_deadzone = %d\n", p->stick_deadzone);
    if (p->cstick_deadzone != IN_UNSET) fprintf(f, "cstick_deadzone = %d\n", p->cstick_deadzone);
    if (p->stick_outer != IN_UNSET) fprintf(f, "stick_outer = %d\n", p->stick_outer);
    if (p->trigger_threshold != IN_UNSET) fprintf(f, "trigger_threshold = %d\n", p->trigger_threshold);
    for (int a = 0; a < IN_ACTION_COUNT; a++)
        if (is_default || p->bind[a][0])
            fprintf(f, "%s = %s\n", in_ACTION_NAMES[a], p->bind[a][0] ? p->bind[a] : "none");
}

int input_save(const char *path) {
    if (!path) path = input_config_path();
    pthread_mutex_lock(&in_mu);
    in_ensure_cfg();
    in_last_err[0] = 0;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = in_mkdirs(path) ? NULL : fopen(tmp, "w");
    if (!f) {
        in_err_add("%s: %s", path, strerror(errno));
        pthread_mutex_unlock(&in_mu);
        return -1;
    }
    fprintf(f, "# ALBW native port: input configuration. Format: native/platform/input.h\n"
               "# Bindings: comma-separated alternatives, '&' joins a combination, \"none\" unbinds.\n\n"
               "[general]\ncontrollers = %s\nmotion = %s\nfallback = %s\n",
            g_input_cfg.multi ? "any" : "active", g_input_cfg.motion ? "on" : "off", g_input_cfg.fallback ? "on" : "off");
    if (g_input_cfg.fallback_mapping[0]) fprintf(f, "fallback_mapping = %s\n", g_input_cfg.fallback_mapping);
    fprintf(f, "\n[keyboard]\n");
    for (int a = 0; a < IN_ACTION_COUNT; a++)
        fprintf(f, "%s = %s\n", in_ACTION_NAMES[a], g_input_cfg.key[a][0] ? g_input_cfg.key[a] : "none");
    fprintf(f, "\n[gamepad]\n");
    in_write_profile(f, &g_input_cfg.pad, 1);
    for (int i = 0; i < g_input_cfg.n_overrides; i++) {
        fprintf(f, "\n[gamepad %s]\n", g_input_cfg.overrides[i].guid);
        if (g_input_cfg.overrides[i].name[0]) fprintf(f, "name = %s\n", g_input_cfg.overrides[i].name);
        in_write_profile(f, &g_input_cfg.overrides[i], 0);
    }
    int bad = ferror(f);
    bad |= fclose(f);
    if (bad || rename(tmp, path)) {
        in_err_add("%s: %s", path, strerror(errno));
        remove(tmp);
        pthread_mutex_unlock(&in_mu);
        return -1;
    }
    /* our own write: do not count it as an outside edit */
    struct stat st;
    if (!strcmp(path, in_path_loaded) && !stat(path, &st)) in_mtime_loaded = st.st_mtim;
    pthread_mutex_unlock(&in_mu);
    return 0;
}

void input_get_config(InputConfig *out) {
    pthread_mutex_lock(&in_mu);
    in_ensure_cfg();
    *out = g_input_cfg;
    pthread_mutex_unlock(&in_mu);
}

int input_set_config(const InputConfig *c) {
    static InputConfig tmp;
    char err[160];
    pthread_mutex_lock(&in_mu);
    in_last_err[0] = 0;
    tmp = *c;
    in_sanitize(&tmp);
    in_Bind b;
    for (int a = 0; a < IN_ACTION_COUNT; a++) {
        if (in_parse_bind(tmp.key[a], 1, a, &b, err, sizeof err)) in_err_add("[keyboard] %s: %s", in_ACTION_NAMES[a], err);
        if (in_parse_bind(tmp.pad.bind[a], 0, a, &b, err, sizeof err)) in_err_add("[gamepad] %s: %s", in_ACTION_NAMES[a], err);
        for (int i = 0; i < tmp.n_overrides; i++)
            if (in_parse_bind(tmp.overrides[i].bind[a], 0, a, &b, err, sizeof err))
                in_err_add("[gamepad %s] %s: %s", tmp.overrides[i].guid, in_ACTION_NAMES[a], err);
    }
    for (int i = 0; i < tmp.n_overrides; i++)
        if (!in_valid_guid(tmp.overrides[i].guid)) in_err_add("bad controller GUID \"%s\"", tmp.overrides[i].guid);
    int r = in_last_err[0] ? -1 : 0;
    if (!r) {
        g_input_cfg = tmp;
        in_cfg_ready = 1;
        in_apply_config();
    }
    pthread_mutex_unlock(&in_mu);
    return r;
}

const char *input_last_error(void) {
    pthread_mutex_lock(&in_mu);
    snprintf(in_err_ret, sizeof in_err_ret, "%s", in_last_err);
    pthread_mutex_unlock(&in_mu);
    return in_err_ret;
}

/* ------------------------------------------------------------------ introspection */
const char *input_action_name(InAction a) {
    return (unsigned)a < IN_ACTION_COUNT ? in_ACTION_NAMES[a] : "?";
}

int input_check_binding(const char *binding, int keyboard, InAction a, char *err, size_t errlen) {
    in_Bind b;
    char e[160] = "";
    int r = (unsigned)a < IN_ACTION_COUNT ? in_parse_bind(binding, keyboard, a, &b, e, sizeof e) : -1;
    if (r && err && errlen) snprintf(err, errlen, "%s", e[0] ? e : "bad action");
    return r;
}

int input_controllers(InputControllerInfo *out, int max) {
    pthread_mutex_lock(&in_mu);
    int n = 0;
    for (int i = 0; i < in_npads && n < max; i++, n++) {
        const in_Pad *p = &in_pads[i];
        InputControllerInfo *o = &out[n];
        memset(o, 0, sizeof *o);
        o->id = p->id;
        snprintf(o->name, sizeof o->name, "%s", p->name);
        snprintf(o->guid, sizeof o->guid, "%s", p->guid);
        const char *t = SDL_GetGamepadStringForType(SDL_GetGamepadType(p->gp));
        snprintf(o->type, sizeof o->type, "%s", t ? t : "unknown");
        o->vendor = SDL_GetGamepadVendor(p->gp);
        o->product = SDL_GetGamepadProduct(p->gp);
        o->fallback = p->fallback;
        o->has_accel = p->has_accel;
        o->has_gyro = p->has_gyro;
        o->active = g_input_cfg.multi || p->id == in_active_id;
        o->has_override = p->p.has_override;
    }
    int total = in_npads;
    pthread_mutex_unlock(&in_mu);
    return total;
}

int input_effective_binding(uint32_t id, InAction a, char *out, size_t n) {
    if ((unsigned)a >= IN_ACTION_COUNT || !n) return -1;
    pthread_mutex_lock(&in_mu);
    in_ensure_cfg();
    const in_Bind *b = NULL;
    if (!id) b = &in_kb_bind[a];
    else {
        in_Pad *p = in_find_pad(id);
        if (p) b = &p->p.bind[a];
    }
    if (b) in_bind_text(b, out, n);
    pthread_mutex_unlock(&in_mu);
    return b ? 0 : -1;
}

int input_capture(char *out, size_t n, uint32_t *id) {
    pthread_mutex_lock(&in_mu);
    int r = in_cap_new;
    if (r) {
        snprintf(out, n, "%s", in_cap_name);
        if (id) *id = in_cap_id;
        in_cap_new = 0;
    }
    pthread_mutex_unlock(&in_mu);
    return r;
}

void input_capture_reset(void) {
    pthread_mutex_lock(&in_mu);
    in_cap_new = 0;
    pthread_mutex_unlock(&in_mu);
}
