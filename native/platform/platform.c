/* SDL3 + OpenGL 3.3 platform layer. */
#include "platform.h"
/* native/sdk/os.c (app-core scheduler) in the game build; weak no-ops here so the platform layer
 * also links on its own (asset viewer, Qt frontend, tests) */
__attribute__((weak)) void albw_sched_block_begin(void) {}
__attribute__((weak)) void albw_sched_block_end(void) {}
/* the same for the renderer (native/render/render.c: internal resolution) and the profiler
 * (native/runtime/prof.c), which builds without them (asset viewer, render tests) lack */
#include "../render/render.h"
__attribute__((weak)) int render_ready(void) { return 0; }
__attribute__((weak)) int render_get_scale(void) { return 1; }
__attribute__((weak)) void render_set_scale(int scale) { (void)scale; }
__attribute__((weak)) void render_set_widescreen(int rows) { (void)rows; }
__attribute__((weak)) void prof_frame(void) {}
#include <SDL3/SDL.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

/* Screen layout engine (presets, custom layout, touch mapping), shared with the Qt frontend.
 * Compiled into this translation unit so the build scripts that compile platform.c alone
 * (tools/link.sh, native/assets/build.sh, native/render/test/build.sh) need no change.
 * A build that compiles layout.c separately defines PLAT_LAYOUT_SEPARATE. */
#ifndef PLAT_LAYOUT_SEPARATE
#include "layout.c"
#else
#include "layout.h"
#endif

/* Keyboard + game controller input (input.h), compiled in the same way as layout.c; a build
 * that compiles input.c separately defines PLAT_INPUT_SEPARATE. */
#ifndef PLAT_INPUT_SEPARATE
#include "input.c"
#else
#include "input.h"
#endif

#define GL_DEFINE(ret, name, args) ret (*name) args;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

int gl_load(void *(*get_proc)(const char *)) {
    int missing = 0;
#define GL_LOAD(ret, name, args) \
    name = (ret(*) args)get_proc(#name); \
    if (!name) { fprintf(stderr, "GL: missing %s\n", #name); missing++; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return missing;
}

/* KHR_debug (core in 4.3, extension on most 3.3 drivers): optional, never required */
typedef void (*GLDebugProc)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
static void debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei len, const GLchar *msg,
                     const void *user) {
    if (severity == 0x826B) return;  /* GL_DEBUG_SEVERITY_NOTIFICATION */
    fprintf(stderr, "GL debug [type %#x sev %#x]: %s\n", type, severity, msg);
}

void gl_enable_debug(void *(*get_proc)(const char *)) {
    void (*cb)(GLDebugProc, const void *) = (void (*)(GLDebugProc, const void *))get_proc("glDebugMessageCallback");
    if (!cb) {
        fprintf(stderr, "GL: KHR_debug not available, ALBW_GL_DEBUG ignored\n");
        return;
    }
    glEnable(0x92E0);  /* GL_DEBUG_OUTPUT */
    glEnable(0x8242);  /* GL_DEBUG_OUTPUT_SYNCHRONOUS */
    cb(debug_cb, NULL);
    fprintf(stderr, "GL: debug output enabled\n");
}

#if defined(__ANDROID__)
/* GLES 3.2 (README: Android port, step 6): "#version 330 core" -> "#version 320 es" plus default
 * precisions (fragment shaders have none for float / int / most sampler types), placed after the
 * leading preprocessor lines (#extension must come before any code). Returns malloc'd text. */
static char *gles_source(const char *src) {
    static const char prec[] =
        "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
        "precision highp sampler3D;\nprecision highp samplerCube;\nprecision highp sampler2DArray;\n"
        "precision highp sampler2DShadow;\nprecision highp isampler2D;\nprecision highp usampler2D;\n"
        "precision highp samplerBuffer;\nprecision highp isamplerBuffer;\nprecision highp usamplerBuffer;\n";
    const char *body = src;
    if (!strncmp(body, "#version 330 core", 17)) body += 17;
    else if (!strncmp(body, "#version", 8)) body = strchr(body, '\n') ? strchr(body, '\n') : body + strlen(body);
    while (*body == '\n') body++;
    const char *code = body;   /* after the leading preprocessor lines */
    while (*code == '#') {
        const char *nl = strchr(code, '\n');
        if (!nl) { code += strlen(code); break; }
        code = nl + 1;
    }
    size_t n = strlen(src) + sizeof prec + 64;
    char *out = malloc(n);
    snprintf(out, n, "#version 320 es\n%.*s%s#line 2\n%s", (int)(code - body), body, prec, code);
    return out;
}
#endif

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
#if defined(__ANDROID__)
    char *es = gles_source(src);
    glShaderSource(s, 1, (const char *const *)&es, NULL);
    free(es);
#else
    glShaderSource(s, 1, &src, NULL);
#endif
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
#if defined(__ANDROID__)
        { char *es = gles_source(src); fprintf(stderr, "---- source (GLES) ----\n%s\n----\n", es); free(es); }
#endif
        abort();
    }
    return s;
}

GLuint gl_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program link failed:\n%s\n", log);
        abort();
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* ------------------------------------------------------------------ state */
/* Windows: "separate" (default) = one window per screen, "joined" = both screens in one window
 * (see the window mode section below). `win` always exists and owns the GL context: it is the
 * joined window, or the top screen's window; `win_bottom` exists only while separate. Both
 * windows share the one context (SDL_GL_MakeCurrent per window before drawing into it). */
static SDL_Window *win;
static SDL_Window *win_bottom;
static int g_separate;               /* current mode; layout.cfg "windows" is the saved choice */
static int g_gamescope;   /* Steam Deck Game Mode / gamescope session (plat_init) */
static int g_joined_w, g_joined_h;   /* joined window size (points), restored when joining again */
static int g_last_two_preset = LAYOUT_DEFAULT;  /* separate F2: "both windows" returns to it */
static uint64_t g_geom_dirty_ns;     /* a separate window moved/resized: save once it settles */
static SDL_GLContext ctx;
static PlatConfig cfg;
static PlatInput in;
static uint64_t next_frame_ns;
/* screen layout state (see the layout section below) */
static LayoutConfig g_layout_cfg;
static Layout g_layout;
static LayoutEdit g_layout_edit;
static int g_layout_editing;
static char g_layout_base_title[256];
static SDL_Cursor *g_layout_cursor[4];   /* default, move, nwse, nesw */
static void plat_layout_title(void);
static int plat_windows_init(void);
static void plat_set_windows(int separate, int save);
static void plat_layout_changed(void);
static void plat_layout_cursor(int which);
static int g_menu_open, g_menu_sel, g_menu_cur, g_quit_req;   /* Esc menu bar (menu.c): open, item, menu */
static char g_menu_note[160];
/* Debug menu switches (menu.c): read by sdk/eft.c and render/render.c through weak references, so
 * builds without the platform layer see them as off. Speed: percent of 60 Hz, 0 = unlimited. */
int g_dbg_eft_off, g_dbg_fog_off;
static int g_dbg_show_fps, g_dbg_speed = 100;
/* save states (native/runtime/savestate.c; weak: the asset viewer has none): F6 save, F7 load,
 * F8 next slot, File menu */
__attribute__((weak)) void albw_ss_request(int op, int slot);
__attribute__((weak)) void albw_ss_frame_point(void);
__attribute__((weak)) int albw_ss_slot_info(int slot, char *buf, size_t n);
__attribute__((weak)) const char *albw_ss_note(void);
#define SS_SLOTS_UI 10
static int g_ss_slot;
static char g_title_fps[160];
static const char *g_title_msg;   /* save-state outcome, shown for a few seconds */
static int g_title_dirty;   /* " - 59.9 fps" while Debug > Show FPS is on, + a save-state message */

typedef struct { GLuint fbo, color, depth; int w, h; } Target;
static Target screens[2];
static GLuint blit_prog, blit_vao;

static void target_init(Target *t, int w, int h) {
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->color);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "screen render target incomplete\n");
        abort();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Internal resolution (Esc menu, layout.cfg render_scale, --render-scale, ALBW_RENDER_SCALE):
 * the renderer's surfaces and these screen targets are re-created at the new multiple. */
static int g_want_render_scale;
static void plat_apply_render_scale(int r) {
    if (r < 1 || r > 8) return;
    render_set_scale(r);
    if (render_get_scale() != r) return;
    for (int i = 0; i < 2; i++) {
        Target *t = &screens[i];
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteRenderbuffers(1, &t->depth);
        glDeleteTextures(1, &t->color);
        target_init(t, (i == SCREEN_TOP ? TOP_W : BOTTOM_W) * r, SCREEN_H * r);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cfg.render_scale = r;
}

static const char *BLIT_VS =
    "#version 330 core\n"
    "uniform vec4 rect; out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = p;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *BLIT_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o; uniform sampler2D tex;\n"
    "void main(){ o = texture(tex, uv); }\n";

#ifndef ALBW_WINDOWS
/* Hybrid laptops (Intel / AMD graphics + an NVIDIA GPU, "Optimus"): OpenGL runs on the built-in GPU
 * unless the program asks for the NVIDIA one (GitHub issue 5). Ask the way NVIDIA's prime-run does:
 * __NV_PRIME_RENDER_OFFLOAD=1 and __GLX_VENDOR_LIBRARY_NAME=nvidia, set before SDL loads OpenGL.
 * Only when the machine has an NVIDIA GPU and another one; a value the user already set wins.
 * ALBW_GPU=integrated keeps the built-in GPU, ALBW_GPU=nvidia forces the offload. */
static void plat_pick_hybrid_gpu(void) {
    const char *want = getenv("ALBW_GPU");
    if (want && !strcmp(want, "integrated")) return;
    int nvidia = 0, other = 0;
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof path, "/sys/class/drm/card%d/device/vendor", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned vendor = 0;
        if (fscanf(f, "%x", &vendor) == 1) {
            if (vendor == 0x10de) nvidia = 1;            /* NVIDIA */
            else if (vendor == 0x8086 || vendor == 0x1002) other = 1;   /* Intel, AMD */
        }
        fclose(f);
    }
    if (!(nvidia && other) && !(want && !strcmp(want, "nvidia"))) return;
    /* the game is a 32-bit program: it needs the 32-bit NVIDIA OpenGL library */
    if (access("/usr/lib32/libGLX_nvidia.so.0", F_OK) != 0 &&
        access("/usr/lib/i386-linux-gnu/libGLX_nvidia.so.0", F_OK) != 0) {
        fprintf(stderr, "platform: hybrid GPU, but the 32-bit NVIDIA OpenGL library is missing: running on the\n"
                        "    built-in GPU. Install it (Arch: lib32-nvidia-utils; Ubuntu / Debian: libnvidia-gl-<version>:i386).\n");
        return;
    }
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
    fprintf(stderr, "platform: hybrid GPU: using the NVIDIA GPU (ALBW_GPU=integrated to keep the built-in one)\n");
}
#endif

int plat_init(const char *title, const PlatConfig *c) {
    cfg = *c;
    if (cfg.scale < 1) cfg.scale = 2;
#ifndef ALBW_WINDOWS   /* Linux only (the Windows build, windows/README.md, uses SDL's own driver) */
    {
        /* GPU on NVIDIA + Wayland: this 32-bit build's EGL needs the 32-bit egl-wayland platform
         * library; without it EGL falls back to Mesa's software rasteriser (llvmpipe: ~20-25 fps,
         * every core busy). X11 (XWayland) reaches the GPU through the 32-bit NVIDIA GLX, so
         * prefer it in that case unless the user chose a driver (SDL_VIDEO_DRIVER). */
        const char *drv = getenv("SDL_VIDEO_DRIVER");
        plat_pick_hybrid_gpu();
        if ((!drv || !*drv) && getenv("DISPLAY") && access("/proc/driver/nvidia/version", F_OK) == 0) {
            const char *libs[] = {"/usr/lib32/libnvidia-egl-wayland.so.1", "/usr/lib/i386-linux-gnu/libnvidia-egl-wayland.so.1"};
            int have = 0;
            for (size_t i = 0; i < sizeof libs / sizeof *libs; i++) have |= access(libs[i], F_OK) == 0;
            if (!have) {
                SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11,wayland");
                fprintf(stderr, "platform: NVIDIA without 32-bit egl-wayland: using X11 for GPU OpenGL\n");
            }
        }
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    input_init();  /* keyboard + controllers: config, mapping DB, already connected pads */
    int debug = getenv("ALBW_GL_DEBUG") != NULL;
#if defined(__ANDROID__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG |
                                              (debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0));
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* the window needs no depth/stencil: the game renders into the screen targets */
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    snprintf(g_layout_base_title, sizeof g_layout_base_title, "%s", title);
    int have_layout_cfg = layout_load(&g_layout_cfg, NULL) == 0;  /* defaults if there is no layout.cfg yet */
    /* Steam Deck Game Mode / any gamescope session: one window is shown at a time, full screen, and
     * there may be no keyboard. Joined window, full screen (this run only); on the first start the
     * large-top layout (the bottom screen stays reachable through the touchscreen). */
    const char *xdg_desk = getenv("XDG_CURRENT_DESKTOP");
    g_gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY") || (xdg_desk && strstr(xdg_desk, "gamescope")) ||
                  (getenv("SteamDeck") && getenv("SteamGamepadUI"));
    if (g_gamescope && !have_layout_cfg) g_layout_cfg.preset = LAYOUT_LARGE_TOP;
    {   /* internal resolution: ALBW_RENDER_SCALE, else --render-scale, else layout.cfg render_scale */
        const char *rs = getenv("ALBW_RENDER_SCALE");
        int r = rs && atoi(rs) >= 1 && atoi(rs) <= 8 ? atoi(rs) : c->render_scale >= 1 ? c->render_scale : g_layout_cfg.render_scale;
        cfg.render_scale = r >= 1 && r <= 8 ? r : 1;
        g_want_render_scale = cfg.render_scale;
    }
    g_separate = g_layout_cfg.separate;
    const char *wm = getenv("ALBW_WINDOWS");  /* for this run only: not written to layout.cfg */
    if (wm && *wm) {
        int v = layout_windows_from_key(wm);
        if (v >= 0) g_separate = v;
        else fprintf(stderr, "ALBW_WINDOWS=%s: expected separate or joined\n", wm);
    }
    if (g_layout_cfg.single_screen) g_separate = 0;   /* single-screen mode: one window (saved choice kept) */
    if (g_gamescope) g_separate = 0;
    if (g_layout_cfg.widescreen) { layout_set_top_w(LAYOUT_WIDE_TOP_W); render_set_widescreen(LAYOUT_WIDE_TOP_W); }
    if (g_layout_cfg.preset != LAYOUT_TOP_ONLY && g_layout_cfg.preset != LAYOUT_BOTTOM_ONLY)
        g_last_two_preset = g_layout_cfg.preset;
    g_joined_w = TOP_W * cfg.scale;
    g_joined_h = SCREEN_H * 2 * cfg.scale;
    if (plat_windows_init()) return -1;
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fprintf(stderr, "Could not create an OpenGL 3.3 core context (%s).\n"
                "This port needs OpenGL 3.3: any Intel GPU since 2011 (HD 2000+), AMD since 2009 (HD 5000+)\n"
                "or NVIDIA since 2008 (GeForce 400+), with Mesa or the vendor driver.\n", SDL_GetError());
        return -1;
    }
    if (gl_load((void *(*)(const char *))SDL_GL_GetProcAddress)) return -1;
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    fprintf(stderr, "GL: %s | %s | %s (context %d.%d)\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
            glGetString(GL_VERSION), major, minor);
    {
        const char *rnd = (const char *)glGetString(GL_RENDERER);
        if (rnd && (strstr(rnd, "llvmpipe") || strstr(rnd, "softpipe") || strstr(rnd, "SWR")))
            fprintf(stderr, "GL: WARNING: software rasteriser (%s): expect ~20-25 fps and every core busy.\n"
                    "    On NVIDIA + Wayland the 32-bit EGL needs lib32 egl-wayland; or run with SDL_VIDEO_DRIVER=x11.\n", rnd);
    }
#if defined(__ANDROID__)
    if (major * 10 + minor < 32) {
        fprintf(stderr, "OpenGL ES %d.%d is too old: 3.2 is required.\n", major, minor);
        return -1;
    }
#else
    if (major * 10 + minor < 33) {
        fprintf(stderr, "OpenGL %d.%d is too old: 3.3 is required.\n", major, minor);
        return -1;
    }
#endif
    if (debug) gl_enable_debug((void *(*)(const char *))SDL_GL_GetProcAddress);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    target_init(&screens[SCREEN_TOP], TOP_W * cfg.render_scale, SCREEN_H * cfg.render_scale);
    target_init(&screens[SCREEN_BOTTOM], BOTTOM_W * cfg.render_scale, SCREEN_H * cfg.render_scale);
    blit_prog = gl_program(BLIT_VS, BLIT_FS);
    glGenVertexArrays(1, &blit_vao);
    /* separate: the bottom screen's window (shares ctx); sets the swap intervals in both modes */
    plat_set_windows(g_separate, 0);
    if (g_gamescope) {
        SDL_SetWindowFullscreen(win, true);
        fprintf(stderr, "platform: gamescope / Steam Deck Game Mode: one full-screen window; "
                "menu: both sticks clicked (L3+R3)\n");
    }
    next_frame_ns = plat_ticks_ns();
    return 0;
}

void plat_shutdown(void) {
    input_shutdown();
    if (g_geom_dirty_ns && layout_save(&g_layout_cfg, NULL)) fprintf(stderr, "layout: cannot save %s\n", layout_default_path());
    if (win && ctx) SDL_GL_MakeCurrent(win, ctx);
    if (win_bottom) SDL_DestroyWindow(win_bottom);
    if (ctx) SDL_GL_DestroyContext(ctx);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

uint64_t plat_ticks_ns(void) { return SDL_GetTicksNS(); }

/* ------------------------------------------------------------------ window mode
 * Separate (default): the top screen in its own window (400x240 x scale), the bottom screen in
 * another (320x240 x scale), each resizable, the screen letterboxed in it; the mouse touches the
 * bottom screen in the bottom window. Joined: one window, laid out by the presets / custom
 * layout below, exactly as before separate windows existed.
 *   F5              join / separate the windows (saved in layout.cfg "windows")
 *   ALBW_WINDOWS=separate|joined   overrides layout.cfg for one run (not saved unless F5 is used)
 * With separate windows the presets that place two screens in one window have no effect (they
 * stay the joined layout); top_only / bottom_only hide the other screen's window, and F2 cycles
 * both windows -> top only -> bottom only. F3 joins the windows and opens the layout editor; F4
 * (swap) and Ctrl+drag only apply to the joined window. Keyboard, controllers, the frontend
 * menu binding and the F-keys work from either window (SDL keyboard state is global); closing
 * either window quits. Each separate window's size (and position, where the windowing system
 * reports one: not Wayland) is remembered in layout.cfg. */

#define PLAT_WINDOW_FLAGS (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)

static SDL_Window *plat_window_of(int screen) { return screen == SCREEN_TOP ? win : win_bottom; }

/* Wayland neither reports nor lets clients choose window positions */
static int plat_pos_known(void) {
    const char *d = SDL_GetCurrentVideoDriver();
    return !d || strcmp(d, "wayland") != 0;
}

static void plat_separate_size(int s, int *w, int *h) {
    const LayoutWindowGeom *g = &g_layout_cfg.window[s];
    if (g->has_size) {
        *w = g->w;
        *h = g->h;
    } else {
        *w = (s == SCREEN_TOP ? TOP_W : BOTTOM_W) * cfg.scale;
        *h = SCREEN_H * cfg.scale;
    }
}

/* a saved position is used only if the window would still be on a connected display */
static int plat_saved_pos(int s, int *x, int *y) {
    const LayoutWindowGeom *g = &g_layout_cfg.window[s];
    if (!g->has_pos || !plat_pos_known()) return 0;
    int w, h, n = 0, ok = 0;
    plat_separate_size(s, &w, &h);
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    for (int i = 0; ids && i < n && !ok; i++) {
        SDL_Rect r;
        int cx = g->x + w / 2;
        if (SDL_GetDisplayUsableBounds(ids[i], &r) && cx >= r.x && cx < r.x + r.w && g->y >= r.y && g->y < r.y + r.h) ok = 1;
    }
    SDL_free(ids);
    if (ok) {
        *x = g->x;
        *y = g->y;
    }
    return ok;
}

static void plat_set_size_restored(SDL_Window *w, int ww, int wh) {
    if (SDL_GetWindowFlags(w) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) SDL_RestoreWindow(w);
    SDL_SetWindowSize(w, ww, wh);
}

/* First window: the joined window, or the top screen's window. Created hidden so that it can be
 * placed before it is mapped (window managers honour a position best at map time). */
static int plat_windows_init(void) {
    int w = g_joined_w, h = g_joined_h, x, y;
    if (g_separate) plat_separate_size(SCREEN_TOP, &w, &h);
    win = SDL_CreateWindow(g_layout_base_title, w, h, PLAT_WINDOW_FLAGS | SDL_WINDOW_HIDDEN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    if (g_separate && plat_saved_pos(SCREEN_TOP, &x, &y)) {
        SDL_SetWindowPosition(win, x, y);
    } else if (g_separate && plat_pos_known()) {
        /* first separate run: centre the pair (top above bottom) on the primary display */
        int bw, bh;
        SDL_Rect u;
        plat_separate_size(SCREEN_BOTTOM, &bw, &bh);
        int pair_h = h + bh + 2 * 32;   /* both windows + typical title bars (positions are the client area's) */
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &u) && pair_h <= u.h)
            SDL_SetWindowPosition(win, u.x + (u.w - w) / 2, u.y + (u.h - pair_h) / 2 + 32);
        else
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (!g_separate || layout_separate_visible(&g_layout_cfg, SCREEN_TOP)) SDL_ShowWindow(win);
    return 0;
}

/* default place of the bottom window: under the top one (centred), or to its right if the
 * display has no room below */
static void plat_place_bottom(void) {
    int x, y;
    if (plat_saved_pos(SCREEN_BOTTOM, &x, &y)) {
        SDL_SetWindowPosition(win_bottom, x, y);
        return;
    }
    if (!plat_pos_known()) return;
    int tx, ty, tw, th, bw, bh, bt = 0, bl = 0, bb = 0, br = 0;
    SDL_SyncWindow(win);  /* the window manager has placed (and decorated) the top window */
    SDL_GetWindowPosition(win, &tx, &ty);
    SDL_GetWindowSize(win, &tw, &th);
    SDL_GetWindowSize(win_bottom, &bw, &bh);
    if (!SDL_GetWindowBordersSize(win, &bt, &bl, &bb, &br)) bt = 30;  /* unknown: a typical title bar */
    x = tx + (tw - bw) / 2;
    y = ty + th + bb + bt;   /* the bottom window's title bar just below the top window's frame */
    SDL_Rect u;
    SDL_DisplayID d = SDL_GetDisplayForWindow(win);
    if (d && SDL_GetDisplayUsableBounds(d, &u) && y + bh > u.y + u.h) {
        x = tx + tw + br + bl;
        y = ty;
    }
    SDL_SetWindowPosition(win_bottom, x, y);
}

/* Swap intervals: only one window waits for vsync (the top one, or the bottom one while the top
 * is hidden); the other presents immediately, so two swaps per frame never wait twice. */
static void plat_apply_vsync(void) {
    SDL_Window *primary = win;
    if (g_separate && win_bottom && !layout_separate_visible(&g_layout_cfg, SCREEN_TOP)) primary = win_bottom;
    SDL_Window *ws[2] = {win, win_bottom};
    for (int i = 0; i < 2; i++) {
        if (!ws[i]) continue;
        SDL_GL_MakeCurrent(ws[i], ctx);
        if (ws[i] == primary && cfg.vsync) {  /* adaptive if the driver has it, else on, else off */
            if (!SDL_GL_SetSwapInterval(-1) && !SDL_GL_SetSwapInterval(1)) SDL_GL_SetSwapInterval(0);
        } else {
            SDL_GL_SetSwapInterval(0);
        }
    }
    SDL_GL_MakeCurrent(win, ctx);
}

/* separate: show the windows of the screens top_only / bottom_only leave visible */
static void plat_windows_visibility(void) {
    if (!g_separate || !win_bottom) {
        SDL_ShowWindow(win);
        return;
    }
    SDL_Window *focus = SDL_GetKeyboardFocus();
    for (int s = 0; s < 2; s++) {
        SDL_Window *w = plat_window_of(s);
        if (layout_separate_visible(&g_layout_cfg, s)) SDL_ShowWindow(w);
        else SDL_HideWindow(w);
    }
    /* keep the keyboard on one of our windows when the focused one was hidden */
    for (int s = 0; s < 2; s++)
        if (focus == plat_window_of(s) && !layout_separate_visible(&g_layout_cfg, s))
            SDL_RaiseWindow(plat_window_of(1 - s));
}

/* Switch between joined and separate. save: also make it the saved choice (F5); at start-up the
 * mode comes from layout.cfg / ALBW_WINDOWS and nothing is written. */
static void plat_set_windows(int separate, int save) {
    int was = g_separate && win_bottom;
    SDL_Window *focus = SDL_GetKeyboardFocus();
    int had_focus = focus && (focus == win || focus == win_bottom);
    if (separate && !was) {
        if (win_bottom == NULL && !(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) &&
            !g_separate)
            SDL_GetWindowSize(win, &g_joined_w, &g_joined_h);   /* runtime split: remember the joined size */
        if (g_layout_editing) {
            g_layout_editing = 0;
            layout_edit_end(&g_layout_edit);
            plat_layout_cursor(0);
        }
        int bw, bh, x, y;
        plat_separate_size(SCREEN_BOTTOM, &bw, &bh);
        win_bottom = SDL_CreateWindow(g_layout_base_title, bw, bh, PLAT_WINDOW_FLAGS | SDL_WINDOW_HIDDEN);
        if (!win_bottom) {
            fprintf(stderr, "SDL_CreateWindow (bottom screen): %s; keeping one window\n", SDL_GetError());
            g_separate = 0;
        } else {
            if (!g_separate) {   /* runtime split: the joined window becomes the top screen's window */
                int tw, th;
                plat_separate_size(SCREEN_TOP, &tw, &th);
                plat_set_size_restored(win, tw, th);
                if (plat_saved_pos(SCREEN_TOP, &x, &y)) SDL_SetWindowPosition(win, x, y);
            }
            g_separate = 1;
            SDL_SetWindowMinimumSize(win, TOP_W / 4, SCREEN_H / 4);
            SDL_SetWindowMinimumSize(win_bottom, BOTTOM_W / 4, SCREEN_H / 4);
            plat_place_bottom();
        }
    } else if (!separate && (was || g_separate)) {
        SDL_GL_MakeCurrent(win, ctx);   /* never leave the context current on a destroyed window */
        if (win_bottom) SDL_DestroyWindow(win_bottom);
        win_bottom = NULL;
        g_separate = 0;
        plat_set_size_restored(win, g_joined_w, g_joined_h);
    }
    plat_windows_visibility();
    /* keep the keyboard on our side: window managers may focus the new bottom window, or another
     * application when the focused bottom window goes away */
    if (had_focus) {
        SDL_Window *keep = focus == win_bottom ? win_bottom : win;   /* win_bottom is NULL once joined */
        if (g_separate && !layout_separate_visible(&g_layout_cfg, SCREEN_TOP)) keep = win_bottom;
        SDL_RaiseWindow(keep);
    }
    plat_apply_vsync();
    if (save) {
        g_layout_cfg.separate = g_separate;
        plat_layout_changed();
    } else {
        plat_layout_title();
    }
}

/* the layout inside the window of one separate screen */
static void plat_window_layout(int s, Layout *l) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(plat_window_of(s), &ww, &wh);
    layout_compute_single(&g_layout_cfg, s, ww, wh, l);
}

/* separate window moved / resized: remember it (layout.cfg, written once it settles) */
static void plat_window_geom_event(SDL_WindowID id) {
    if (!g_separate) return;
    for (int s = 0; s < 2; s++) {
        SDL_Window *w = plat_window_of(s);
        if (!w || SDL_GetWindowID(w) != id) continue;
        if (SDL_GetWindowFlags(w) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
            return;
        LayoutWindowGeom g = g_layout_cfg.window[s];
        SDL_GetWindowSize(w, &g.w, &g.h);
        g.has_size = 1;
        if (plat_pos_known()) {
            SDL_GetWindowPosition(w, &g.x, &g.y);
            g.has_pos = 1;
        }
        if (memcmp(&g, &g_layout_cfg.window[s], sizeof g)) {
            g_layout_cfg.window[s] = g;
            g_geom_dirty_ns = plat_ticks_ns();
        }
    }
}

/* separate F2: both windows -> top only -> bottom only (Shift: backwards). "Both" restores the
 * two-screen preset of the joined window, so the joined layout is not lost on the way. */
static void plat_separate_cycle(int back) {
    int p = g_layout_cfg.preset, idx = p == LAYOUT_TOP_ONLY ? 1 : p == LAYOUT_BOTTOM_ONLY ? 2 : 0;
    if (idx == 0) g_last_two_preset = p;
    static const int ONE[3] = {-1, LAYOUT_TOP_ONLY, LAYOUT_BOTTOM_ONLY};
    idx = (idx + (back ? 2 : 1)) % 3;
    g_layout_cfg.preset = idx ? ONE[idx] : g_last_two_preset;
    plat_windows_visibility();
    plat_apply_vsync();
    plat_layout_changed();
}

/* ------------------------------------------------------------------ screen layout (layout.c)
 * Where the two screens go in the joined window: presets or a custom layout, saved in layout.cfg.
 * (F5 and the separate-window meaning of F2-F4: see the window mode section above.)
 *   F1              frontend menu (input.cfg binding; input.c)
 *   Escape          Esc menu: pause + window / layout / filter / fullscreen options, quit
 *                   (menu.c; QUIT binding of input.cfg; ALBW_ESC_QUITS=1 restores quit-on-Esc)
 *   F2 / Shift+F2   next / previous preset
 *   F3              custom layout editor on/off (Esc also leaves it). In the editor:
 *                   left drag = move a screen, drag a corner or right drag = resize,
 *                   wheel = scale, Shift = free aspect, Alt = no snapping
 *   F4              swap the screens (presets) / bring the other screen to the front (custom)
 *   F5              separate windows / one joined window
 *   F9              single-screen mode on / off (SCREEN binding, Tab: show the other screen)
 *   Ctrl + left/right drag   move/resize a screen without entering the editor (-> custom) */

#define LAYOUT_HANDLE 10.0f      /* corner handle size, window points */

static void plat_layout_update(void) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    layout_compute(&g_layout_cfg, ww, wh, &g_layout);
}

static void plat_layout_title(void) {
    char t[512];
    if (g_separate && win_bottom) {
        snprintf(t, sizeof t, "%s - Top%s", g_layout_base_title, g_title_fps);
        SDL_SetWindowTitle(win, t);
        snprintf(t, sizeof t, "%s - Bottom%s", g_layout_base_title, g_title_fps);
        SDL_SetWindowTitle(win_bottom, t);
        return;
    }
    if (!g_layout_editing && g_layout_cfg.preset == LAYOUT_DEFAULT && !g_layout_cfg.swap) snprintf(t, sizeof t, "%s%s", g_layout_base_title, g_title_fps);
    else snprintf(t, sizeof t, "%s%s  [%s%s]%s", g_layout_base_title, g_title_fps, layout_preset_name(g_layout_cfg.preset),
                  g_layout_cfg.swap && g_layout_cfg.preset != LAYOUT_CUSTOM ? ", swapped" : "",
                  g_layout_editing ? "  LAYOUT EDITOR: drag = move, corner/right-drag = resize, wheel = scale, "
                               "Shift = free aspect, Alt = no snap, F3 = done" : "");
    SDL_SetWindowTitle(win, t);
}

static void plat_layout_changed(void) {
    plat_layout_update();
    plat_layout_title();
    if (layout_save(&g_layout_cfg, NULL)) fprintf(stderr, "layout: cannot save %s\n", layout_default_path());
}

/* single-screen mode (layout.cfg single_screen, F9, Esc menu > Video): one screen at a time, the
 * top one; the SCREEN binding (Tab / right-stick click) brings the bottom one up in its place and
 * back. Touch follows the screen that is shown (layout_hit). */
/* At the title or the file select: no player, or the title's hidden player, which the game parks at
 * (1000, 1000, 1000). Used by the single-screen file-select switch (plat_poll). */
static int at_title(void) {
    const float *albw_debug_player_pos(void);
    const float *p = albw_debug_player_pos();
    return !p || (p[0] == 1000.0f && p[1] == 1000.0f && p[2] == 1000.0f);
}

/* Bottom screen brought up for the title's file select (see plat_poll): while set, the game keeps
 * Start and the D-pad (its own file cursor), the bottom-screen selector stays off, and the top
 * screen comes back by itself once Link is in the field. */
static int g_ss_front;

static void plat_single_screen_set(int on) {
    g_layout_cfg.single_screen = on != 0;
    g_layout_cfg.show_bottom = 0;
    g_ss_front = 0;
    if (on) {
        /* always one window: join separate windows (not saved: turning the mode off restores
         * them) and size it for one screen */
        if (g_separate) plat_set_windows(0, 0);
        if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
            int tw, th;
            plat_separate_size(SCREEN_TOP, &tw, &th);
            plat_set_size_restored(win, tw, th);
        }
    } else if (g_layout_cfg.separate && !g_separate) {
        plat_set_windows(1, 0);
    } else if (!g_separate && !(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        plat_set_size_restored(win, g_joined_w, g_joined_h);
    }
    plat_windows_visibility();
    plat_layout_changed();
    fprintf(stderr, "layout: single-screen mode %s\n", on ? "on (Tab / right-stick click: other screen)" : "off");
}
/* widescreen (layout.cfg widescreen, F10, Esc menu > Video): a 16:9 top screen. The renderer
 * draws the top screen LAYOUT_WIDE_TOP_W wide and the game widens its projections to match
 * (render.c albw_ws_factor): more of the world at the sides, nothing stretched. */
static int g_ws_pending;   /* 1 off, 2 on: applied between frames (plat_frame_end_hook) */
static void plat_widescreen_apply(int on);
static void plat_widescreen_set(int on) {
    g_ws_pending = on ? 2 : 1;
    if (!render_ready()) { g_ws_pending = 0; plat_widescreen_apply(on); }
}
/* render.c render_frame_end: after the present, before the next frame's game update builds its
 * projections (pumping events can happen mid-frame) */
void plat_frame_end_hook(void) {
    if (!g_ws_pending) return;
    int on = g_ws_pending == 2;
    g_ws_pending = 0;
    if (on != g_layout_cfg.widescreen) plat_widescreen_apply(on);
}
static void plat_widescreen_apply(int on) {
    g_layout_cfg.widescreen = on != 0;
    int old_w = TOP_W;
    int w = on ? LAYOUT_WIDE_TOP_W : 400;
    layout_set_top_w(w);
    render_set_widescreen(w);
    Target *t = &screens[SCREEN_TOP];
    glDeleteFramebuffers(1, &t->fbo);
    glDeleteRenderbuffers(1, &t->depth);
    glDeleteTextures(1, &t->color);
    target_init(t, TOP_W * cfg.render_scale, SCREEN_H * cfg.render_scale);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_joined_w = TOP_W * cfg.scale;
    SDL_Window *tw = win;   /* the top screen's window in both modes */
    if (!(SDL_GetWindowFlags(tw) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int ww, wh;
        if (g_separate || g_layout_cfg.single_screen) {
            /* keep the user's window size, only widened / narrowed with the screen */
            LayoutWindowGeom *gw = &g_layout_cfg.window[SCREEN_TOP];
            if (gw->has_size) gw->w = (int)((long)gw->w * w / old_w);
            plat_separate_size(SCREEN_TOP, &ww, &wh);
            if (!(g_layout_cfg.single_screen && g_layout_cfg.show_bottom))   /* bottom shown: its size stays */
                plat_set_size_restored(tw, ww, wh);
        } else {
            plat_set_size_restored(tw, g_joined_w, g_joined_h);
        }
    }
    plat_layout_changed();
    fprintf(stderr, "layout: widescreen %s (top screen %dx240)\n", on ? "on" : "off", w);
}
/* Esc > File > Reset game: like the console's reset, the game starts over from the boot (what was
 * not saved in game is lost; save files and save states stay). Two presses within 3 s. The
 * process restarts itself (same arguments, environment and working directory): a clean boot,
 * nothing of the running game's memory or threads carried over. */
extern char **albw_argv;
static uint64_t g_reset_armed_ns;
static int plat_reset_armed(void) { return g_reset_armed_ns && plat_ticks_ns() - g_reset_armed_ns < 3000000000ull; }
static void route_saveset(const char *name);   /* menu.c */
static void plat_restart(void) {
    fprintf(stderr, "menu: restarting\n");
    fflush(NULL);
    plat_shutdown();
    execv("/proc/self/exe", albw_argv);
    fprintf(stderr, "menu: restart failed (%s)\n", strerror(errno));
    _exit(1);
}
static void plat_reset_press(void) {
    if (!plat_reset_armed()) { g_reset_armed_ns = plat_ticks_ns(); return; }
    fprintf(stderr, "menu: reset game\n");
    plat_restart();
}
static void plat_single_screen_flip(void) {
    if (!g_layout_cfg.single_screen) return;
    g_layout_cfg.show_bottom ^= 1;
    g_ss_front = 0;   /* a manual flip takes over from the file-select switch */
    extern long g_plat_frames;
    if (getenv("ALBW_SEL_TRACE")) fprintf(stderr, "selector: f=%ld single-screen shows the %s screen\n", g_plat_frames, g_layout_cfg.show_bottom ? "bottom" : "top");
    plat_windows_visibility();
    plat_layout_update();
    plat_layout_title();
}

static void plat_layout_cursor(int which) {
    static const SDL_SystemCursor ids[4] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_MOVE,
                                            SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE};
    if (!g_layout_cursor[which]) g_layout_cursor[which] = SDL_CreateSystemCursor(ids[which]);
    if (g_layout_cursor[which]) SDL_SetCursor(g_layout_cursor[which]);
}

/* F12: save both screens (the last finished frame) to ~/.local/share/albw-port/screenshots/
 * shot_<time>_top.ppm / _bottom.ppm. Kept out of plat_end_frame (save states record its code). */
static void __attribute__((noinline)) plat_screenshot_now(void) {
    const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
    char dir[600];
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/albw-port/screenshots", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/albw-port/screenshots", home ? home : ".");
    char cmd[700];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
    for (int i = 0; i < 2; i++) {
        Target *t = &screens[i];
        unsigned char *px = malloc((size_t)t->w * t->h * 4);
        if (!px) return;
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glReadPixels(0, 0, t->w, t->h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
        char path[760];
        snprintf(path, sizeof path, "%s/shot_%s_%s.ppm", dir, stamp, i ? "bottom" : "top");
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
            for (int y = t->h - 1; y >= 0; y--)
                for (int x = 0; x < t->w; x++) fwrite(px + ((size_t)y * t->w + x) * 4, 1, 3, f);
            fclose(f);
            fprintf(stderr, "screenshot: %s\n", path);
        }
        free(px);
    }
}

/* Layout keys and mouse editing. Returns 1 if the event was consumed (not passed to the game). */
static int plat_layout_event(const SDL_Event *e) {
    float pd = SDL_GetWindowPixelDensity(win);
    SDL_Keymod mod = SDL_GetModState();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.key == SDLK_F5) {
            if (!e->key.repeat) {
                if (g_layout_cfg.single_screen) plat_single_screen_set(0);   /* F5 leaves single-screen mode */
                else plat_set_windows(!g_separate, 1);
            }
            return 1;
        }
        if (e->key.key == SDLK_F6 || e->key.key == SDLK_F7) {   /* save / load state (current slot) */
            if (!e->key.repeat && albw_ss_request) albw_ss_request(e->key.key == SDLK_F6 ? 1 : 2, g_ss_slot);
            return 1;
        }
        if (e->key.key == SDLK_F8) {   /* next save-state slot (Shift: previous) */
            if (!e->key.repeat) {
                g_ss_slot = (g_ss_slot + ((mod & SDL_KMOD_SHIFT) ? SS_SLOTS_UI - 1 : 1)) % SS_SLOTS_UI;
                fprintf(stderr, "savestate: slot %d\n", g_ss_slot + 1);
            }
            return 1;
        }
        if (e->key.key == SDLK_F12) {   /* screenshot of both screens */
            if (!e->key.repeat) plat_screenshot_now();
            return 1;
        }
        if (e->key.key == SDLK_F10) {   /* widescreen on / off */
            if (!e->key.repeat) plat_widescreen_set(!g_layout_cfg.widescreen);
            return 1;
        }
        if (e->key.key == SDLK_F9) {   /* single-screen mode on / off */
            if (!e->key.repeat) plat_single_screen_set(!g_layout_cfg.single_screen);
            return 1;
        }
        if (e->key.key == SDLK_F2 && g_separate) {
            if (!e->key.repeat) plat_separate_cycle((mod & SDL_KMOD_SHIFT) != 0);
            return 1;
        }
        if (e->key.key == SDLK_F2) {
            if (!e->key.repeat) {
                int n = LAYOUT_PRESET_COUNT, step = (mod & SDL_KMOD_SHIFT) ? n - 1 : 1;
                g_layout_cfg.preset = (g_layout_cfg.preset + step) % n;
                plat_layout_changed();
            }
            return 1;
        }
        if (e->key.key == SDLK_F3) {
            if (!e->key.repeat) {
                if (g_separate) plat_set_windows(0, 1);  /* the editor arranges the joined window */
                g_layout_editing = !g_layout_editing;
                plat_layout_update();
                if (g_layout_editing) layout_make_custom(&g_layout_cfg, &g_layout);
                layout_edit_end(&g_layout_edit);
                plat_layout_cursor(0);
                plat_layout_changed();
            }
            return 1;
        }
        if (e->key.key == SDLK_F4 && g_separate) {
            if (!e->key.repeat) fprintf(stderr, "layout: F4 (swap) applies to the joined window (F5 joins)\n");
            return 1;
        }
        if (e->key.key == SDLK_F4) {
            if (!e->key.repeat) {
                if (g_layout_cfg.preset == LAYOUT_CUSTOM) g_layout_cfg.custom_front ^= 1;
                else g_layout_cfg.swap ^= 1;
                plat_layout_changed();
            }
            return 1;
        }
        if (e->key.key == SDLK_ESCAPE && g_layout_editing) {  /* leave the editor instead of quitting */
            g_layout_editing = 0;
            layout_edit_end(&g_layout_edit);
            plat_layout_cursor(0);
            plat_layout_changed();
            return 1;
        }
        return 0;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
        plat_window_geom_event(e->window.windowID);
        return 0;
    default:
        break;
    }
    if (g_separate) return 0;   /* the mouse only touches the bottom screen (plat_poll) */
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT && e->button.button != SDL_BUTTON_RIGHT) return g_layout_editing;
        if (!g_layout_editing && !(mod & SDL_KMOD_CTRL)) return 0;
        plat_layout_update();
        if (layout_edit_begin(&g_layout_edit, &g_layout_cfg, &g_layout, e->button.x * pd, e->button.y * pd, LAYOUT_HANDLE * pd,
                              e->button.button == SDL_BUTTON_RIGHT)) {
            plat_layout_title();
            return 1;
        }
        return g_layout_editing;
    case SDL_EVENT_MOUSE_MOTION:
        if (g_layout_edit.mode != LAYOUT_EDIT_NONE) {
            plat_layout_update();
            layout_edit_update(&g_layout_edit, &g_layout_cfg, &g_layout, e->motion.x * pd, e->motion.y * pd, (mod & SDL_KMOD_SHIFT) != 0,
                               (mod & SDL_KMOD_ALT) != 0);
            return 1;
        }
        if (g_layout_editing) {
            int s, c;
            plat_layout_update();
            int m = layout_edit_hit(&g_layout, e->motion.x * pd, e->motion.y * pd, LAYOUT_HANDLE * pd, &s, &c);
            plat_layout_cursor(m == LAYOUT_EDIT_RESIZE ? (c == 0 || c == 3 ? 2 : 3) : m == LAYOUT_EDIT_MOVE ? 1 : 0);
        }
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (g_layout_edit.mode != LAYOUT_EDIT_NONE) {
            layout_edit_end(&g_layout_edit);
            plat_layout_changed();
            return 1;
        }
        return g_layout_editing;
    case SDL_EVENT_MOUSE_WHEEL:
        if (g_layout_editing && e->wheel.y != 0) {
            plat_layout_update();
            int s = layout_hit(&g_layout, e->wheel.mouse_x * pd, e->wheel.mouse_y * pd);
            if (s >= 0) {
                layout_edit_scale(&g_layout_cfg, &g_layout, s, e->wheel.y > 0 ? 1.1f : 1 / 1.1f);
                plat_layout_changed();
            }
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

/* editor overlay: outlines, corner handles and snap guides, drawn with scissored clears */
static void plat_layout_fill(int x, int y, int w, int h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    glScissor(x, g_layout.wh - (y + h), w, h);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void plat_layout_overlay(void) {
    float pd = SDL_GetWindowPixelDensity(win);
    int t = pd > 1.5f ? 3 : 2, hs = (int)(LAYOUT_HANDLE * pd);
    glEnable(GL_SCISSOR_TEST);
    for (int k = 0; k < 2; k++) {
        int s = g_layout.order[k];
        if (!g_layout.visible[s]) continue;
        int x = (int)g_layout.r[s].x, y = (int)g_layout.r[s].y, w = (int)g_layout.r[s].w, h = (int)g_layout.r[s].h;
        float cr = s ? 0.3f : 1.0f, cg = s ? 0.8f : 0.75f, cb = s ? 1.0f : 0.2f;  /* top amber, bottom blue */
        plat_layout_fill(x, y, w, t, cr, cg, cb);
        plat_layout_fill(x, y + h - t, w, t, cr, cg, cb);
        plat_layout_fill(x, y, t, h, cr, cg, cb);
        plat_layout_fill(x + w - t, y, t, h, cr, cg, cb);
        for (int c = 0; c < 4; c++)
            plat_layout_fill((c & 1 ? x + w : x) - hs / 2, (c & 2 ? y + h : y) - hs / 2, hs, hs, 1, 1, 1);
    }
    if (g_layout_edit.guide_x >= 0) plat_layout_fill((int)g_layout_edit.guide_x - 1, 0, 2, g_layout.wh, 1, 0.2f, 0.8f);
    if (g_layout_edit.guide_y >= 0) plat_layout_fill(0, (int)g_layout_edit.guide_y - 1, g_layout.ww, 2, 1, 0.2f, 0.8f);
    glDisable(GL_SCISSOR_TEST);
}

/* presented frames (plat_end_frame calls), the frame numbers of ALBW_SHOT and ALBW_PRESS */
long g_plat_frames; /* platform frame counter (also read by debug traces) */

/* ALBW_PRESS=<frame>:<button>[:<frames>][,...] (debug aid, headless runs): hold a 3DS button from
 * that presented frame on for <frames> frames (default 6). Buttons: A B X Y L R START SELECT UP
 * DOWN LEFT RIGHT; circle pad: CUP CDOWN CLEFT CRIGHT (full deflection + the direction bits,
 * combinable for diagonals). Example: ALBW_PRESS=1800:A,2400:START,3000:CUP:120 */
static uint32_t scripted_keys(void) {
    static const char *spec;
    static int init;
    if (!init) { init = 1; spec = getenv("ALBW_PRESS"); }
    if (!spec) return 0;
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
        {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN},
        {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT},
        {"CUP", KEY_CPAD_UP}, {"CDOWN", KEY_CPAD_DOWN}, {"CLEFT", KEY_CPAD_LEFT}, {"CRIGHT", KEY_CPAD_RIGHT}};
    uint32_t keys = 0;
    for (const char *s = spec; *s;) {
        char *e;
        long f = strtol(s, &e, 10);
        if (e == s || *e != ':') break;
        const char *b = e + 1;
        size_t n = strcspn(b, ":,");
        long len = 6;
        const char *t = b + n;
        if (*t == ':') { len = strtol(t + 1, &e, 10); t = e; }
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
            if (strlen(names[k].name) == n && !strncmp(names[k].name, b, n) && g_plat_frames >= f &&
                g_plat_frames < f + len)
                keys |= names[k].bit;
        s = (*t == ',') ? t + 1 : t;
        if (!*t) break;
    }
    return keys;
}

/* ALBW_ROUTE (debug aid, headless runs): a closed-loop input script that steers Link by his real
 * position, so it reaches the same spot whatever the timing variance (blind ALBW_PRESS routes miss:
 * DIDNT_WORK iteration 17). Steps separated by ';', run one after another:
 *   at F            wait for presented frame F
 *   goto X Z [TOL]  circle pad towards world (X, Z) until within TOL (default 0.3); when Link stops
 *                   getting closer for 90 frames (walked into something) it prints "route: blocked"
 *                   and goes on; after 1800 frames it gives up (prints "route: FAILED", route stops)
 *   wait N          N frames
 *   press BTN [N]   hold a button (ALBW_PRESS names, incl. CUP..) N frames (default 6), then 1 frame off
 *   spawn NAME [DX DZ]  spawn actor profile NAME (e.g. EnemyKeese) at Link + (DX, 0, DZ) (default 0 2)
 *   profiles        list the registered actor profile names
 *   warp X Y Z      set Link's position (debug)
 *   event NAME      start a scripted sequence by flowchart entry name (debug)
 *   actors          list live actors (profile, position)
 *   touch X Y [N]   touch the bottom screen at (X, Y) (0..319, 0..239) for N frames (default 6), then
 *                   1 frame off
 *   save S | load S save state slot S
 *   log             print Link's position
 *   quit            leave the game
 * Link's position: the object of the player update 001EA96C (albw_dbg_player), +0x30 x / y / z
 * (the words its velocity step integrates).
 * Pad mapping (screen up = -Z, right = +X) can be flipped with ALBW_ROUTE_AXES=<sx><sz>, e.g. "+-".
 * ALBW_PLAYER_LOG=N prints the position every N frames. Example (yard state -> vane):
 *   ALBW_ROUTE='at 30;goto 4 12;goto 4 15 0.2;press CDOWN 10;press A;wait 120;save 9;quit' */
__attribute__((weak)) void *albw_dbg_player;
const float *albw_debug_player_pos(void);                         /* runtime/debug_spawn.c */
void *albw_debug_spawn(const char *name, float dx, float dz);
void albw_debug_list_profiles(void);
int albw_debug_preload(const char *name);
void albw_debug_list_actors(void);
static long g_spawn_wait = 45;   /* ALBW_SPAWN_WAIT: frames between the resource request and the create */
static const float *route_player_pos(void) {
    /* the game's own player position (00520FC0); the player-update object + 0x30 used before went
     * stale in Lorule Castle */
    const float *p = albw_debug_player_pos();
    if (p) return p;
    return albw_dbg_player ? (const float *)((const char *)albw_dbg_player + 0x30) : NULL;
}
static uint32_t route_button(const char *name, size_t n) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
        {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN},
        {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT},
        {"CUP", KEY_CPAD_UP}, {"CDOWN", KEY_CPAD_DOWN}, {"CLEFT", KEY_CPAD_LEFT}, {"CRIGHT", KEY_CPAD_RIGHT}};
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (strlen(names[k].name) == n && !strncmp(names[k].name, name, n)) return names[k].bit;
    return 0;
}
static int g_route_touch, g_route_tx, g_route_ty;   /* touch step: applied in plat_poll */
/* returns the buttons to hold this frame; *pad_x / *pad_y get an analogue circle pad (or stay 0) */
static uint32_t route_step(int *pad_x, int *pad_y) {
    static const char *spec, *cur;
    static int init, sx = 1, sz = -1;
    static long t0 = -1, best_f;
    static float best_d;
    *pad_x = *pad_y = 0;
    if (!init) {
        init = 1;
        cur = spec = getenv("ALBW_ROUTE");
        const char *ax = getenv("ALBW_ROUTE_AXES");
        if (ax && ax[0] && ax[1]) { sx = ax[0] == '-' ? -1 : 1; sz = ax[1] == '-' ? -1 : 1; }
    }
    const char *pl = getenv("ALBW_PLAYER_LOG");   /* each call: a cached static comes back from save states */
    long log_every = pl ? atol(pl) : 0;
    const float *pp = route_player_pos();
    if (log_every > 0 && pp && g_plat_frames % log_every == 0)
        fprintf(stderr, "player f=%ld pos %.3f %.3f %.3f obj %p\n", g_plat_frames, pp[0], pp[1], pp[2], albw_dbg_player);
    if (!cur || !*cur) return 0;
    while (*cur == ';' || *cur == ' ') cur++;
    if (!*cur) return 0;
    char op[16] = {0}, arg[16] = {0};
    double a = 0, b = 0, c = 0;
    int n = sscanf(cur, " %15[^ ;]", op);
    const char *rest = cur + strcspn(cur, " ;");
    size_t len = strcspn(cur, ";");
    char step[128];
    snprintf(step, sizeof step, "%.*s", (int)(len < 127 ? len : 127), cur);
    if (t0 < 0) { t0 = g_plat_frames; best_d = 1e30f; best_f = g_plat_frames; }
    long el = g_plat_frames - t0;
    int done = 0;
    uint32_t keys = 0;
    (void)n;
    g_route_touch = 0;
    if (!strcmp(op, "touch")) {
        long hold = 6;
        int tx = 0, ty = 0;
        int k = sscanf(rest, " %d %d %ld", &tx, &ty, &hold);
        if (k < 3) hold = 6;
        if (el < hold) { g_route_touch = 1; g_route_tx = tx; g_route_ty = ty; }
        done = el >= hold + 1;
    } else if (!strcmp(op, "spawn")) {
        char nm[64] = {0};
        double dx = 0, dz = 2;
        sscanf(rest, " %63[^ ;] %lf %lf", nm, &dx, &dz);
        if (el == 0) { albw_debug_preload(nm); const char *w = getenv("ALBW_SPAWN_WAIT"); if (w) g_spawn_wait = atol(w); }
        if (el >= g_spawn_wait) { albw_debug_spawn(nm, (float)dx, (float)dz); done = 1; }
    } else if (!strcmp(op, "warp")) {   /* debug: set Link's position (x y z) */
        double wx = 0, wy = 0, wz = 0;
        sscanf(rest, " %lf %lf %lf", &wx, &wy, &wz);
        float *pp = (float *)albw_debug_player_pos();
        if (pp) { pp[0] = (float)wx; pp[1] = (float)wy; pp[2] = (float)wz; }
        fprintf(stderr, "route: warp to %.2f %.2f %.2f%s\n", wx, wy, wz, pp ? "" : " (no player)");
        done = 1;
    } else if (!strcmp(op, "scene")) {
        /* debug: scene COURSE SCENE POINT [TRANSITION] - SCENE as in the file names (DungeonEast3 = 3;
         * the game's own scene field counts from 0). The game's own scene change, as an exit zone
         * (005416A0) makes it: 004EF418(scene manager *data_00709DF8, &{u8 course, s32 scene @+4,
         * u16 point @+8}, transition, 0); the scene manager applies it (004EECD4: loads
         * <Course><scene>.szs and its stage, Link appears at the entry point). COURSE: a number or
         * a name (FieldLight .. DungeonBoss: the order of the game's course name table, EU 0x120398). Entry
         * points: the stage's System objects ID 7, ARG[0] (World/Byaml/<Course><scene>_stage.byaml). */
        static const char *const courses[] = {"FieldLight", "FieldDark", "IndoorLight", "IndoorDark", "CaveLight",
            "CaveDark", "Dummy7", "CrossBattle", "Demo", "DungeonEast", "DungeonWind", "DungeonHera", "DungeonCastle",
            "DungeonDark", "DungeonWater", "DungeonDokuro", "DungeonHagure", "DungeonIce", "DungeonSand", "DungeonKame",
            "DungeonGanon", "DungeonBoss", "AttractionLight", "AttractionDark"};
        extern char data_00709DF8[];
        int func_004EF418(int mgr, uint8_t *loc, int transition, int flag);
        char cn[32] = "";
        int scene = 1, point = 0, tr = 0;
        sscanf(rest, " %31s %d %d %d", cn, &scene, &point, &tr);
        int course = -1;
        for (int i = 0; i < (int)(sizeof courses / sizeof *courses); i++)
            if (!strcasecmp(cn, courses[i])) course = i;
        if (course < 0 && cn[0] >= '0' && cn[0] <= '9') course = atoi(cn);
        uint32_t mgr = *(uint32_t *)data_00709DF8;
        if (course < 0 || !mgr) {
            fprintf(stderr, "route: scene %s: %s\n", cn, course < 0 ? "unknown course" : "no scene manager");
        } else {
            _Alignas(4) uint8_t loc[12] = {0};
            loc[0] = (uint8_t)course;
            int scene0 = scene - 1;   /* the game counts scenes from 0: DungeonEast3 = scene 2 */
            memcpy(loc + 4, &scene0, 4);
            uint16_t pt = (uint16_t)point;
            memcpy(loc + 8, &pt, 2);
            int ok = func_004EF418((int)mgr, loc, tr, 0);
            fprintf(stderr, "route: scene: course %d (%s) scene %d point %d transition %d: %s\n", course,
                    course < (int)(sizeof courses / sizeof *courses) ? courses[course] : "?", scene, point, tr,
                    ok ? "requested" : "refused");
        }
        done = 1;
    } else if (!strcmp(op, "event")) {   /* debug: start a flowchart entry (SCRIPTED_EVENTS.md) */
        extern int albw_debug_event(const char *name);
        char nm[128] = "";
        sscanf(rest, " %127[^; ]", nm);
        albw_debug_event(nm);
        done = 1;
    } else if (!strcmp(op, "saveset")) {   /* saveset <name>: Esc > File > Save set, then Load (restarts the game) */
        char nm[64] = "";
        sscanf(rest, " %63[^; ]", nm);
        route_saveset(nm);
        done = 1;
    } else if (!strcmp(op, "speed")) {   /* speed <percent>: like ALBW_SPEED, after a state load (0 = unlimited) */
        g_dbg_speed = atoi(rest);
        done = 1;
    } else if (!strcmp(op, "reset")) {   /* Esc > File > Reset game (both presses) */
        plat_reset_press();
        plat_reset_press();
        done = 1;
    } else if (!strcmp(op, "actors")) {
        albw_debug_list_actors();
        done = 1;
    } else if (!strcmp(op, "profiles")) {
        albw_debug_list_profiles();
        done = 1;
    } else if (!strcmp(op, "at")) {
        done = g_plat_frames >= atol(rest);
    } else if (!strcmp(op, "wait")) {
        done = el >= atol(rest);
    } else if (!strcmp(op, "press") || !strcmp(op, "hold")) {   /* hold: no release frame after it */
        long hold = 6;
        int k = sscanf(rest, " %15[^ ;] %ld", arg, &hold);
        if (k < 1) hold = 6;
        uint32_t bit = 0;   /* "A+CDOWN": several buttons at once */
        for (const char *b = arg; *b;) {
            size_t n = strcspn(b, "+");
            bit |= route_button(b, n);
            b += n;
            if (*b == '+') b++;
        }
        if (el < hold) keys = bit;
        done = op[0] == 'h' ? el >= hold - 1 : el >= hold + 1;
        if (bit & (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT) && el < hold) {
            *pad_x = (bit & KEY_CPAD_RIGHT ? 156 : 0) - (bit & KEY_CPAD_LEFT ? 156 : 0);
            *pad_y = (bit & KEY_CPAD_UP ? 156 : 0) - (bit & KEY_CPAD_DOWN ? 156 : 0);
        }
    } else if (!strcmp(op, "goto")) {
        c = 0.3;
        sscanf(rest, "%lf %lf %lf", &a, &b, &c);
        if (!pp) {
            if (el > 1800) { fprintf(stderr, "route: FAILED %s (no player)\n", step); cur = ""; return 0; }
        } else {
            float dx = (float)a - pp[0], dz = (float)b - pp[2];
            float d = sqrtf(dx * dx + dz * dz);
            if (d < best_d - 0.05f) { best_d = d; best_f = g_plat_frames; }
            if (d <= c) {
                fprintf(stderr, "route: reached (%.2f, %.2f) at f=%ld pos %.3f %.3f %.3f\n", a, b, g_plat_frames,
                        pp[0], pp[1], pp[2]);
                done = 1;
            } else if (el > 1800) {
                fprintf(stderr, "route: FAILED %s at f=%ld pos %.3f %.3f %.3f (timeout)\n", step,
                        g_plat_frames, pp[0], pp[1], pp[2]);
                cur = "";
                return 0;
            } else if (g_plat_frames - best_f > 90) {   /* walked into something: go on from here */
                fprintf(stderr, "route: blocked %s at f=%ld pos %.3f %.3f %.3f\n", step, g_plat_frames, pp[0],
                        pp[1], pp[2]);
                done = 1;
            } else {
                /* full deflection far away, slower in the last unit so it does not overshoot */
                float m = d > 1.0f ? 156.0f : 60.0f + 96.0f * d;
                float px = sx * dx / d * m, py = sz * dz / d * m;
                *pad_x = (int)px;
                *pad_y = (int)py;
                if (*pad_x > 40) keys |= KEY_CPAD_RIGHT;
                if (*pad_x < -40) keys |= KEY_CPAD_LEFT;
                if (*pad_y > 40) keys |= KEY_CPAD_UP;
                if (*pad_y < -40) keys |= KEY_CPAD_DOWN;
            }
        }
    } else if (!strcmp(op, "save") || !strcmp(op, "load")) {
        if (albw_ss_request) albw_ss_request(op[0] == 's' ? 1 : 2, atoi(rest));
        fprintf(stderr, "route: %s at f=%ld\n", step, g_plat_frames);
        done = 1;
    } else if (!strcmp(op, "log")) {
        {   /* + where: the scene manager's current {course, scene} (004EECD4: +0x18 / +0x1C, scene from 0) */
            extern char data_00709DF8[];
            uint32_t mgr = *(uint32_t *)data_00709DF8;
            if (mgr) fprintf(stderr, "route: f=%ld course %d scene %d (file %d)\n", g_plat_frames,
                             *(int8_t *)(uintptr_t)(mgr + 0x18), *(int32_t *)(uintptr_t)(mgr + 0x1C),
                             *(int32_t *)(uintptr_t)(mgr + 0x1C) + 1);
        }
        if (pp) fprintf(stderr, "route: f=%ld pos %.3f %.3f %.3f\n", g_plat_frames, pp[0], pp[1], pp[2]);
        else fprintf(stderr, "route: f=%ld no player\n", g_plat_frames);
        done = 1;
    } else if (!strcmp(op, "quit")) {
        fprintf(stderr, "route: quit at f=%ld\n", g_plat_frames);
        g_quit_req = 1;
        done = 1;
    } else {
        fprintf(stderr, "route: unknown step '%s', route stopped\n", step);
        cur = "";
        return 0;
    }
    if (done) { cur += len; t0 = -1; }
    return keys;
}

#include "selector.c"   /* single-screen bottom-screen selector (D-pad + A) */

int plat_poll(void) {
    if (g_quit_req) return 0;   /* "Quit game" in the Esc menu */
    uint32_t prev = in.held;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (plat_layout_event(&e)) continue;  /* layout keys (F2-F4) and layout editing with the mouse */
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return 0;  /* either window */
        input_handle_event(&e);  /* controller hotplug, MENU/QUIT keys, binding capture */
    }
    /* buttons (keyboard kb, controllers gp), circle pad (+ its direction bits cp), C-stick,
     * motion, frontend menu: input.c, mapping from input.cfg */
    uint32_t kb, gp, cp;
    int quit_ev = !input_update(&in, &kb, &gp, &cp);
    {   /* ALBW_INPUT_TRACE=1: every change of the keyboard / controller button words (which source
         * pressed Start, Select, ...); read per call (a save state restores statics) */
        static uint32_t pkb, pgp;
        if ((kb != pkb || gp != pgp) && getenv("ALBW_INPUT_TRACE"))
            fprintf(stderr, "input: f=%ld keyboard %#x controller %#x%s%s\n", g_plat_frames, kb, gp,
                    ((kb | gp) & KEY_START) ? " START" : "", ((kb | gp) & KEY_SELECT) ? " SELECT" : "");
        pkb = kb; pgp = gp;
    }
    /* QUIT binding (Escape): the Esc menu, not an exit. The MENU binding (controller: Guide, or both
     * sticks clicked; F1) opens it as well: without a keyboard (Steam Deck) it was out of reach */
    if (quit_ev || in.menu) {
        static int esc_quits = -1;
        if (esc_quits < 0) esc_quits = getenv("ALBW_ESC_QUITS") != NULL;
        if (esc_quits && quit_ev) return 0;
        g_menu_open = 1;
        g_menu_cur = 0;
        g_menu_sel = 0;
        g_menu_note[0] = 0;
    }
    if (in.screen) plat_single_screen_flip();
    if (g_geom_dirty_ns && plat_ticks_ns() - g_geom_dirty_ns > 500000000ull) {  /* moves settled */
        g_geom_dirty_ns = 0;
        if (layout_save(&g_layout_cfg, NULL)) fprintf(stderr, "layout: cannot save %s\n", layout_default_path());
    }

    /* touch: left mouse button on the bottom screen, wherever the layout draws it (separate: in
     * the bottom window, in its own coordinates). A touch starts inside the screen and, while held,
     * follows the pointer clamped to the screen's edges (SDL captures the mouse while a button is
     * held, so the coordinates stay relative to the window the press started in). */
    static int touching;
    float mx, my;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    int tx, ty, inside, pressed;
    if (g_separate) {
        SDL_Window *mw = SDL_GetMouseFocus();
        Layout bl;
        int vis = win_bottom && mw == win_bottom && layout_separate_visible(&g_layout_cfg, SCREEN_BOTTOM);
        float pd = vis ? SDL_GetWindowPixelDensity(win_bottom) : 1;
        if (vis) plat_window_layout(SCREEN_BOTTOM, &bl);
        inside = vis && layout_to_screen(&bl, SCREEN_BOTTOM, mx * pd, my * pd, &tx, &ty);
        pressed = vis && (mb & SDL_BUTTON_LMASK);
    } else {
        float pd = SDL_GetWindowPixelDensity(win);
        plat_layout_update();
        inside = layout_to_screen(&g_layout, SCREEN_BOTTOM, mx * pd, my * pd, &tx, &ty);
        pressed = (mb & SDL_BUTTON_LMASK) && !g_layout_editing && g_layout_edit.mode == LAYOUT_EDIT_NONE &&
                  g_layout.visible[SCREEN_BOTTOM];
    }
    touching = pressed && (inside || touching);
    in.touch = touching;
    if (in.touch) {
        in.touch_x = tx;
        in.touch_y = ty;
    }
    uint32_t sk = scripted_keys();
    if (sk & (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)) {  /* scripted circle pad */
        in.cpad_x = (int16_t)((sk & KEY_CPAD_RIGHT ? 156 : 0) - (sk & KEY_CPAD_LEFT ? 156 : 0));
        in.cpad_y = (int16_t)((sk & KEY_CPAD_UP ? 156 : 0) - (sk & KEY_CPAD_DOWN ? 156 : 0));
    }
    int rpx, rpy;
    uint32_t rk = route_step(&rpx, &rpy);   /* ALBW_ROUTE autopilot */
    if (rpx || rpy) {
        in.cpad_x = (int16_t)rpx;
        in.cpad_y = (int16_t)rpy;
    }
    sk |= rk;
    if (g_route_touch) {   /* ALBW_ROUTE touch step */
        in.touch = 1;
        in.touch_x = g_route_tx;
        in.touch_y = g_route_ty;
    }
    in.held = kb | gp | cp | (in.touch ? KEY_TOUCH : 0) | sk;
    /* single-screen remaps only in the field (Link exists): at the title and the file select
     * Start goes to the game (straight to picking a save) and its own cursor works with the D-pad */
    /* in the field = the player update (001EA96C, it sets albw_dbg_player) ran within the last 10
     * seconds of frames: taken and cleared here. The Gear / Items menus and the Pause stop the field
     * for as long as they are open, so a short window flipped the screen back to the top while a
     * menu was up (it blinked); at the title after boot the player never ran at all. */
    static long field_frame = -100000;
    if (&albw_dbg_player && albw_dbg_player) { field_frame = g_plat_frames; albw_dbg_player = NULL; }
    int in_field = g_plat_frames - field_frame < 600;
    /* File select: the save list ("Select a file") is on the bottom screen, the top only previews
     * the highlighted file. In single-screen mode, Start or A at the title (no player yet)
     * brings the bottom screen up; the top comes back once the player update runs (a save was
     * loaded). The game keeps its buttons meanwhile (g_ss_front). */
    if (g_layout_cfg.single_screen) {
        uint32_t pressed = in.held & ~prev;
        /* at_title(), not "!in_field": during a long
         * cutscene (Yuga's speech) the player update pauses for over 10 s, and every A press to
         * advance the text flipped to the bottom screen - the map covered the scene (user). */
        if ((pressed & (KEY_START | KEY_A)) && getenv("ALBW_SEL_TRACE")) {
            const float *albw_debug_player_pos(void); const float *pp = albw_debug_player_pos();
            fprintf(stderr, "selector: f=%ld press, player %s %.3f %.3f %.3f\n", g_plat_frames, pp ? "yes" : "no", pp ? pp[0] : 0, pp ? pp[1] : 0, pp ? pp[2] : 0);
        }
        if (at_title() && !g_layout_cfg.show_bottom && (pressed & (KEY_START | KEY_A))) {
            plat_single_screen_flip();
            g_ss_front = 1;
        } else if (in_field && g_ss_front && g_plat_frames == field_frame) {
            plat_single_screen_flip();   /* clears g_ss_front */
        }
    }
    /* Start flips while the bottom screen is up (a menu may stay open any time) or in the field;
     * not at the title / file select, where Start belongs to the game. No automatic flip. */
    if (g_layout_cfg.single_screen && !g_ss_front && (in_field || g_layout_cfg.show_bottom)) {
        /* single-screen mode: Start brings the bottom screen (map, Gear, Items) up and back instead
         * of the game's Pause box; Select is the game's Start (Pause: Continue / Quit). The game
         * does not use Select on the field. */
        static int start_prev;
        int st = (in.held & KEY_START) != 0;
        if (st && !start_prev) plat_single_screen_flip();
        start_prev = st;
        in.held &= ~KEY_START;
        if (in.held & KEY_SELECT) in.held = (in.held & ~KEY_SELECT) | KEY_START;
    }
    sel_input(&in);   /* bottom screen up in single-screen mode: D-pad moves the selector, A taps */
    in.down = in.held & ~prev;
    in.up = prev & ~in.held;
    return 1;
}

const PlatInput *plat_input(void) { return &in; }

void plat_screen_size(int screen, int *w, int *h) {
    *w = screens[screen].w;
    *h = screens[screen].h;
}

void plat_begin_screen(int screen) {
    Target *t = &screens[screen];
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
}

/* Debug screenshots: ALBW_SHOT=<frame>[,<frame>...] writes both screen targets as PPM files
 * (ALBW_SHOT_DIR, default ".") when plat_end_frame is called for the n-th time:
 * shot_<frame>_top.ppm / shot_<frame>_bottom.ppm (unrotated screen orientation). */
static void shot_maybe(void) {
    static long frame;
    static const char *spec;
    static int init;
    if (!init) { init = 1; spec = getenv("ALBW_SHOT"); }
    frame++;
    g_plat_frames = frame;
    { void albw_x87_guard(const char *, void *); albw_x87_guard("frame start", 0); }
    { /* ALBW_TRAP_FRAME=<n>: SIGTRAP once at presented frame n (gdb inspection of a given moment) */
        static long trap = -2;
        if (trap == -2) { const char *e = getenv("ALBW_TRAP_FRAME"); trap = e ? atol(e) : -1; }
        if (trap > 0 && frame == trap) raise(SIGTRAP);
    }
    if (!spec) return;
    int hit = 0;
    for (const char *s = spec; *s;) {
        char *e;
        long v = strtol(s, &e, 10);
        if (e == s) break;
        if (v == frame) hit = 1;
        s = (*e == ',') ? e + 1 : e;
    }
    if (!hit) return;
    const char *dir = getenv("ALBW_SHOT_DIR");
    for (int i = 0; i < 2; i++) {
        Target *t = &screens[i];
        unsigned char *px = malloc((size_t)t->w * t->h * 4);
        if (!px) return;
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glReadPixels(0, 0, t->w, t->h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
        char path[512];
        snprintf(path, sizeof path, "%s/shot_%ld_%s.ppm", dir ? dir : ".", frame, i ? "bottom" : "top");
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
            for (int y = t->h - 1; y >= 0; y--)
                for (int x = 0; x < t->w; x++) fwrite(px + ((size_t)y * t->w + x) * 4, 1, 3, f);
            fclose(f);
            fprintf(stderr, "plat: wrote %s\n", path);
        }
        free(px);
    }
}

/* draw the screens of layout l into the current window's default framebuffer */
static void plat_composite(const Layout *l) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, l->ww, l->wh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(blit_prog);
    glBindVertexArray(blit_vao);
    GLint loc = glGetUniformLocation(blit_prog, "rect");
    GLint filter = g_layout_cfg.linear_filter ? GL_LINEAR : GL_NEAREST;
    for (int k = 0; k < 2; k++) {
        int i = l->order[k];
        if (!l->visible[i]) continue;
        float rect[4];
        layout_rect_ndc(l, i, rect);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, screens[i].color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glUniform4fv(loc, 1, rect);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    sel_composite(l);   /* single-screen selector over the bottom screen (selector.c) */
}

#include "menu.c"   /* Esc menu overlay (pauses the game inside plat_end_frame) */

/* debug: ALBW_FRAME_MS (kept out of plat_end_frame: its stack frame must stay as in older builds,
 * save states resume inside it) */
static __attribute__((noinline)) void frame_ms_trace(void) {
{   /* debug: ALBW_FRAME_MS=<ms>: log every frame that took longer (game + render, wall clock) */
    static long lim = -2;
    static uint64_t last;
    if (lim == -2) { const char *e = getenv("ALBW_FRAME_MS"); lim = e ? atol(e) : -1; }
    if (lim >= 0) {
        uint64_t now = SDL_GetTicksNS();
        extern unsigned long render_variant_builds;
        static unsigned long pb; static uint64_t pt;
        RStats rs = render_stats();
        if (last && (now - last) / 1000000 > (uint64_t)lim) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "frame-ms: f=%ld %.1f ms end %ld.%09ld (shader variants built %lu, textures uploaded %llu)\n",
                    g_plat_frames, (double)(now - last) / 1e6, (long)ts.tv_sec, (long)ts.tv_nsec,
                    render_variant_builds - pb, (unsigned long long)(rs.tex_uploads - pt));
        }
        pb = render_variant_builds; pt = rs.tex_uploads;
        last = now;
    }
}
}

void plat_end_frame(void) {
    frame_ms_trace();

    /* save / load state requests (keys, menu) run here: the main game thread still holds the app
     * core (released below for the swap). A load continues from the saved frame's same point. */
    if (albw_ss_request) {   /* ALBW_SS_AT=<frame>:save|load:<slot 1..>[,...] (debug, headless tests) */
        static const char *spec = (const char *)1;
        if (spec == (const char *)1) spec = getenv("ALBW_SS_AT");
        for (const char *q = spec; q && *q;) {
            char *e;
            long f = strtol(q, &e, 10);
            if (e == q || *e != ':') break;
            int op = !strncmp(e + 1, "save", 4) ? 1 : !strncmp(e + 1, "load", 4) ? 2 : 0;
            const char *c = strchr(e + 1, ':');
            int slot = c ? atoi(c + 1) - 1 : 0;
            if (op && f == g_plat_frames + 1) albw_ss_request(op, slot < 0 ? 0 : slot);
            q = strchr(e, ',');
            q = q ? q + 1 : NULL;
        }
    }
    if (albw_ss_frame_point) {
        albw_ss_frame_point();
        static const char *last;
        const char *msg = albw_ss_note ? albw_ss_note() : NULL;
        if (msg && msg[0] && msg != last) { last = msg; }
        {   /* show the outcome in the title for ~3 s */
            static char shown[200];
            static long until;
            if (msg && strcmp(msg, shown)) { snprintf(shown, sizeof shown, "%s", msg); until = 180; g_title_dirty = 1; }
            if (until > 0 && --until == 0) g_title_dirty = 1;
            g_title_msg = until > 0 ? shown : NULL;
        }
    }
    shot_maybe();
    prof_frame();
    SDL_Window *last;   /* presented after the block below starts: may wait for vsync */
    if (!g_separate) {
        plat_layout_update();
        plat_composite(&g_layout);
        if (g_layout_editing || g_layout_edit.mode != LAYOUT_EDIT_NONE) plat_layout_overlay();
        last = win;
    } else {
        /* one window per screen, one shared context: the window that does not wait for vsync
         * (plat_apply_vsync) is drawn and presented first, then the one that does */
        int vis[2] = {layout_separate_visible(&g_layout_cfg, SCREEN_TOP), layout_separate_visible(&g_layout_cfg, SCREEN_BOTTOM)};
        int order[2] = {vis[SCREEN_TOP] ? SCREEN_BOTTOM : SCREEN_TOP, vis[SCREEN_TOP] ? SCREEN_TOP : SCREEN_BOTTOM};
        last = NULL;
        for (int k = 0; k < 2; k++) {
            int s = order[k];
            if (!vis[s]) continue;
            Layout l;
            SDL_GL_MakeCurrent(plat_window_of(s), ctx);
            plat_window_layout(s, &l);
            plat_composite(&l);
            if (k == 0) SDL_GL_SwapWindow(plat_window_of(s));
            else last = plat_window_of(s);
        }
    }
    /* swap + pacing block the calling thread; when that is a game thread (os.c app-core
     * scheduling) the other game threads run meanwhile, as during a 3DS vblank wait */
    albw_sched_block_begin();
    if (last) SDL_GL_SwapWindow(last);
    if (g_want_render_scale && render_ready()) {   /* the start scale, once the renderer is up */
        if (g_want_render_scale != render_get_scale()) plat_apply_render_scale(g_want_render_scale);
        g_want_render_scale = 0;
    }
    {   /* ALBW_RESCALE_AT=<frame>:<scale> (debug): the Esc menu's Resolution change at that frame */
        static long at = -2; static int to;
        static long nf;
        if (at == -2) { const char *e = getenv("ALBW_RESCALE_AT"); at = e ? atol(e) : -1; to = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0; }
        if (at > 0 && ++nf == at && render_ready()) plat_apply_render_scale(to);
    }
    if (g_menu_open) menu_run();   /* Esc menu: the game is paused here until it closes */

    /* 60 Hz pacing (on top of vsync, which may run at a different refresh rate); Debug > Speed
     * scales the period, unlimited skips the wait */
    uint64_t now = plat_ticks_ns();
    {   /* ALBW_SPEED=<percent>[:<frame>] (debug, e.g. tools/boot_save.sh): the start speed (0 =
         * unlimited), back to 100 % at <frame> */
        static int init; static long until = -1, nf;
        if (!init) {
            init = 1;
            const char *e = getenv("ALBW_SPEED");
            if (e) { g_dbg_speed = atoi(e); until = strchr(e, ':') ? atol(strchr(e, ':') + 1) : -1; }
        }
        if (until > 0 && ++nf == until) g_dbg_speed = 100;
    }
    if (g_dbg_speed > 0) {
        next_frame_ns += 1666666700ull / (uint64_t)g_dbg_speed;
        if (next_frame_ns > now) SDL_DelayNS(next_frame_ns - now);
        else if (now - next_frame_ns > 100000000ull) next_frame_ns = now;
    } else next_frame_ns = now;
    {   /* Debug > Show FPS: game frames per second in the window title, updated twice a second */
        static uint64_t t0; static int n, shown;
        n++;
        uint64_t t = plat_ticks_ns();
        if (!t0) t0 = t;
        if (t - t0 >= 500000000ull || shown != g_dbg_show_fps || g_title_dirty) {
            int len = 0;
            g_title_fps[0] = 0;
            if (g_dbg_show_fps) len = snprintf(g_title_fps, sizeof g_title_fps, " - %.1f fps", n * 1e9 / (double)(t - t0 ? t - t0 : 1));
            if (g_title_msg && len >= 0 && (size_t)len < sizeof g_title_fps)
                snprintf(g_title_fps + len, sizeof g_title_fps - (size_t)len, "  [%s]", g_title_msg);
            if (g_dbg_show_fps || shown || g_title_dirty) plat_layout_title();
            g_title_dirty = 0;
            shown = g_dbg_show_fps;
            t0 = t;
            n = 0;
        }
    }
    albw_sched_block_end();
}
