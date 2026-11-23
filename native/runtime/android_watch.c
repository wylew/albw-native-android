/* Android debug aid: a hardware write watchpoint on one game word (README: Android, debugging).
 *
 *   ALBW_WATCH=<hex address>[:<min abs float>]   e.g. ALBW_WATCH=4401b2d4:1000 in albw.env
 *
 * NDK r30's lldb-server crashed on attach, so this is the minimal debugger for one question:
 * "who writes this word?". albw_watch_start() (called from SDL_main, before the game starts and
 * again from the game thread once it exists) forks a helper that PTRACE_ATTACHes to every thread
 * of the game process, programs ARM debug watchpoint 0 (PTRACE_SETHBPREGS: store, 4 bytes, user
 * mode) on the address, and logs every hit to logcat (tag "albw-watch"): pc, lr, sp, the value
 * written (also as a float) and the first stack words. With :<min>, only values whose float
 * magnitude is at least <min> (or NaN / inf) are logged. Threads created later get the watchpoint
 * when the helper rescans /proc (every hit and every 500 ms). Debug only: the game pauses on
 * every hit. */
#if defined(__ANDROID__) && defined(__arm__)
#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

#ifndef PTRACE_GETHBPREGS
#define PTRACE_GETHBPREGS 29
#define PTRACE_SETHBPREGS 30
#endif
#define WLOG(...) __android_log_print(ANDROID_LOG_INFO, "albw-watch", __VA_ARGS__)

static uint32_t g_addr;
static float g_min;

static int attached(pid_t *list, int n, pid_t t) {
    for (int i = 0; i < n; i++) if (list[i] == t) return 1;
    return 0;
}

static int arm_watch_set(pid_t tid, int on) {
    uint32_t a = g_addr & ~3u;
    unsigned bas = 0xF;                    /* the 4 bytes of the word */
    uint32_t ctrl = on ? (bas << 5) | (2u << 3) /* store */ | (2u << 1) /* user */ | 1u : 0;
    if (ptrace(PTRACE_SETHBPREGS, tid, (void *)-1, &a)) return -1;   /* WVR 0 */
    if (ptrace(PTRACE_SETHBPREGS, tid, (void *)-2, &ctrl)) return -1; /* WCR 0 */
    return 0;
}
static int arm_watch(pid_t tid) { return arm_watch_set(tid, 1); }

/* the watchpoint fires before the store completes: step over it with the watchpoint off */
static void step_over(pid_t t) {
    int st;
    arm_watch_set(t, 0);
    if (ptrace(PTRACE_SINGLESTEP, t, 0, 0) == 0) waitpid(t, &st, __WALL);
    arm_watch_set(t, 1);
}

static void helper(pid_t game) {
    static pid_t list[256];
    int n = 0;
    for (int round = 0;; round++) {
        /* attach to new threads */
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/task", game);
        DIR *d = opendir(path);
        if (!d) { WLOG("game gone"); _exit(0); }
        struct dirent *e;
        while ((e = readdir(d))) {
            pid_t t = atoi(e->d_name);
            if (t <= 0 || attached(list, n, t) || n >= 256) continue;
            if (ptrace(PTRACE_SEIZE, t, 0, 0)) { WLOG("seize %d: %s", t, strerror(errno)); list[n++] = t; continue; }
            if (ptrace(PTRACE_INTERRUPT, t, 0, 0) == 0) {
                int st;
                waitpid(t, &st, __WALL);
                if (arm_watch(t)) WLOG("watchpoint on %d: %s", t, strerror(errno));
                ptrace(PTRACE_CONT, t, 0, 0);
            }
            list[n++] = t;
        }
        closedir(d);
        if (round == 0) WLOG("watching %#x (store, 4 bytes) in %d threads, min %g", g_addr, n, g_min);
        /* wait for hits (500 ms, then rescan threads) */
        for (int k = 0; k < 50; k++) {
            int st;
            pid_t t = waitpid(-1, &st, __WALL | WNOHANG);
            if (t <= 0) { usleep(10000); continue; }
            if (WIFEXITED(st) || WIFSIGNALED(st)) continue;
            if (!WIFSTOPPED(st)) continue;
            int sig = WSTOPSIG(st);
            if (sig == SIGTRAP) {
                siginfo_t si;
                ptrace(PTRACE_GETSIGINFO, t, 0, &si);
                struct pt_regs r;
                ptrace(PTRACE_GETREGS, t, 0, &r);
                errno = 0;
                uint32_t v = (uint32_t)ptrace(PTRACE_PEEKDATA, t, (void *)(uintptr_t)(g_addr & ~3u), 0);
                float f; memcpy(&f, &v, 4);
                if (g_min <= 0) {
                    uint32_t sp = (uint32_t)r.ARM_sp, w[8];
                    for (int i = 0; i < 8; i++) w[i] = (uint32_t)ptrace(PTRACE_PEEKDATA, t, (void *)(uintptr_t)(sp + 4u * i), 0);
                    WLOG("hit tid %d pc %#lx lr %#lx sp %#x r0 %#lx r1 %#lx r2 %#lx r3 %#lx old %#x (%g) stack %#x %#x %#x %#x %#x %#x %#x %#x",
                         t, r.ARM_pc, r.ARM_lr, sp, r.ARM_r0, r.ARM_r1, r.ARM_r2, r.ARM_r3, v, f, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
                step_over(t);
                {   /* the value the store wrote */
                    uint32_t nv = (uint32_t)ptrace(PTRACE_PEEKDATA, t, (void *)(uintptr_t)(g_addr & ~3u), 0);
                    float nf; memcpy(&nf, &nv, 4);
                    if (g_min <= 0 || !(fabsf(nf) < g_min))
                        WLOG("  -> wrote %#x (%g) at pc %#lx lr %#lx tid %d", nv, nf, r.ARM_pc, r.ARM_lr, t);
                }
                ptrace(PTRACE_CONT, t, 0, 0);
            } else if (st >> 16 == PTRACE_EVENT_STOP) {
                ptrace(PTRACE_CONT, t, 0, 0);
            } else {
                ptrace(PTRACE_CONT, t, 0, (void *)(intptr_t)sig);   /* deliver it */
            }
        }
    }
}

void albw_watch_start(void) {
    static int started;
    const char *e = getenv("ALBW_WATCH");
    if (started || !e || !*e) return;
    started = 1;
    g_addr = (uint32_t)strtoul(e, NULL, 16);
    const char *c = strchr(e, ':');
    g_min = c ? (float)atof(c + 1) : 0;
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pid_t game = getpid();
    pid_t p = fork();
    if (p == 0) { helper(game); _exit(0); }
    WLOG("helper %d started for %#x", p, g_addr);
}
#else
void albw_watch_start(void) {}
#endif
