/* Native entry point: the same sequence as the game's own startup code (USA 0x00100000),
 *     runtime init x4 -> C++ static constructors -> initialiser table -> final init -> game main,
 * with the platform layer brought up first. SDK functions in this chain are replaced natively
 * (native/sdk/); everything else is the decompiled game code. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../platform/platform.h"
#include "runtime.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   /* main() becomes SDL_main, called by SDLActivity (android/) */
#endif

typedef void (*InitFn)(void);
extern const InitFn albw_ctors[];
extern const uint32_t albw_ctors_count;
extern const InitFn albw_inits[];
extern const uint32_t albw_inits_count;

/* startup chain, in the order the game calls it */
void func_00100024(void);
void func_00100E60(void);
void func_00100C18(void);
void func_00100C90(void);
void albw_fp_set_3ds_mode(void);   /* native/sdk/os.c */
void albw_sched_main_thread(void); /* native/sdk/os.c */

struct MainArgs { int argc; char **argv; };
static int game_main(void *p);

char **albw_argv;   /* for Esc > File > Reset game (platform.c: re-exec) */

/* Portable mode (GitHub issue 6): keep saves, save states, settings, screenshots and logs in one
 * folder instead of ~/.local/share, ~/.config and ~/.local/state. The folder is
 *   ALBW_DATA_DIR=<folder>, or
 *   a folder named "albw-data" next to the program (next to the AppImage file for the AppImage).
 * Every path in the port comes from XDG_DATA_HOME / XDG_CONFIG_HOME / XDG_STATE_HOME, so setting
 * those three (before anything reads them) moves everything. Runs in main, not game_main: game_main
 * is on the saved stack of every save state and must not change. */
static void albw_portable_dirs(void) {
    char base[4096] = "";
    const char *env = getenv("ALBW_DATA_DIR");
    if (env && *env) {
        snprintf(base, sizeof base, "%s", env);
    } else {
        char prog[4096] = "";
        const char *appimage = getenv("APPIMAGE");   /* set by the AppImage runtime: the .AppImage file */
        if (appimage && *appimage) snprintf(prog, sizeof prog, "%s", appimage);
        else {
            ssize_t n = readlink("/proc/self/exe", prog, sizeof prog - 1);
            prog[n > 0 ? n : 0] = 0;
        }
        char *slash = strrchr(prog, '/');
        if (!slash) return;
        *slash = 0;
        snprintf(base, sizeof base, "%s/albw-data", prog);
        struct stat st;
        if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) return;   /* no folder: normal locations */
    }
    char d[4200];
    const char *sub[3][2] = {{"XDG_DATA_HOME", "share"}, {"XDG_CONFIG_HOME", "config"}, {"XDG_STATE_HOME", "state"}};
    mkdir(base, 0755);
    for (int i = 0; i < 3; i++) {
        snprintf(d, sizeof d, "%s/%s", base, sub[i][1]);
        mkdir(d, 0755);
        setenv(sub[i][0], d, 1);
    }
    /* the save folder is not an XDG path in fs.c ($HOME/.local/share/albw-port/save): name it here, or the
     * saves stay outside the folder (an explicit ALBW_SAVE_DIR still wins) */
    if (!getenv("ALBW_SAVE_DIR")) {
        static char sd[4300];
        snprintf(sd, sizeof sd, "%s/share/albw-port/save", base);
        setenv("ALBW_SAVE_DIR", sd, 1);
        struct stat sst;
        const char *home = getenv("HOME");
        char old[4300], oldsave[4400];
        snprintf(old, sizeof old, "%s/.local/share/albw-port/save", home ? home : ".");
        snprintf(oldsave, sizeof oldsave, "%s/savedata", old);
        if (stat(sd, &sst) != 0 && stat(oldsave, &sst) == 0)
            fprintf(stderr, "portable: earlier saves exist in %s; copy the contents of that folder to %s to use them here\n", old, sd);
    }
    fprintf(stderr, "portable: saves, settings and logs in %s\n", base);
}

#ifdef __ANDROID__
#include <pthread.h>
static int g_android_rc;
static void *android_game_thread(void *p) {
    g_android_rc = game_main(p);
    return NULL;
}
/* Android: everything lives in the app's own folder on shared storage,
 *   /storage/emulated/0/Android/data/<package>/files   (visible to file managers and adb):
 *   rom.3ds     the ROM (copied there by the app's ROM picker, android/, or put there by hand)
 *   share/ config/ state/   saves, settings (layout.cfg, input.cfg), save states (portable mode)
 *   albw.log    stderr of the last run
 * Set before albw_portable_dirs, which turns ALBW_DATA_DIR into the XDG paths. */
static void albw_android_dirs(void) {
    const char *base = SDL_GetAndroidExternalStoragePath();
    if (!base || !*base) base = SDL_GetAndroidInternalStoragePath();
    if (!base || !*base) return;
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* Back opens the Esc menu (input.c) */
    static char log[4200], rom[4200];
    snprintf(log, sizeof log, "%s/albw.log", base);
    if (freopen(log, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    setenv("HOME", base, 1);
    setenv("ALBW_DATA_DIR", base, 0);
    snprintf(rom, sizeof rom, "%s/rom.3ds", base);
    if (!getenv("ALBW_ROM")) setenv("ALBW_ROM", rom, 1);
    struct stat st;
    if (stat(getenv("ALBW_ROM"), &st) != 0) {
        char msg[4600];
        snprintf(msg, sizeof msg, "No ROM found at\n%s\n\nUse \"Choose ROM\" in the app, or copy your decrypted "
                 "European .3ds there and name it rom.3ds.", getenv("ALBW_ROM"));
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "A Link Between Worlds", msg, NULL);
        exit(1);
    }
}
#endif

int main(int argc, char **argv) {
    albw_argv = argv;
#ifdef __ANDROID__
    albw_android_dirs();
#endif
    albw_portable_dirs();
    /* x86-64 low-4GB mode: the game runs on a low stack (game code keeps stack addresses in
     * 32-bit words), same thread; on i386 game_main is called directly (runtime/lowmem.c) */
    struct MainArgs a = {argc, argv};
    pmem_init();   /* fixed-address areas before anything else maps memory (save states) */
#ifdef __ANDROID__
    /* Android: no stack switch on SDL's thread. ART checks the stack pointer of every thread that
     * calls into Java (SDL does, through JNI) against that thread's own stack, so the game runs on a
     * thread of its own with a 16 MiB stack instead (SDL attaches it to the VM when it needs to). */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, android_game_thread, &a) != 0) runtime_fatal("cannot start the game thread");
    pthread_attr_destroy(&at);
    pthread_join(th, NULL);
    return g_android_rc;
#else
    return albw_run_on_low_stack(game_main, &a, 16u << 20);
#endif
}

void albw_ss_set_main_base(uintptr_t base);   /* savestate.c */

static int game_main(void *p) {
    /* the saved part of the main thread's stack ends here (frames above: the ucontext start) */
    albw_ss_set_main_base((uintptr_t)__builtin_frame_address(0));
    int argc = ((struct MainArgs *)p)->argc;
    char **argv = ((struct MainArgs *)p)->argv;
    runtime_install_crash_handler();
    prof_init();
    PlatConfig cfg = {2, 0, 1};   /* render_scale 0: layout.cfg (Esc menu) decides */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) cfg.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--render-scale") && i + 1 < argc) cfg.render_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-vsync")) cfg.vsync = 0;
    }
    if (plat_init("The Legend of Zelda: A Link Between Worlds", &cfg)) return 1;
    /* the game's main thread runs with the same VFP mode as its other threads (FZ: the kernel's
     * initial FPSCR is not in the game code; 00106314 sets 0x03000000 on every thread it
     * creates). After plat_init: host libraries (SDL, GL drivers) keep their IEEE mode at init. */
    albw_fp_set_3ds_mode();
    /* this is the game's main thread: it owns the app core from here on (native/sdk/os.md,
     * "App-core scheduling"; ALBW_SCHED=parallel turns the model off) */
    albw_sched_main_thread();

    fprintf(stderr, "startup: runtime init\n");
    func_00100024();  /* zero .bss (decompiled, harmless) */
    /* 00100BC4 / 00100D84: armcc C-library start-up (per-thread library state, stdio). On Linux,
     * libc did all of this before main(); the native replacement is to not run the 3DS version. */
    /* 00100E60: nn startup (nninitStartUp): SetHeapSize(0x60000) (native, sdk/memory.c) and the
     * nn allocator on a 0x40000 MemoryBlock (00100E78). The framework bootstrap 00100F68 puts
     * sead's RootHeapMemory right after it (albw_os_heap_address() + 0x40000). */
    func_00100E60();
    fprintf(stderr, "startup: %u static constructors\n", albw_ctors_count);
    int verbose = getenv("ALBW_TRACE_CTORS") != NULL;
    for (uint32_t i = 0; i < albw_ctors_count; i++) {
        if (verbose || (i & 63) == 0) {
            Dl_info di;
            const char *name = dladdr((void *)albw_ctors[i], &di) && di.dli_sname ? di.dli_sname : "?";
            fprintf(stderr, "ctor %u/%u %s\n", i, albw_ctors_count, name);
        }
        albw_ctors[i]();
    }
    for (uint32_t i = 0; i < albw_inits_count; i++) albw_inits[i]();
    fprintf(stderr, "startup: game main\n");
    func_00100C18();
    func_00100C90();
    fprintf(stderr, "game returned\n");
    plat_shutdown();
    /* exit here, not through the ucontext start frames above (after a state load this stack's
     * lower frames came from the saving run) */
    exit(0);
}
