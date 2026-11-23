/* libmain.so: the library SDLActivity loads and whose SDL_main it calls. It loads the game,
 * libalbw.so (tools/link_android.sh), at a fixed address and runs it (README: Android save states).
 *
 * Why: save states store pointers into the game's code and data. On the PC the binary is non-PIE
 * (always at the same address); an Android app's libraries are loaded wherever the linker likes.
 * android_dlopen_ext(ANDROID_DLEXT_RESERVED_ADDRESS) loads a library into a range we reserved, so
 * the game is at ALBW_GAME_BASE in every run (free in a 32-bit app process: README memory map;
 * below it the game heaps 0x40000000-0x5C000000, VRAM 0x3E000000). If the range is taken the game
 * still runs, loaded anywhere (save states then work only within that run). */
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALBW_GAME_BASE 0x60000000u
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "albw", __VA_ARGS__)

/* the span of the library's PT_LOAD segments (what the linker maps) */
static size_t image_span(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    Elf32_Ehdr eh;
    size_t span = 0;
    if (pread(fd, &eh, sizeof eh, 0) == (ssize_t)sizeof eh && !memcmp(eh.e_ident, ELFMAG, SELFMAG)) {
        for (int i = 0; i < eh.e_phnum; i++) {
            Elf32_Phdr ph;
            if (pread(fd, &ph, sizeof ph, eh.e_phoff + (off_t)i * eh.e_phentsize) != (ssize_t)sizeof ph) break;
            if (ph.p_type == PT_LOAD && ph.p_vaddr + ph.p_memsz > span) span = ph.p_vaddr + ph.p_memsz;
        }
    }
    close(fd);
    return (span + 0xFFFFu) & ~(size_t)0xFFFFu;
}

int SDL_main(int argc, char **argv) {
    Dl_info di;
    char path[1024];
    if (!dladdr((void *)SDL_main, &di) || !di.dli_fname) { LOG("loader: cannot find myself"); return 1; }
    snprintf(path, sizeof path, "%s", di.dli_fname);
    char *slash = strrchr(path, '/');
    if (!slash) return 1;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "libalbw.so");

    android_dlextinfo ext;
    memset(&ext, 0, sizeof ext);
    size_t span = image_span(path);
    void *want = (void *)(uintptr_t)ALBW_GAME_BASE;
    void *r = span ? mmap(want, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) : MAP_FAILED;
    if (r == want) {
        ext.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
        ext.reserved_addr = r;
        ext.reserved_size = span;
    } else {
        if (r != MAP_FAILED) munmap(r, span);
        LOG("loader: %#x..+%#zx is taken: the game loads anywhere (save states only within this run)", ALBW_GAME_BASE, span);
    }
    void *h = android_dlopen_ext(path, RTLD_NOW, &ext);
    if (!h) { LOG("loader: %s: %s", path, dlerror()); return 1; }
    int (*entry)(int, char **) = (int (*)(int, char **))dlsym(h, "albw_android_main");
    if (!entry) { LOG("loader: no albw_android_main in %s", path); return 1; }
    LOG("loader: %s at %p", path, ext.flags ? ext.reserved_addr : NULL);
    return entry(argc, argv);
}
