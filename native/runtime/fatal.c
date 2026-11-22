/* Fatal errors with a native backtrace (symbol names come from the func_<ADDR> exports). */
#include <execinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#endif

void runtime_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#ifdef __ANDROID__
    {   /* no terminal on Android: show the message (stderr goes to albw.log) */
        char msg[1024];
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "A Link Between Worlds: fatal error", msg, NULL);
    }
#endif
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}
