/* Sampling CPU profiler (debug aid, off unless ALBW_PROFILE is set).
 *
 *   ALBW_PROFILE=<file>     sample the program counter of whichever thread is running at every
 *                           1 ms of process CPU time (ITIMER_PROF / SIGPROF), histogram it in
 *                           16-byte buckets of the executable's text, and rewrite <file> every
 *                           600 presented frames (prof_frame(), from plat_end_frame) and at exit.
 *   ALBW_PROFILE_HZ=<n>     sampling rate (default 1000).
 * <file>: "# samples <total> in_text <n> outside <n>" then "<hex addr> <count>" per bucket;
 * tools/prof_report.py resolves the addresses to functions (nm) and prints the top list.
 * Samples outside the executable (libc, the GL driver, SDL): the last 65536 are resolved with
 * dladdr to library + nearest exported symbol ("@ <lib> <sym> <count>" lines); no frame pointers
 * to attribute them to a caller.
 * Written because perf_event_open is not available in this environment (perf_event_paranoid). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <link.h>
#include <dlfcn.h>

#include "runtime.h"

static uint32_t *g_hist;
static uintptr_t g_lo, g_hi;
static uint32_t g_total, g_outside;
#define OUT_RING 65536   /* last outside-the-executable PCs, resolved to library + symbol at dump */
static uintptr_t g_out_pc[OUT_RING];
static const char *g_path;
static long g_frames;

static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__arm__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.arm_pc;
#else
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
    __atomic_add_fetch(&g_total, 1, __ATOMIC_RELAXED);
    if (pc >= g_lo && pc < g_hi) __atomic_add_fetch(&g_hist[(pc - g_lo) >> 4], 1, __ATOMIC_RELAXED);
    else g_out_pc[__atomic_fetch_add(&g_outside, 1, __ATOMIC_RELAXED) % OUT_RING] = pc;
}

/* the executable's code segment (decompiled objects define their own `etext`-like symbols, so
 * the program headers are used instead) */
static int find_text(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;   /* the main program comes first, unnamed */
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            g_lo = info->dlpi_addr + ph->p_vaddr;
            g_hi = g_lo + ph->p_memsz;
        }
    }
    return 1;
}

static void prof_dump(void) {
    if (!g_path) return;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    uint32_t total = g_total, out = g_outside;
    fprintf(f, "# samples %u in_text %u outside %u frames %ld\n", total, total - out, out, g_frames);
    size_t n = (g_hi - g_lo) >> 4;
    for (size_t i = 0; i < n; i++)
        if (g_hist[i]) fprintf(f, "%lx %u\n", (unsigned long)(g_lo + (i << 4)), g_hist[i]);
    /* outside samples (up to the last OUT_RING): "@ <library> <symbol> <count>" */
    struct { const char *lib, *sym; uintptr_t off; uint32_t n; } agg[512];
    int na = 0;
    uint32_t nr = out < OUT_RING ? out : OUT_RING;
    for (uint32_t i = 0; i < nr; i++) {
        Dl_info di;
        const char *lib = "?", *sym = "?";
        uintptr_t off = 0;
        if (dladdr((void *)g_out_pc[i], &di)) {
            if (di.dli_fname) lib = di.dli_fname;
            if (di.dli_sname) sym = di.dli_sname;
            else off = (g_out_pc[i] - (uintptr_t)di.dli_fbase) & ~(uintptr_t)0xFF;   /* unnamed: 256-byte block */
        }
        int k = 0;
        while (k < na && (agg[k].lib != lib || agg[k].sym != sym || agg[k].off != off)) k++;
        if (k == na) {
            if (na == 512) continue;
            agg[na].lib = lib, agg[na].sym = sym, agg[na].off = off, agg[na].n = 0, na++;
        }
        agg[k].n++;
    }
    for (int k = 0; k < na; k++) {
        if (agg[k].sym[0] == '?' && agg[k].off) fprintf(f, "@ %s +%lx %u\n", agg[k].lib, (unsigned long)agg[k].off, agg[k].n);
        else fprintf(f, "@ %s %s %u\n", agg[k].lib, agg[k].sym, agg[k].n);
    }
    fclose(f);
    rename(tmp, g_path);
}

void prof_init(void) {
    const char *p = getenv("ALBW_PROFILE");
    if (!p || !*p) return;
    dl_iterate_phdr(find_text, NULL);
    if (g_hi <= g_lo) return;
    g_hist = calloc(((g_hi - g_lo) >> 4) + 1, sizeof *g_hist);
    if (!g_hist) return;
    g_path = p;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    const char *hz = getenv("ALBW_PROFILE_HZ");
    long us = 1000000 / (hz && atol(hz) > 0 ? atol(hz) : 1000);
    struct itimerval it = {{0, us}, {0, us}};
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(prof_dump);
    fprintf(stderr, "profile: sampling every %ld us of CPU time -> %s\n", us, p);
}

void prof_frame(void) {
    if (!g_path) return;
    if (++g_frames % 600 == 0) prof_dump();
}
