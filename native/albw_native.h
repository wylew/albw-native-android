/* Force-included into every decomp source for the native (gcc/clang, 32-bit x86) build.
 * Maps armcc-only keywords to native equivalents. Code-generation hints that only existed to
 * reproduce armcc's exact bytes become no-ops: they never change program behaviour. */
#ifndef ALBW_NATIVE_H
#define ALBW_NATIVE_H

#define __weak               __attribute__((weak))
#define __schedule_barrier() ((void)0)
#define __declspec(x)        __attribute__((x))
#define __forceinline        inline __attribute__((always_inline))
#define __inline             inline
#define __value_in_regs
#define __softfp
#ifndef __OBJC__
#define __packed
#endif
#define __nop()              ((void)0)
#define __rev(x)             __builtin_bswap32((unsigned)(x))
/* single-threaded semantics until the native threading layer exists */
#define __ldrex(p)           (*(p))
#define __strex(v, p)        ((*(p) = (v)), 0)
/* SDK system-call wrappers: declared as plain functions, provided by the native SDK layer */
#define __svc(n)

/* ALBW_GP: 4-byte game pointer qualifier (empty on 32-bit), see native/include/gptr.h */
#include "include/gptr.h"

#ifdef __cplusplus
extern "C" {
#endif
/* the game's thread-local storage block (TLS), provided by the native platform layer */
#include <stdint.h>
uintptr_t albw_tls(void);
/* a hard-coded 3DS VRAM address (0x1F000000..0x1F5FFFFF) in game code. On Android 0x1F000000 is
 * inside ART's Java heap, so VRAM is mapped at ALBW_ANDROID_VRAM instead (native/sdk/gfx.c; free in
 * a 32-bit app process, README memory map). It must stay below 0x80000000: the game's VRAM allocator
 * rounds addresses with signed int arithmetic (README: Android boot stop A6). Elsewhere VRAM is at
 * the 3DS address itself. */
#define ALBW_ANDROID_VRAM 0x3E000000u
#if defined(__ANDROID__)
#define ALBW_VRAM_ADDR(a) ((unsigned int)(a) - 0x1F000000u + ALBW_ANDROID_VRAM)
#else
#define ALBW_VRAM_ADDR(a) (a)
#endif

#ifdef __cplusplus
}
#endif

#endif
