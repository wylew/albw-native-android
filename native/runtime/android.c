/* Android glue (README: Android port). Compiled into every build but empty except on Android.
 *
 *   SDL_main            SDLActivity's entry point: paths + logging, then the port's main()
 *   albw_backtrace /    execinfo replacements: bionic has backtrace() only from API 33 (minSdk 24);
 *   albw_backtrace_symbols_fd   android_compat.h renames the calls
 *
 * Paths: everything lives in the app's external files folder
 *   /sdcard/Android/data/org.albw.port/files/
 *     rom.3ds            your decrypted EU ROM (adb push; a file picker comes in step 7)
 *     share/ config/ state/   saves, settings, logs (portable mode, runtime/entry.c)
 *     albw.env           optional KEY=VALUE lines (debug environment variables)
 * stdout / stderr go to logcat (tag "albw"). */
#if defined(__ANDROID__)
#define _GNU_SOURCE
#include <errno.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>
#include <SDL3/SDL.h>

/* ---- backtrace ---- */
struct bt_state { void **buf; int max, n; };
static _Unwind_Reason_Code bt_step(struct _Unwind_Context *ctx, void *arg) {
    struct bt_state *s = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (s->n >= s->max) return _URC_END_OF_STACK;
        s->buf[s->n++] = (void *)pc;
    }
    return _URC_NO_REASON;
}
int albw_backtrace(void **buf, int max) {
    struct bt_state s = {buf, max, 0};
    _Unwind_Backtrace(bt_step, &s);
    return s.n;
}
void albw_backtrace_symbols_fd(void *const *buf, int n, int fd) {
    for (int i = 0; i < n; i++) {
        Dl_info di;
        char line[512];
        if (dladdr(buf[i], &di) && di.dli_sname)
            snprintf(line, sizeof line, "%s(%s+0x%lx) [%p]\n", di.dli_fname ? di.dli_fname : "?", di.dli_sname,
                     (unsigned long)((uintptr_t)buf[i] - (uintptr_t)di.dli_saddr), buf[i]);
        else if (dladdr(buf[i], &di) && di.dli_fname)
            snprintf(line, sizeof line, "%s+0x%lx [%p]\n", di.dli_fname,
                     (unsigned long)((uintptr_t)buf[i] - (uintptr_t)di.dli_fbase), buf[i]);
        else
            snprintf(line, sizeof line, "[%p]\n", buf[i]);
        ssize_t r = write(fd, line, strlen(line));
        (void)r;
    }
}

/* ---- stdout / stderr -> logcat ---- */
static int g_log_pipe[2];
static void *log_thread(void *arg) {
    /* the read end in a local: g_log_pipe is static data, which a save-state load overwrites with
     * the saving run's descriptor numbers. Reading that one failed, this thread exited, the pipe
     * filled and the game froze in its next write to stderr (README: Android stop A14) */
    int fd = (int)(intptr_t)arg;
    char buf[2048];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "albw", start);
            start = nl + 1;
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof buf - 1) {   /* a very long line: flush it as is */
            buf[have] = 0;
            __android_log_write(ANDROID_LOG_INFO, "albw", buf);
            have = 0;
        }
    }
    return NULL;
}
static void log_to_logcat(void) {
    if (pipe(g_log_pipe)) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(g_log_pipe[1], 1);
    dup2(g_log_pipe[1], 2);
    pthread_t t;
    if (pthread_create(&t, NULL, log_thread, (void *)(intptr_t)g_log_pipe[0]) == 0) pthread_detach(t);
}

/* ---- entry ---- */
int main(int argc, char **argv);   /* runtime/entry.c */

/* the entry the loader (android/loader/loader.c, libmain.so) calls after loading libalbw.so at its
 * fixed address (SDLActivity calls the loader's SDL_main) */
int albw_android_main(int argc, char **argv) {
    log_to_logcat();
    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (!ext) {
        fprintf(stderr, "android: no external files folder (%s)\n", SDL_GetError());
        return 1;
    }
    mkdir(ext, 0755);
    {   /* debug options: KEY=VALUE lines in <files>/albw.env (adb push), e.g. ALBW_GFX_TRACE=1 */
        char envf[1024], line[1024];
        snprintf(envf, sizeof envf, "%s/albw.env", ext);
        FILE *f = fopen(envf, "r");
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "android: %s=%s (albw.env)\n", line, eq + 1);
        }
        if (f) fclose(f);
    }
    setenv("HOME", ext, 0);
    setenv("ALBW_DATA_DIR", ext, 0);   /* portable mode: share/ config/ state/ under it */
    char rom[1024];
    snprintf(rom, sizeof rom, "%s/rom.3ds", ext);
    setenv("ALBW_ROM", rom, 0);
    /* crashes: Android's debuggerd writes a full tombstone (adb logcat -b crash); the port's own
     * handler cannot unwind past the signal frame on ARM and re-entered itself */
    setenv("ALBW_CRASH_HANDLER", "0", 0);
    fprintf(stderr, "android: data folder %s, ROM %s%s\n", ext, getenv("ALBW_ROM"),
            access(getenv("ALBW_ROM"), R_OK) ? " (MISSING: adb push your EU ROM there)" : "");
    {   /* save states: this library and its .ssmap (APK asset written by tools/link_android.sh) */
        Dl_info di;
        void albw_ss_android_init(const char *lib, char *map, size_t len);
        size_t mlen = 0;
        char *map = SDL_LoadFile("libalbw.ssmap", &mlen);
        if (dladdr((void *)albw_android_main, &di) && di.dli_fname) {
            albw_ss_android_init(di.dli_fname, map, mlen);
            fprintf(stderr, "android: save states %s (%s, map %zu bytes, loaded at %p)\n", map ? "available" : "UNAVAILABLE (no map asset)",
                    di.dli_fname, mlen, di.dli_fbase);
        }
    }
    void albw_overflow_report_install(void);   /* android_overflow.c: stack overflow report */
    albw_overflow_report_install();
    void albw_watch_start(void);   /* android_watch.c: ALBW_WATCH debug watchpoint */
    albw_watch_start();
    return main(argc, argv);
}
#endif
