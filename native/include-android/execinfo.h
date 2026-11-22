/* <execinfo.h> for Android builds (tools/build_android.sh puts this folder on the include path).
 * Bionic only has backtrace() from API 33; this small version walks the stack with the unwinder
 * and names frames with dladdr, which is all the crash / fatal reports need. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1   /* dladdr (always declared by bionic; glibc only with this) */
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <unwind.h>
#include <unistd.h>

struct albw_bt_state { void **buf; int n, max; };

static _Unwind_Reason_Code albw_bt_step(struct _Unwind_Context *ctx, void *arg) {
    struct albw_bt_state *s = (struct albw_bt_state *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (s->n >= s->max) return _URC_END_OF_STACK;
        s->buf[s->n++] = (void *)pc;
    }
    return _URC_NO_REASON;
}

static inline int albw_backtrace(void **buf, int max) {
    struct albw_bt_state s = {buf, 0, max};
    _Unwind_Backtrace(albw_bt_step, &s);
    return s.n;
}

static inline void albw_backtrace_symbols_fd(void *const *buf, int n, int fd) {
    for (int i = 0; i < n; i++) {
        Dl_info di = {0};
        char line[256];
        int len;
        if (dladdr(buf[i], &di) && di.dli_sname)
            len = snprintf(line, sizeof line, "%s(%s+%#lx) [%p]\n", di.dli_fname ? di.dli_fname : "?", di.dli_sname,
                           (unsigned long)((const char *)buf[i] - (const char *)di.dli_saddr), buf[i]);
        else
            len = snprintf(line, sizeof line, "%s [%p]\n", di.dli_fname ? di.dli_fname : "?", buf[i]);
        if (len > 0) write(fd, line, (size_t)(len < (int)sizeof line ? len : (int)sizeof line - 1));
    }
}

#define backtrace albw_backtrace
#define backtrace_symbols_fd albw_backtrace_symbols_fd
