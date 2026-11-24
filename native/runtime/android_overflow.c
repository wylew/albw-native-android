/* Android debug aid: stack overflow report (README: Android, debugging).
 *
 * The system's crash report (tombstone) of a stack overflow on a game thread shows one frame: the
 * game's code has no unwind tables. This SIGSEGV handler (runs on bionic's per-thread signal stack,
 * then hands the signal on to the previous handler, so the tombstone is still written) checks for a
 * fault in the guard below a fixed stack slot and scans that thread's stack for return addresses
 * (words in libalbw.so's code right after a bl / blx), logging to logcat (tag "albw-overflow"):
 *   - the most frequent return addresses (the recursion cycle) with their function names,
 *   - the 40 outermost frames (how the thread got into it).
 * Not async-signal-safe (dladdr, android log); the process is dying anyway. */
#if defined(__ANDROID__) && defined(__arm__)
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#define OLOG(...) __android_log_print(ANDROID_LOG_ERROR, "albw-overflow", __VA_ARGS__)
#define STACKS_LO 0x58000000u   /* runtime/pmem.c PMEM_STACKS */
#define STACKS_HI 0x5C000000u

static struct sigaction g_prev;
static uintptr_t g_text_lo, g_text_hi;

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
/* is [a, a+32) mapped (debug: the fault report must not fault) */
static int readable(uintptr_t a) {
    if (a < 0x10000u) return 0;
    uintptr_t pg = a & ~(uintptr_t)4095;
    unsigned char vec[2];
    return mincore((void *)pg, ((a + 32 - pg) + 4095) & ~4095u, vec) == 0;
}
static int is_ret(uintptr_t a) {
    if ((a & 3) || a < g_text_lo + 4 || a >= g_text_hi) return 0;
    uint32_t w = *(const uint32_t *)(a - 4);
    return ((w >> 28) != 0xF && ((w >> 24) & 0xF) == 0xB) || (w >> 25) == 0x7D ||
           ((w >> 28) != 0xF && (w & 0x0FFFFFF0u) == 0x012FFF30u);
}
static const char *name_of(uintptr_t a, uintptr_t *off) {
    Dl_info di;
    if (dladdr((void *)(a - 4), &di) && di.dli_sname) { *off = a - (uintptr_t)di.dli_saddr; return di.dli_sname; }
    *off = a - g_text_lo;
    return "?";
}

static void handler(int sig, siginfo_t *si, void *uc_v) {
    uintptr_t fa = (uintptr_t)si->si_addr;
    ucontext_t *uc = uc_v;
    uintptr_t sp = uc->uc_mcontext.arm_sp;
    if (fa >= STACKS_LO && fa < STACKS_HI && sp >= STACKS_LO && sp < STACKS_HI && fa < sp + 0x10000) {
        /* the stack: from sp up to the first unreadable / foreign region; slot tops are at most
         * 16 MiB above (runtime/pmem.c), scan up to that */
        uintptr_t top = sp + (16u << 20) + 0x10000;
        if (top > STACKS_HI) top = STACKS_HI;
        enum { TOPN = 24, MAXD = 512 };
        static uintptr_t dist[MAXD];
        static unsigned cnt[MAXD];
        static uintptr_t outer[40];
        int nd = 0, no = 0;
        unsigned total = 0;
        OLOG("stack overflow: fault %#lx sp %#lx pc %#lx lr %#lx", (unsigned long)fa, (unsigned long)sp,
             (unsigned long)uc->uc_mcontext.arm_pc, (unsigned long)uc->uc_mcontext.arm_lr);
        uintptr_t pcs[2] = {uc->uc_mcontext.arm_pc + 4, uc->uc_mcontext.arm_lr};
        for (int i = 0; i < 2; i++) { uintptr_t o; const char *n = name_of(pcs[i], &o); OLOG("  %s %s+%#lx", i ? "lr" : "pc", n, (unsigned long)o); }
        /* stop at a page that is not part of this stack: the slot's 64 KiB guard above the top is
         * PROT_NONE, so check readability per page with the slot layout instead: slot 0 is
         * [0x58010000, 0x59010000), workers 1 MiB + 64 KiB guard each */
        uintptr_t lim;
        if (sp < 0x59010000u) lim = 0x59010000u;
        else { uintptr_t k = (sp - 0x59020000u) / 0x110000u; lim = 0x59020000u + k * 0x110000u + 0x100000u; }
        if (lim < top) top = lim;
        for (uintptr_t p = sp; p + 4 <= top; p += 4) {
            uintptr_t w = *(const uint32_t *)p;
            if (!is_ret(w)) continue;
            total++;
            int j;
            for (j = 0; j < nd; j++) if (dist[j] == w) { cnt[j]++; break; }
            if (j == nd && nd < MAXD) { dist[nd] = w; cnt[nd] = 1; nd++; }
            if (no < 40) outer[no++] = w; else { memmove(outer, outer + 1, 39 * sizeof outer[0]); outer[39] = w; }
        }
        OLOG("  %u return-address words in [%#lx, %#lx), %d distinct; most frequent:", total, (unsigned long)sp, (unsigned long)top, nd);
        for (int r = 0; r < TOPN && r < nd; r++) {
            int b = r;
            for (int j = r + 1; j < nd; j++) if (cnt[j] > cnt[b]) b = j;
            uintptr_t td = dist[r]; unsigned tc = cnt[r]; dist[r] = dist[b]; cnt[r] = cnt[b]; dist[b] = td; cnt[b] = tc;
            uintptr_t o; const char *n = name_of(dist[r], &o);
            OLOG("  %7u x %s+%#lx (%#lx)", cnt[r], n, (unsigned long)o, (unsigned long)dist[r]);
        }
        OLOG("  outermost frames (innermost first):");
        for (int i = 0; i < no; i++) { uintptr_t o; const char *n = name_of(outer[i], &o); OLOG("    %s+%#lx", n, (unsigned long)o); }
    }
    else if (getenv("ALBW_FAULT_REPORT")) {
        /* any other fault (debug, ALBW_FAULT_REPORT=1): registers, and for each register that
         * points into memory the first words there and the words those point at */
        const unsigned long *rg = &uc->uc_mcontext.arm_r0;   /* r0..r10, fp, ip, sp, lr, pc */
        static const char *const nm[] = {"r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","fp","ip","sp","lr","pc"};
        OLOG("fault %#lx at pc %#lx", (unsigned long)fa, (unsigned long)uc->uc_mcontext.arm_pc);
        {   /* ALBW_FAULT_PEEK=<hex>[,<hex>...]: words at given addresses, and where the first points */
            const char *pk = getenv("ALBW_FAULT_PEEK");
            while (pk && *pk) {
                char *end;
                uintptr_t a = (uintptr_t)strtoul(pk, &end, 16);
                if (end == pk) break;
                if (readable(a)) {
                    const uint32_t *w = (const uint32_t *)(a & ~3u);
                    OLOG("  peek %08lx: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned long)a, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                    if (readable(w[0])) {
                        const uint32_t *x = (const uint32_t *)(uintptr_t)(w[0] & ~3u);
                        OLOG("    [%08x]: %08x %08x %08x %08x %08x %08x", w[0], x[0], x[1], x[2], x[3], x[4], x[5]);
                    }
                }
                pk = *end == ',' ? end + 1 : end;
            }
        }
        {   /* return addresses on the stack (innermost first), with where they are */
            uintptr_t p = uc->uc_mcontext.arm_sp & ~3u;
            for (int k = 0; k < 4096 && readable(p); k++, p += 4) {
                uintptr_t w = *(const uint32_t *)p, o;
                if (!is_ret(w)) continue;
                const char *n = name_of(w, &o);
                OLOG("  stack %08lx: ret %s+%#lx", (unsigned long)p, n, (unsigned long)o);
            }
        }
        for (int i = 0; i < 16; i++) {
            uintptr_t v = rg[i], o;
            const char *n = (v >= g_text_lo && v < g_text_hi) ? name_of(v + 4, &o) : NULL;
            if (n) { OLOG("  %-3s %08lx  code %s+%#lx", nm[i], (unsigned long)v, n, (unsigned long)(o - 4)); continue; }
            if (!readable(v) || i == 15) { OLOG("  %-3s %08lx", nm[i], (unsigned long)v); continue; }
            const uint32_t *w = (const uint32_t *)(v & ~3u);
            OLOG("  %-3s %08lx -> %08x %08x %08x %08x %08x %08x %08x %08x", nm[i], (unsigned long)v, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            for (int k = 0; k < 4; k++) {
                if (!readable(w[k])) continue;
                const uint32_t *x = (const uint32_t *)(uintptr_t)(w[k] & ~3u);
                uintptr_t oo; const char *xn = (x[0] >= g_text_lo && x[0] < g_text_hi) ? name_of(x[0] + 4, &oo) : "";
                OLOG("        [+%d] %08x -> %08x %08x %08x %08x  %s", k * 4, w[k], x[0], x[1], x[2], x[3], xn);
            }
        }
    }
    /* hand on: ART's signal chain (libsigchain) treats a re-raise with the previous action as
     * SIG_DFL and aborts without the fault details; returning with the previous action installed
     * re-runs the faulting instruction, and the system's handler writes the tombstone as before */
    (void)sig;
    sigaction(SIGSEGV, &g_prev, NULL);
}

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    uintptr_t me = (uintptr_t)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz;
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && me >= lo && me < hi) { g_text_lo = lo; g_text_hi = hi; return 1; }
    }
    return 0;
}
void albw_overflow_report_install(void) {
    /* the code segment of this library (its first pages before the code are not mapped) */
    dl_iterate_phdr(phdr_cb, (void *)(uintptr_t)albw_overflow_report_install);
    if (!g_text_lo) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev);
}
#else
void albw_overflow_report_install(void) {}
#endif
