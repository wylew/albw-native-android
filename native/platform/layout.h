/* Screen layout engine, shared by the SDL backend (platform.c) and the Qt frontend
 * (native/frontend/qt). Plain C, no GL/SDL/Qt, 32- and 64-bit clean.
 *
 * Windows: "separate" (the default) gives each screen its own window, "joined" draws both into one
 * window. The presets and the custom layout arrange the screens of the joined window; with
 * separate windows each window shows its one screen letterboxed (layout_compute_single), and of
 * the presets only top_only / bottom_only matter: they hide the other screen's window
 * (layout_separate_visible). swap, custom rectangles and custom visibility apply to joined only.
 *
 * It decides where the two 3DS screens are drawn inside the (joined) window:
 *   - presets (top above bottom, side by side, large + small, picture-in-picture, one screen),
 *   - a custom layout: one rectangle per screen, stored normalized to the window (0..1) so it
 *     follows window resizes, edited by dragging/resizing with the mouse (layout_edit_*),
 * and maps window coordinates back to 3DS screen coordinates (bottom-screen touch).
 *
 * All rectangles are in window PIXELS (framebuffer pixels, top-left origin, y down).
 *
 * Config file (text, locale independent), default path:
 *   $ALBW_LAYOUT_CFG, else $XDG_CONFIG_HOME/albw-port/layout.cfg, else ~/.config/albw-port/layout.cfg
 *   # comment
 *   preset default            default | side_by_side | large_top | pip | top_only | bottom_only | custom
 *   swap 0                    1 = exchange the two screens' places in the presets
 *   integer_scale 0           1 = presets scale by whole multiples of the native size
 *   linear_filter 1           1 = bilinear, 0 = nearest-neighbour when scaling the screens
 *   large_ratio 2.50000       large_top / pip: big screen size relative to the small one
 *   keep_aspect 1             custom: keep 5:3 / 4:3 when resizing (and letterbox inside the box)
 *   render_scale 1            internal resolution: 1..8 x the 3DS screens (ALBW_RENDER_SCALE overrides)
 *   menu_style bar            Esc menu: bar (across the top) or centered (a panel in the middle)
 *   confirm_load 1            quick save-state loads (L3+R, F7) ask for a second press
 *   hd_textures 1             use an installed HD texture pack (Esc menu > Textures)
 *   hd_cache 1                keep the pack's decoded images in a disk cache (off deletes it)
 *   hud_overlay 0             bottom-screen rupees and Y item on the main screen (single screen)
 *   snap 1                    custom editor: snap to window edges/centre, the other screen, integer sizes
 *   custom_front bottom       screen drawn last (on top) where custom rectangles overlap
 *   widescreen 0              1 = 16:9 top screen: the game draws a wider view (F10)
 *   single_screen 0           1 = show one screen: the top one, and the bottom one in its place while
 *                             the SCREEN binding (Tab / right-stick click) has it toggled up
 *   custom top 0.00000 0.00000 1.00000 0.50000 1      x y w h (normalized to the window) visible
 *   custom bottom 0.10000 0.50000 0.80000 0.50000 1
 * Unknown keys are ignored, missing keys keep their defaults. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define LAYOUT_TOP_W (layout_top_w())   /* 400, or wider in widescreen */
int layout_top_w(void);
void layout_set_top_w(int w);
#define LAYOUT_WIDE_TOP_W 427   /* 16:9 at 240 rows */
#define LAYOUT_BOTTOM_W 320
#define LAYOUT_SCREEN_H 240
#define LAYOUT_SCREEN_TOP 0
#define LAYOUT_SCREEN_BOTTOM 1

typedef enum {
    LAYOUT_DEFAULT = 0,      /* top above bottom, bottom centred (the 3DS) */
    LAYOUT_SIDE_BY_SIDE,     /* top left, bottom right */
    LAYOUT_LARGE_TOP,        /* big top, small bottom at its lower right (swap: big bottom) */
    LAYOUT_PIP,              /* top fills the window, small bottom overlaid in the lower right corner */
    LAYOUT_TOP_ONLY,
    LAYOUT_BOTTOM_ONLY,
    LAYOUT_CUSTOM,           /* free rectangles (LayoutConfig.custom) */
    LAYOUT_PRESET_COUNT
} LayoutPreset;

typedef struct { float x, y, w, h; } LayoutRect;

/* Remembered geometry of one separate window (window points). */
typedef struct { int x, y, w, h, has_pos, has_size; } LayoutWindowGeom;

typedef struct {
    int separate;            /* 1 = one window per screen (default), 0 = both screens in one window */
    int preset;              /* LayoutPreset */
    int swap;
    int integer_scale;
    int linear_filter;
    float large_ratio;       /* 1.5 .. 6 */
    int keep_aspect;
    int snap;
    int render_scale;        /* 1..8 internal resolution multiple */
    int menu_centered;       /* Esc menu style: 0 bar across the top, 1 centered panel */
    int confirm_load;        /* quick loads (L3+R, F7) ask twice: 1 (default) or 0 */
    int hd_textures;         /* HD texture pack (render/hdtex.c) used when installed: 1 (default) or 0 */
    int hd_cache;            /* disk cache of the pack's decoded images: 1 (default) or 0 (deleted) */
    int hud_overlay;         /* rupees + Y item on the main screen when the bottom one is hidden: 0 (default) or 1 */
    int custom_front;        /* LAYOUT_SCREEN_TOP / _BOTTOM */
    LayoutRect custom[2];    /* normalized to the window */
    int custom_visible[2];
    LayoutWindowGeom window[2];  /* separate windows, per screen */
    int single_screen;       /* 1 = one screen at a time (overrides preset / swap / custom) */
    int widescreen;          /* 1 = 16:9 top screen (wider view, not stretched) */
    int show_bottom;         /* single_screen: the bottom screen is up (runtime only, not saved) */
} LayoutConfig;

/* A computed layout for one window size. */
typedef struct {
    int ww, wh;              /* window size in pixels */
    LayoutRect r[2];         /* pixel-aligned screen rectangles */
    int visible[2];
    int order[2];            /* draw order: order[0] first, order[1] last (on top) */
} Layout;

void layout_defaults(LayoutConfig *c);
void layout_compute(const LayoutConfig *c, int ww, int wh, Layout *out);

/* Separate windows: the layout of the window that shows only `screen` (letterboxed, centred,
 * integer_scale respected), so layout_to_screen / layout_rect_ndc work on it as on the joined one. */
void layout_compute_single(const LayoutConfig *c, int screen, int ww, int wh, Layout *out);
/* Separate windows: is the window of `screen` shown? (top_only / bottom_only hide the other one) */
int layout_separate_visible(const LayoutConfig *c, int screen);
/* "separate" / "joined" (ALBW_WINDOWS, layout.cfg "windows"): 1 / 0, -1 if neither */
int layout_windows_from_key(const char *key);

/* Natural canvas of a preset in 3DS pixels (e.g. default 400x480), for "window scale Nx". */
void layout_canvas_size(const LayoutConfig *c, int *w, int *h);

const char *layout_preset_name(int preset);   /* human-readable, e.g. "Side by side" */
const char *layout_preset_key(int preset);    /* config-file key, e.g. "side_by_side" */
int layout_preset_from_key(const char *key);  /* -1 if unknown */
void layout_native_size(int screen, int *w, int *h);

/* Topmost visible screen under window pixel (x, y), or -1. */
int layout_hit(const Layout *l, float x, float y);
/* Window pixel -> native screen pixel of `screen` (0..399 / 0..319, 0..239), clamped to the screen.
 * Returns 1 if the point is inside the drawn screen. */
int layout_to_screen(const Layout *l, int screen, float x, float y, int *sx, int *sy);
/* Pixel rect -> OpenGL NDC rect (x, y, w, h; bottom-left origin) for a full-window viewport. */
void layout_rect_ndc(const Layout *l, int screen, float ndc[4]);

/* Turn the current layout into the custom one (preset = custom, rects copied). */
void layout_make_custom(LayoutConfig *c, const Layout *l);

/* ---- custom layout editing (mouse drag / resize), shared by both frontends ---- */
enum { LAYOUT_EDIT_NONE = 0, LAYOUT_EDIT_MOVE, LAYOUT_EDIT_RESIZE };
typedef struct {
    int mode;                /* LAYOUT_EDIT_* (NONE = no drag in progress) */
    int screen;
    int corner;              /* resize: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right (the dragged one) */
    float mx0, my0;          /* pointer at drag start */
    LayoutRect r0;           /* rectangle at drag start (pixels) */
    float guide_x, guide_y;  /* snap guide lines (pixels) of the last update, < 0 = none */
} LayoutEdit;

/* What a press at (x, y) would do: LAYOUT_EDIT_MOVE / _RESIZE / _NONE; screen and corner out.
 * handle = corner handle size in pixels. */
int layout_edit_hit(const Layout *l, float x, float y, float handle, int *screen, int *corner);
/* Start a drag. force_resize: resize from the nearest corner even if not on a handle (right mouse).
 * Switches the config to the custom preset if needed. Returns 1 if a drag started. */
int layout_edit_begin(LayoutEdit *e, LayoutConfig *c, const Layout *l, float x, float y, float handle,
                      int force_resize);
/* Pointer moved to (x, y). free_aspect: invert keep_aspect for this update (e.g. Shift held);
 * no_snap: disable snapping for this update (e.g. Alt held). Updates c->custom. */
void layout_edit_update(LayoutEdit *e, LayoutConfig *c, const Layout *l, float x, float y, int free_aspect,
                        int no_snap);
void layout_edit_end(LayoutEdit *e);
/* Scale one custom screen around its centre (mouse wheel), factor e.g. 1.1 or 1/1.1. */
void layout_edit_scale(LayoutConfig *c, const Layout *l, int screen, float factor);
/* Set a custom screen's size to n x native (n = 0: as large as fits), keeping its centre. */
void layout_edit_set_size(LayoutConfig *c, const Layout *l, int screen, float n);

/* ---- config file ---- */
const char *layout_default_path(void);
int layout_load(LayoutConfig *c, const char *path);       /* path NULL = default; 0 ok, -1 not found (defaults kept) */
int layout_save(const LayoutConfig *c, const char *path); /* creates the directory; 0 ok */

/* Locale-independent number formatting/parsing for the config files (Qt sets LC_NUMERIC). */
void layout_fmt_float(char *buf, int n, float v);
float layout_parse_float(const char *s, const char **end);

#ifdef __cplusplus
}
#endif
