/* Force-included by tools/link_android.sh for the port's own C files (not the decomp objects). */
#pragma once
#if defined(__ANDROID__)
/* bionic declares backtrace() only for API 33+; runtime/android.c has replacements */
int albw_backtrace(void **buf, int max);
void albw_backtrace_symbols_fd(void *const *buf, int n, int fd);
#define backtrace albw_backtrace
#define backtrace_symbols_fd albw_backtrace_symbols_fd
#endif
