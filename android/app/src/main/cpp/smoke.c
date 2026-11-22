/* Android step 1 smoke test (README: Android port).
 *
 * - opens a fullscreen GLES 3 window and clears it; the colour changes with gamepad buttons /
 *   touches, so input can be checked by eye;
 * - logs gamepad and touch events, GL version / renderer / extensions (logcat tag "albw");
 * - step 2: writes /proc/self/maps to <external files>/maps.txt and logs whether the fixed
 *   address ranges the 32-bit game build expects are free.
 *
 *   adb logcat -s albw:* SDL:*
 *   adb pull /sdcard/Android/data/org.albw.port/files/maps.txt
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "albw", __VA_ARGS__)

static SDL_Window *win;
static SDL_GLContext ctx;
static float rgb[3] = {0.10f, 0.12f, 0.20f};

/* fixed ranges of the 32-bit PC build (tools/link.sh, native/sdk/memory.c, gfx.c) */
static const struct { uintptr_t lo, hi; const char *what; } fixed[] = {
    {0x08048000u, 0x0A000000u, "program image (approx. 30 MiB)"},
    {0x1F000000u, 0x1F600000u, "VRAM"},
    {0x30000000u, 0x30300000u, "game data (.albw_ro/.albw_rw/.albw_zi)"},
    {0x40000000u, 0x5C000000u, "game memory (heaps)"},
};

static void check_maps(void)
{
    char out[512] = "";
    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (ext) snprintf(out, sizeof out, "%s/maps.txt", ext);
    FILE *in = fopen("/proc/self/maps", "r");
    FILE *of = out[0] ? fopen(out, "w") : NULL;
    if (!in) { LOG("cannot open /proc/self/maps"); return; }
    int clash[4] = {0};
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        if (of) fputs(line, of);
        line[strcspn(line, "\n")] = 0;
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2) continue;
        for (int i = 0; i < 4; i++)
            if (lo < fixed[i].hi && hi > fixed[i].lo) {
                clash[i]++;
                LOG("CLASH %s: %s", fixed[i].what, line);
            }
    }
    fclose(in);
    if (of) { fclose(of); LOG("memory map written to %s", out); }
    LOG("pointer size %d bytes", (int)sizeof(void *));
    for (int i = 0; i < 4; i++)
        LOG("%08lx-%08lx %-40s %s", (unsigned long)fixed[i].lo, (unsigned long)fixed[i].hi,
            fixed[i].what, clash[i] ? "TAKEN" : "free");
}

SDL_AppResult SDL_AppInit(void **state, int argc, char **argv)
{
    (void)state; (void)argc; (void)argv;
    check_maps();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        LOG("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    win = SDL_CreateWindow("ALBW", 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!win || !(ctx = SDL_GL_CreateContext(win))) {
        LOG("window / GLES 3.2 context: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_GL_SetSwapInterval(1);
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    LOG("window %dx%d", w, h);
    LOG("GL_VERSION  %s", glGetString(GL_VERSION));
    LOG("GL_RENDERER %s", glGetString(GL_RENDERER));
    LOG("GLSL        %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
        if (strstr(e, "framebuffer_fetch") || strstr(e, "border_clamp") || strstr(e, "logic_op") ||
            strstr(e, "depth_clamp") || strstr(e, "clip_control") || strstr(e, "texture_format_BGRA"))
            LOG("ext: %s", e);
    }
    LOG("%d extensions", n);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *state, SDL_Event *ev)
{
    (void)state;
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *g = SDL_OpenGamepad(ev->gdevice.which);
        LOG("gamepad added: %s", g ? SDL_GetGamepadName(g) : SDL_GetError());
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        int b = ev->gbutton.button;
        LOG("button %s", SDL_GetGamepadStringForButton(b));
        rgb[0] = (b & 1) ? 0.8f : 0.1f;
        rgb[1] = (b & 2) ? 0.8f : 0.1f;
        rgb[2] = (b & 4) ? 0.8f : 0.2f;
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (ev->gaxis.value > 16000 || ev->gaxis.value < -16000)
            LOG("axis %s %d", SDL_GetGamepadStringForAxis(ev->gaxis.axis), ev->gaxis.value);
        break;
    case SDL_EVENT_FINGER_DOWN:
        LOG("touch %.3f %.3f", ev->tfinger.x, ev->tfinger.y);
        rgb[0] = ev->tfinger.x; rgb[1] = ev->tfinger.y; rgb[2] = 0.5f;
        break;
    case SDL_EVENT_KEY_DOWN:
        LOG("key %s", SDL_GetKeyName(ev->key.key));
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *state)
{
    (void)state;
    glClearColor(rgb[0], rgb[1], rgb[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(win);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *state, SDL_AppResult result)
{
    (void)state; (void)result;
    if (ctx) SDL_GL_DestroyContext(ctx);
}
