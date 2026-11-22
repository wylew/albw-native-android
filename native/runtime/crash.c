/* Crash reporting: SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT print what a bug report needs
 * (signal, fault address, faulting instruction, presented frame number, symbolized backtrace:
 * the game functions keep their func_<USA ADDR> names through -rdynamic) to stderr and append
 * it to a crash log, then the default action runs (core dump / exit status unchanged, so gdb,
 * coredumpctl and the test scripts see the crash as before).
 *   log file: $ALBW_CRASH_LOG, else $XDG_STATE_HOME/albw-port/crash.log, else
 *             ~/.local/state/albw-port/crash.log (directory created at install)
 *   ALBW_CRASH_HANDLER=0 leaves the signals alone.
 * Runs on an alternate signal stack, so a stack overflow is reported too. Only async-signal-safe
 * calls in the handler (write/open/close, backtrace pre-warmed at install so it does not load
 * libgcc from inside the handler). */
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include "runtime.h"

extern long g_plat_frames; /* native/platform/platform.c */

static char g_log_path[512];
static char g_altstack[64 * 1024];

/* --- async-signal-safe formatting */
static void put(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}
static void put_hex(int fd, uintptr_t v) {
    char b[2 + 2 * sizeof v + 1];
    int i = (int)sizeof b - 1;
    b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && i > 2);
    b[--i] = 'x'; b[--i] = '0';
    put(fd, b + i);
}
static void put_dec(int fd, long v) {
    char b[24];
    int i = (int)sizeof b - 1;
    b[i] = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u && i > 1);
    if (v < 0) b[--i] = '-';
    put(fd, b + i);
}

static const char *sig_name(int s) {
    switch (s) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "signal";
    }
}

static void report(int fd, int sig, siginfo_t *si, void *uc_v) {
    put(fd, "\n==== ALBW crash: ");
    put(fd, sig_name(sig));
    put(fd, "\n  fault address ");
    put_hex(fd, (uintptr_t)(si ? si->si_addr : 0));
    uintptr_t pc = 0;
#if defined(__i386__)
    if (uc_v) pc = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.gregs[REG_EIP];
#elif defined(__x86_64__)
    if (uc_v) pc = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.gregs[REG_RIP];
#elif defined(__arm__)
    if (uc_v) pc = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.arm_pc;
#endif
    put(fd, "  pc ");
    put_hex(fd, pc);
    put(fd, "  frame ");
    put_dec(fd, g_plat_frames);
    put(fd, "\n  backtrace (innermost first; func_XXXXXXXX = USA game address):\n");
    if (pc < 0x10000) {
        /* a call through a bad pointer (pc in the low pages): the unwinder faults on such a
         * frame (lamp / bush crash, BOOT_LOG 160t: no report at all). The caller's return
         * address is on top of the stack. */
        uintptr_t sp = 0;
#if defined(__i386__)
        if (uc_v) sp = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.gregs[REG_ESP];
#elif defined(__x86_64__)
        if (uc_v) sp = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.gregs[REG_RSP];
#elif defined(__arm__)
        if (uc_v) sp = (uintptr_t)((ucontext_t *)uc_v)->uc_mcontext.arm_sp;
#endif
        put(fd, "  jumped to a bad address; called from:\n  ");
        if (sp) {
            void *ra = *(void **)sp;
            backtrace_symbols_fd(&ra, 1, fd);
        }
        put(fd, "==== end of crash report\n");
        return;
    }
    void *bt[48];
    int n = backtrace(bt, 48);
    if (pc) {
        void *p = (void *)pc;
        put(fd, "  [pc] ");
        backtrace_symbols_fd(&p, 1, fd);
    }
    backtrace_symbols_fd(bt, n, fd);
    put(fd, "  (unnamed ./albw() [0x...] frames: addr2line -f -e build/albw <addresses>)\n");
    put(fd, "==== end of crash report\n");
}

static void on_crash(int sig, siginfo_t *si, void *uc) {
    int saved = errno;
    /* the log file first: if writing the report faults again, the file still has it */
    if (g_log_path[0]) {
        int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) {
            report(fd, sig, si, uc);
            close(fd);
        }
    }
    report(2, sig, si, uc);
    if (g_log_path[0]) {
        put(2, "crash report appended to ");
        put(2, g_log_path);
        put(2, "\n");
    }
    errno = saved;
    /* SA_RESETHAND restored the default action: re-raise for the core dump / exit status (a fault
     * would also repeat on return, but SIGABRT / raised signals would not) */
    raise(sig);
}

static void mkdir_p(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
}

void runtime_install_crash_handler(void) {
    const char *off = getenv("ALBW_CRASH_HANDLER");
    if (off && !strcmp(off, "0")) return;
    const char *p = getenv("ALBW_CRASH_LOG");
    if (p && *p) {
        snprintf(g_log_path, sizeof g_log_path, "%s", p);
    } else {
        const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        if (st && *st) snprintf(g_log_path, sizeof g_log_path, "%s/albw-port/crash.log", st);
        else if (home && *home) snprintf(g_log_path, sizeof g_log_path, "%s/.local/state/albw-port/crash.log", home);
    }
    if (g_log_path[0]) mkdir_p(g_log_path);
    { void *warm[2]; (void)backtrace(warm, 2); } /* load libgcc's unwinder now, not in the handler */
    stack_t ss = {0};
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof g_altstack;
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
