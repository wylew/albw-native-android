/* Native replacement of the nn::fs layer (file system) of the 3DS SDK.
 *
 * LEVEL CHOSEN
 * The game's nn::fs code is plain user-mode C down to a thin layer of one-function-per-command
 * fs:USER IPC wrappers (each writes one command header to TLS+0x80 and issues svc 0x32).  Above
 * that layer the SDK itself
 *   - parses the RomFS (level-3 header, directory/file hash tables, metadata, file data) through a
 *     generic "file" object whose vtable[0] is "read N bytes at offset X" (USA 0010DC6C reads the
 *     0x28-byte level-3 header at offset 0 of the file opened with OpenFileDirectly(SelfNCCH,
 *     {type 0 = RomFS}); USA 0010F240 builds the RomFS archive on top of that file object), and
 *   - implements archives / mount names / file streams / path conversion for save data.
 * So the RomFS is replaced at the "raw bytes" level: the file opened by OpenFileDirectly on the
 * SelfNCCH archive is the IVFC level-3 region of the user's ROM (the same bytes the 3DS file
 * system service serves), read straight from the .3ds with pread().  Save data and extra data are
 * replaced at the archive/file level by a host directory.  Every replaced function is one of the
 * IPC wrappers (or a destructor that ends in svc CloseHandle); everything above stays decompiled.
 * Nothing here interprets an IPC buffer or a svc: the functions are reimplemented from their
 * meaning (open/read/write/close a file, open/format an archive...).
 *
 * ROM:      env ALBW_ROM, else the path build.sh saved in ~/.config/albw-port/rom_path, else rom.3ds.  NCSD (.3ds/.cci) or bare NCCH
 *           (.cxi), must be decrypted (checked via the plaintext IVFC magic).
 * Save dir: env ALBW_SAVE_DIR (default $HOME/.local/share/albw-port/save):
 *             savedata/             archive 0x4 (SaveData) - the game's "Save_%d.bin" files etc.
 *             savedata.format       format info written by FormatSaveData; absent = not formatted
 *             extdata/<hi>_<lo>/    archive 0x6 (ExtSaveData), created on open
 *             sharedext/<hi>_<lo>/  archive 0x7 (SharedExtSaveData, e.g. f000000b gamecoin.dat)
 *             sdmc/                 archive 0x9 (SDMC)
 * Logging:  env ALBW_FS_LOG=1 prints every operation to stderr.
 *
 * Handles: file/directory/archive objects live in game memory and only keep a 32-bit handle (file
 * session handle at obj+4 on 3DS, archive handle as a u64 at obj+8).  The native state lives in
 * static tables here; the game only ever sees a small tagged index, so this is 64-bit clean.
 *
 * Argument conventions: callers are decompiled ARM code; a C call passes r0-r3 then the stack
 * words in order.  A u64 argument occupies an even register pair (or 8-aligned stack slot), so the
 * prototypes below contain explicit "pad" words where the ARM ABI skipped a register.  Every
 * signature was read from the EU machine code (eu_code.bin) of the wrapper, not from Ghidra.
 *
 * Result codes (0 = success, negative = failure, layout desc[0:9] module[10:17] summary[21:26]
 * level[27:31]; module 0x11 = FS) are the ones the decompiled callers test:
 *   0025C4BC  open SaveData: desc 0x154..0x18F (not formatted / corrupted) -> FormatSaveData, retry
 *   006522F4  CreateFile:   desc 0xB4..0xC7 (already exists) -> DeleteFile + CreateFile;
 *                            desc 100..0xB3 (not found) tolerated
 *   003CE6EC  CreateFile on SharedExtSaveData f000000b: desc 0xB4..0xC7 tolerated, else fatal
 *   0025C5FC/0025C32C  file open: any negative -> create the file
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../runtime/runtime.h"
#include "../include/gptr.h"

/* ---------------------------------------------------------------------------------------------
 * result codes
 * ------------------------------------------------------------------------------------------- */
#define RES_OK               0
#define RES_FILE_NOT_FOUND   ((int32_t)0xC8804470u) /* desc 112 FileNotFound, NotFound, Status   */
#define RES_PATH_NOT_FOUND   ((int32_t)0xC8804478u) /* desc 120 NotFound (path/archive)          */
#define RES_ALREADY_EXISTS   ((int32_t)0xC82044BEu) /* desc 190 AlreadyExists, NothingHappened   */
#define RES_NOT_FORMATTED    ((int32_t)0xC8A04554u) /* desc 340 NotFormatted, InvalidState       */
#define RES_OUT_OF_RESOURCE  ((int32_t)0xD8604659u) /* the SDK's own FS out-of-memory result     */
#define RES_INVALID_HANDLE   ((int32_t)0xD8E007F7u) /* kernel "invalid handle"                   */
#define RES_ACCESS_DENIED    ((int32_t)0xC8E04582u) /* desc 386 write to read-only, InvalidState */

/* archive ids (nn::fs ArchiveId) */
#define ARCHIVE_SELF_NCCH        0x00000003u
#define ARCHIVE_SAVE_DATA        0x00000004u
#define ARCHIVE_EXT_SAVE_DATA    0x00000006u
#define ARCHIVE_SHARED_EXT_SAVE  0x00000007u
#define ARCHIVE_SDMC             0x00000009u

/* LowPath types */
#define PATH_EMPTY  1u
#define PATH_BINARY 2u
#define PATH_ASCII  3u
#define PATH_UTF16  4u

/* open flags */
#define OPEN_READ   1u
#define OPEN_WRITE  2u
#define OPEN_CREATE 4u

/* ---------------------------------------------------------------------------------------------
 * logging
 * ------------------------------------------------------------------------------------------- */
static int fs_log_level = -1;

static void fs_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fs_log(const char *fmt, ...) {
    if (fs_log_level < 0) {
        const char *e = getenv("ALBW_FS_LOG");
        fs_log_level = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!fs_log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[fs] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void fs_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fs_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[fs] warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------------------------------
 * the ROM: RomFS level 3 and ExeFS
 * ------------------------------------------------------------------------------------------- */
#define ROM_DEFAULT_PATH "rom.3ds"   /* ALBW_ROM overrides (run.sh / build.sh set it) */

static struct {
    int ready;
    int fd;
    uint64_t romfs_l3_off;  /* absolute file offset of the RomFS level-3 image */
    uint64_t romfs_l3_size;
    uint64_t exefs_off;     /* absolute file offset of the ExeFS header */
    uint64_t exefs_size;
} rom;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int read_exact(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *b = buf;
    while (n) {
        ssize_t r = pread(fd, b, n, (off_t)off);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

/* Locate the RomFS level-3 image and the ExeFS inside the ROM.  Called with fs_lock held. */
static void rom_open_locked(void) {
    if (rom.ready)
        return;
    const char *path = getenv("ALBW_ROM");
    static char cfg_path[4096];
    if (!path || !*path) {
        /* started without run.sh: the ROM recorded by build.sh ($XDG_CONFIG_HOME or
         * ~/.config/albw-port/rom_path), then rom.3ds in the working directory */
        char f[4096];
        const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
        f[0] = 0;
        if (xdg && *xdg) snprintf(f, sizeof f, "%s/albw-port/rom_path", xdg);
        else if (home && *home) snprintf(f, sizeof f, "%s/.config/albw-port/rom_path", home);
        FILE *cf = f[0] ? fopen(f, "r") : NULL;
        if (cf) {
            if (fgets(cfg_path, sizeof cfg_path, cf)) cfg_path[strcspn(cfg_path, "\r\n")] = 0;
            fclose(cf);
        }
        path = cfg_path[0] ? cfg_path : ROM_DEFAULT_PATH;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        runtime_fatal("fs: cannot open the ROM '%s' (%s); set ALBW_ROM to your decrypted .3ds/.cxi",
                      path, strerror(errno));
    uint8_t h[0x200];
    if (read_exact(fd, h, sizeof h, 0) != 0)
        runtime_fatal("fs: ROM '%s' is too small", path);

    uint64_t ncch;
    if (!memcmp(h + 0x100, "NCSD", 4)) {
        ncch = (uint64_t)rd32(h + 0x120) * 0x200; /* partition 0 (the game, CXI) */
    } else if (!memcmp(h + 0x100, "NCCH", 4)) {
        ncch = 0;                                  /* bare .cxi */
    } else {
        runtime_fatal("fs: '%s' is neither an NCSD (.3ds) nor an NCCH (.cxi) image", path);
    }
    uint8_t n[0x200];
    if (read_exact(fd, n, sizeof n, ncch) != 0 || memcmp(n + 0x100, "NCCH", 4) != 0)
        runtime_fatal("fs: ROM '%s': no NCCH header at 0x%llx", path, (unsigned long long)ncch);

    rom.exefs_off = ncch + (uint64_t)rd32(n + 0x1A0) * 0x200;
    rom.exefs_size = (uint64_t)rd32(n + 0x1A4) * 0x200;

    uint64_t romfs = ncch + (uint64_t)rd32(n + 0x1B0) * 0x200;
    uint64_t romfs_size = (uint64_t)rd32(n + 0x1B4) * 0x200;
    if (romfs_size == 0)
        runtime_fatal("fs: ROM '%s' has no RomFS", path);
    uint8_t iv[0x60];
    /* Some decryption tools leave the NCCH NoCrypto flag (0x18F bit 2) clear, so plaintext is
     * detected from the IVFC magic instead. */
    if (read_exact(fd, iv, sizeof iv, romfs) != 0 || memcmp(iv, "IVFC", 4) != 0 ||
        rd32(iv + 4) != 0x10000)
        runtime_fatal("fs: ROM '%s': no plaintext RomFS IVFC header at 0x%llx (encrypted dump?)",
                      path, (unsigned long long)romfs);
    /* IVFC: master hash size at 0x08; level 3 = {u64 logical off, u64 size, u32 log2 block}
     * at 0x3C/0x44/0x4C.  Level 3 data follows the 0x60-byte header and the master hash,
     * aligned to the level-3 block size. */
    uint32_t master = rd32(iv + 0x08);
    uint64_t l3_size = rd64(iv + 0x44);
    uint32_t l3_log2 = rd32(iv + 0x4C);
    if (l3_log2 < 4 || l3_log2 > 24)
        runtime_fatal("fs: ROM '%s': bad IVFC level-3 block size 2^%u", path, l3_log2);
    uint64_t blk = (uint64_t)1 << l3_log2;
    uint64_t l3 = (0x60 + (uint64_t)master + blk - 1) & ~(blk - 1);
    if (l3 + l3_size > romfs_size)
        runtime_fatal("fs: ROM '%s': RomFS level 3 exceeds the RomFS", path);
    rom.romfs_l3_off = romfs + l3;
    rom.romfs_l3_size = l3_size;

    uint8_t l3h[4];
    if (read_exact(fd, l3h, 4, rom.romfs_l3_off) != 0 || rd32(l3h) != 0x28)
        runtime_fatal("fs: ROM '%s': RomFS level-3 header not found at 0x%llx", path,
                      (unsigned long long)rom.romfs_l3_off);
    rom.fd = fd;
    rom.ready = 1;
    fs_log("ROM %s: RomFS level 3 at 0x%llx size 0x%llx, ExeFS at 0x%llx", path,
           (unsigned long long)rom.romfs_l3_off, (unsigned long long)rom.romfs_l3_size,
           (unsigned long long)rom.exefs_off);
}

/* Find an ExeFS section by name ("icon", "banner", "logo", ".code").  fs_lock held. */
static int rom_exefs_find_locked(const char *name, uint64_t *off, uint64_t *size) {
    uint8_t h[0x200];
    if (rom.exefs_size == 0 || read_exact(rom.fd, h, sizeof h, rom.exefs_off) != 0)
        return -1;
    for (int i = 0; i < 10; i++) {
        const uint8_t *e = h + i * 16;
        if (e[0] && !strncmp((const char *)e, name, 8)) {
            *off = rom.exefs_off + 0x200 + rd32(e + 8);
            *size = rd32(e + 12);
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------------------------
 * host save directory
 * ------------------------------------------------------------------------------------------- */
static char save_root[PATH_MAX];

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* fs_lock held */
static const char *save_root_locked(void) {
    if (save_root[0])
        return save_root;
    const char *e = getenv("ALBW_SAVE_DIR");
    int r;
    if (e && *e) {
        r = snprintf(save_root, sizeof save_root, "%s", e);
    } else {
        const char *home = getenv("HOME");
        r = snprintf(save_root, sizeof save_root, "%s/.local/share/albw-port/save",
                     home && *home ? home : ".");
    }
    if (r < 0 || (size_t)r >= sizeof save_root)
        runtime_fatal("fs: save directory path too long");
    while (r > 1 && save_root[r - 1] == '/')
        save_root[--r] = 0;
    if (mkdir_p(save_root) != 0)
        runtime_fatal("fs: cannot create the save directory '%s' (%s)", save_root,
                      strerror(errno));
    fs_log("save directory %s", save_root);
    return save_root;
}

/* Names of the folders directly inside `dir` (at most `max`, names cut to 63 characters): the
 * Esc > File > Save set list. Returns how many. */
static char (*list_out)[64];
static int list_n, list_max;
static int list_cb(const char *path, const struct stat *st, int type, struct FTW *f) {
    (void)st;
    if (f->level == 1 && type == FTW_D && list_n < list_max) snprintf(list_out[list_n++], 64, "%s", path + f->base);
    return 0;
}
int albw_list_subdirs(const char *dir, char (*names)[64], int max) {
    list_out = names; list_n = 0; list_max = max;
    nftw(dir, list_cb, 4, FTW_PHYS);
    return list_n;
}

/* A RomFS file by path ("Archive/Lyt_Game.arc"), read whole into a malloc'd buffer (*size bytes),
 * NULL if missing. For the port's own use (platform/bottom_wide.c: the wood of the 16:9 bottom
 * screen), not the game's file service. Walks the level-3 directory / file metadata: entries
 * {parent, sibling, child dir (dirs) | data offset u64 + size u64 (files), first file (dirs),
 * hash next, name length, UTF-16 name}, 0xFFFFFFFF = none. */
void *albw_romfs_read(const char *path, size_t *size) {
    void *out = NULL;
    pthread_mutex_lock(&fs_lock);
    rom_open_locked();
    uint8_t h[0x28];
    uint8_t *dm = NULL, *fm = NULL;
    if (read_exact(rom.fd, h, sizeof h, rom.romfs_l3_off) != 0) goto done;
    uint32_t dmo = rd32(h + 0x0C), dms = rd32(h + 0x10), fmo = rd32(h + 0x1C), fms = rd32(h + 0x20), fdo = rd32(h + 0x24);
    dm = malloc(dms);
    fm = malloc(fms);
    if (!dm || !fm || read_exact(rom.fd, dm, dms, rom.romfs_l3_off + dmo) != 0 ||
        read_exact(rom.fd, fm, fms, rom.romfs_l3_off + fmo) != 0)
        goto done;
    uint32_t dir = 0;   /* the root */
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int is_dir = e != NULL;
        uint32_t o = rd32(dm + dir + (is_dir ? 8 : 12));   /* first child directory / first file */
        while (o != 0xFFFFFFFFu) {
            const uint8_t *ent = is_dir ? dm + o : fm + o;
            uint32_t nl = rd32(ent + (is_dir ? 20 : 28));
            const uint8_t *nm = ent + (is_dir ? 24 : 32);
            int same = nl == n * 2;
            for (size_t i = 0; same && i < n; i++) same = nm[2 * i] == (uint8_t)p[i] && nm[2 * i + 1] == 0;
            if (same) break;
            o = rd32(ent + 4);   /* sibling */
        }
        if (o == 0xFFFFFFFFu || (is_dir ? o + 24 > dms : o + 32 > fms)) goto done;
        if (is_dir) { dir = o; p = e + 1; continue; }
        uint64_t off = rd64(fm + o + 8), sz = rd64(fm + o + 16);
        if (sz > ((uint64_t)64 << 20) || off + sz > rom.romfs_l3_size) goto done;
        out = malloc(sz ? (size_t)sz : 1);
        if (out && read_exact(rom.fd, out, (size_t)sz, rom.romfs_l3_off + fdo + off) != 0) { free(out); out = NULL; }
        if (out) *size = (size_t)sz;
        break;
    }
done:
    free(dm);
    free(fm);
    pthread_mutex_unlock(&fs_lock);
    return out;
}

/* The save folder in use (Esc > File > Save set reads and writes the game's save files here). */
const char *albw_save_root(void) {
    pthread_mutex_lock(&fs_lock);
    const char *r = save_root_locked();
    pthread_mutex_unlock(&fs_lock);
    return r;
}

static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(p);
}

/* delete everything under dir (dir itself is kept / recreated) */
static int wipe_dir(const char *dir) {
    struct stat st;
    if (lstat(dir, &st) == 0 && nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
        return -1;
    return mkdir_p(dir);
}

/* Convert a LowPath (ASCII or UTF-16, game memory) to a safe relative host path in out.
 * Leading/duplicate '/' are dropped; "." components skipped; ".." rejected.  Returns 0 or -1. */
static int lowpath_to_rel(uint32_t type, const void *data, uint32_t size, char *out, size_t cap) {
    char utf8[PATH_MAX];
    size_t n = 0;
    if (!data && size)
        return -1;
    if (type == PATH_ASCII) {
        const char *s = data;
        for (uint32_t i = 0; i < size && s[i]; i++) {
            if (n + 1 >= sizeof utf8)
                return -1;
            utf8[n++] = s[i];
        }
    } else if (type == PATH_UTF16) {
        const uint8_t *s = data;
        uint32_t cnt = size / 2;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t c = (uint32_t)s[2 * i] | (uint32_t)s[2 * i + 1] << 8;
            if (c == 0)
                break;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < cnt) {
                uint32_t c2 = (uint32_t)s[2 * i + 2] | (uint32_t)s[2 * i + 3] << 8;
                if (c2 >= 0xDC00 && c2 < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                    i++;
                }
            }
            if (n + 4 >= sizeof utf8)
                return -1;
            if (c < 0x80) {
                utf8[n++] = (char)c;
            } else if (c < 0x800) {
                utf8[n++] = (char)(0xC0 | c >> 6);
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                utf8[n++] = (char)(0xE0 | c >> 12);
                utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8[n++] = (char)(0xF0 | c >> 18);
                utf8[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
    } else if (type == PATH_EMPTY) {
        n = 0;
    } else {
        return -1;
    }
    utf8[n] = 0;

    size_t o = 0;
    const char *p = utf8;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *q = p;
        while (*q && *q != '/' && *q != '\\')
            q++;
        size_t len = (size_t)(q - p);
        if (len == 1 && p[0] == '.') {
            p = q;
            continue;
        }
        if (len == 2 && p[0] == '.' && p[1] == '.')
            return -1;
        if (o + len + 2 > cap)
            return -1;
        if (o)
            out[o++] = '/';
        memcpy(out + o, p, len);
        o += len;
        p = q;
    }
    if (cap == 0)
        return -1;
    out[o] = 0;
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 * archive table (archive handle = u64 {lo = ARCH_TAG | index+1, hi = 0})
 * ------------------------------------------------------------------------------------------- */
enum { ARCH_FREE = 0, ARCH_ROM, ARCH_HOST };

#define MAX_ARCHIVES 32
#define ARCH_TAG 0x41000000u

static struct {
    int kind;
    uint32_t id;
    char dir[PATH_MAX]; /* ARCH_HOST: archive root */
} archives[MAX_ARCHIVES];

/* fs_lock held; returns the slot or NULL */
static int arch_lookup_locked(uint32_t lo, uint32_t hi) {
    if (hi != 0 || (lo & 0xFF000000u) != ARCH_TAG)
        return -1;
    uint32_t i = (lo & 0xFFFFFFu) - 1;
    if (i >= MAX_ARCHIVES || archives[i].kind == ARCH_FREE)
        return -1;
    return (int)i;
}

/* Directory of a host-backed archive (fs_lock held).  Returns 0, or a result code. */
static int32_t arch_host_dir_locked(uint32_t id, uint32_t ptype, const void *path, uint32_t psize,
                                    char *dir, size_t cap, int for_open) {
    const char *root = save_root_locked();
    int r;
    switch (id) {
    case ARCHIVE_SAVE_DATA: {
        r = snprintf(dir, cap, "%s/savedata", root);
        if (for_open) {
            char fmt[PATH_MAX];
            struct stat st;
            snprintf(fmt, sizeof fmt, "%s/savedata.format", root);
            if (stat(fmt, &st) != 0)
                return RES_NOT_FORMATTED;
        }
        break;
    }
    case ARCHIVE_EXT_SAVE_DATA:
    case ARCHIVE_SHARED_EXT_SAVE: {
        /* binary path {u32 media type, u32 save id low, u32 save id high} */
        if (ptype != PATH_BINARY || psize < 12 || !path)
            return RES_PATH_NOT_FOUND;
        const uint8_t *b = path;
        r = snprintf(dir, cap, "%s/%s/%08x_%08x", root,
                     id == ARCHIVE_EXT_SAVE_DATA ? "extdata" : "sharedext", rd32(b + 8),
                     rd32(b + 4));
        break;
    }
    case ARCHIVE_SDMC:
        r = snprintf(dir, cap, "%s/sdmc", root);
        break;
    default:
        return RES_PATH_NOT_FOUND;
    }
    if (r < 0 || (size_t)r >= cap)
        return RES_PATH_NOT_FOUND;
    if (for_open && mkdir_p(dir) != 0) {
        fs_warn("cannot create archive directory %s: %s", dir, strerror(errno));
        return RES_OUT_OF_RESOURCE;
    }
    return RES_OK;
}

/* host path of an archive-relative LowPath (fs_lock held) */
static int32_t arch_host_path_locked(int a, uint32_t ptype, const void *path, uint32_t psize,
                                     char *out, size_t cap) {
    char rel[PATH_MAX];
    if (archives[a].kind != ARCH_HOST || lowpath_to_rel(ptype, path, psize, rel, sizeof rel) != 0)
        return RES_PATH_NOT_FOUND;
    int r = snprintf(out, cap, "%s%s%s", archives[a].dir, rel[0] ? "/" : "", rel);
    if (r < 0 || (size_t)r >= cap)
        return RES_PATH_NOT_FOUND;
    return RES_OK;
}

static int32_t errno_result(int e, int is_file) {
    switch (e) {
    case ENOENT:
        return is_file ? RES_FILE_NOT_FOUND : RES_PATH_NOT_FOUND;
    case ENOTDIR:
    case EISDIR:
        return RES_PATH_NOT_FOUND;
    case EEXIST:
        return RES_ALREADY_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS:
        return RES_ACCESS_DENIED;
    default:
        return RES_OUT_OF_RESOURCE;
    }
}

/* ---------------------------------------------------------------------------------------------
 * file / directory handle table (handle = HANDLE_TAG | index+1)
 * ------------------------------------------------------------------------------------------- */
enum { H_FREE = 0, H_ROM, H_HOST, H_DIR };

#define MAX_HANDLES 256
#define HANDLE_TAG 0x46000000u

static struct {
    int kind;
    int closed; /* the session was closed (Close command), slot not yet released (CloseHandle) */
    int fd;     /* H_HOST */
    int writable;
    uint64_t base, size; /* H_ROM: absolute region in the ROM */
} handles[MAX_HANDLES];

/* fs_lock held; returns the handle value or 0 */
static uint32_t handle_alloc_locked(int kind) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].kind == H_FREE) {
            memset(&handles[i], 0, sizeof handles[i]);
            handles[i].kind = kind;
            handles[i].fd = -1;
            return HANDLE_TAG | (uint32_t)(i + 1);
        }
    }
    fs_warn("handle table full (%d open files)", MAX_HANDLES);
    return 0;
}

/* fs_lock held; returns the slot index or -1 */
static int handle_lookup_locked(uint32_t h, int allow_closed) {
    if ((h & 0xFF000000u) != HANDLE_TAG)
        return -1;
    uint32_t i = (h & 0xFFFFFFu) - 1;
    if (i >= MAX_HANDLES || handles[i].kind == H_FREE)
        return -1;
    if (handles[i].closed && !allow_closed)
        return -1;
    return (int)i;
}

static void handle_close_locked(int i) {
    if (handles[i].kind == H_HOST && handles[i].fd >= 0)
        close(handles[i].fd);
    handles[i].fd = -1;
    handles[i].closed = 1;
}

/* ---------------------------------------------------------------------------------------------
 * shared open logic
 * ------------------------------------------------------------------------------------------- */

/* Open an archive by id/path: returns result, *slot on success.  fs_lock held. */
static int32_t archive_open_locked(uint32_t id, uint32_t ptype, const void *path, uint32_t psize,
                                   int *slot) {
    int i;
    for (i = 0; i < MAX_ARCHIVES && archives[i].kind != ARCH_FREE; i++)
        ;
    if (i == MAX_ARCHIVES) {
        fs_warn("archive table full");
        return RES_OUT_OF_RESOURCE;
    }
    if (id == ARCHIVE_SELF_NCCH) {
        rom_open_locked();
        archives[i].kind = ARCH_ROM;
    } else {
        int32_t r = arch_host_dir_locked(id, ptype, path, psize, archives[i].dir,
                                         sizeof archives[i].dir, 1);
        if (r != RES_OK)
            return r;
        archives[i].kind = ARCH_HOST;
    }
    archives[i].id = id;
    *slot = i;
    return RES_OK;
}

/* Open a file inside an open archive.  fs_lock held. */
static int32_t file_open_locked(int a, uint32_t ptype, const void *path, uint32_t psize,
                                uint32_t flags, uint32_t *out) {
    if (archives[a].kind == ARCH_ROM) {
        /* SelfNCCH file path: binary {u32 type, char name[8]}: 0 = RomFS, 1 = ExeFS .code,
         * 2 = ExeFS section by name (the game opens "icon" in 003C7EEC). */
        if (ptype != PATH_BINARY || psize < 4 || !path)
            return RES_PATH_NOT_FOUND;
        const uint8_t *b = path;
        uint32_t type = rd32(b);
        uint64_t off, size;
        if (flags & (OPEN_WRITE | OPEN_CREATE))
            return RES_ACCESS_DENIED;
        if (type == 0) {
            off = rom.romfs_l3_off;
            size = rom.romfs_l3_size;
        } else if (type == 1 || type == 2) {
            char name[9] = ".code";
            if (type == 2) {
                if (psize < 12)
                    return RES_PATH_NOT_FOUND;
                memcpy(name, b + 4, 8);
                name[8] = 0;
            }
            if (rom_exefs_find_locked(name, &off, &size) != 0)
                return RES_FILE_NOT_FOUND;
        } else {
            return RES_PATH_NOT_FOUND;
        }
        uint32_t h = handle_alloc_locked(H_ROM);
        if (!h)
            return RES_OUT_OF_RESOURCE;
        int i = handle_lookup_locked(h, 0);
        handles[i].base = off;
        handles[i].size = size;
        *out = h;
        return RES_OK;
    }

    char hp[PATH_MAX];
    int32_t r = arch_host_path_locked(a, ptype, path, psize, hp, sizeof hp);
    if (r != RES_OK)
        return r;
    int oflags = O_CLOEXEC | ((flags & OPEN_WRITE) ? O_RDWR : O_RDONLY);
    if (flags & OPEN_CREATE)
        oflags |= O_CREAT;
    struct stat st;
    if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode))
        return RES_FILE_NOT_FOUND;
    int fd = open(hp, oflags, 0644);
    if (fd < 0)
        return errno_result(errno, 1);
    uint32_t h = handle_alloc_locked(H_HOST);
    if (!h) {
        close(fd);
        return RES_OUT_OF_RESOURCE;
    }
    int i = handle_lookup_locked(h, 0);
    handles[i].fd = fd;
    handles[i].writable = (flags & OPEN_WRITE) != 0;
    *out = h;
    return RES_OK;
}

/* ===========================================================================================
 * fs:USER session commands
 * =========================================================================================== */

/* USA 0010412C (EU 0010412C) fs:USER InitializeWithSdkVersion (cmd 0x08610042).
 * int32 (u32 *session, u32 sdk_version).  asm: r1 -> cmd[1], process-id descriptor.
 * Native: nothing to initialise (the ROM and save dir are opened lazily).  Confidence: high. */
int32_t func_0010412C(const uint32_t *session, uint32_t sdk_version) {
    fs_log("Initialize sdk=0x%08x", sdk_version);
    return RES_OK;
}

/* USA 001020F0 (EU 001020F0) fs:USER SetPriority (cmd 0x08620040).
 * int32 (u32 *session, u32 priority).  Native: no I/O priorities.  Confidence: high. */
int32_t func_001020F0(const uint32_t *session, uint32_t priority) {
    return RES_OK;
}

/* USA 00110740 (EU 00110740) fs:USER GetPriority (cmd 0x08630000).
 * int32 (u32 *session, u32 *out_priority); out = cmd[2].  Confidence: high. */
int32_t func_00110740(const uint32_t *session, uint32_t *out_priority) {
    if (out_priority)
        *out_priority = 0;
    return RES_OK;
}

/* USA 0010F7EC (EU 0010F7EC) fs:USER OpenFileDirectly (cmd 0x08030204).
 * int32 (u32 *session, u32 *out_handle, u32 transaction, u32 archive_id,
 *        u32 arch_path_type, const void *arch_path, u32 arch_path_size,
 *        u32 file_path_type, const void *file_path, u32 file_path_size,
 *        u32 open_flags, u32 attributes)
 * asm: r2,r3 -> cmd[1],cmd[2]; stack0 -> cmd[3]; stack1 -> cmd[10] (buffer of size stack2);
 *      stack2 -> cmd[4]; stack3 -> cmd[5]; stack4 -> cmd[12] (size stack5); stack5 -> cmd[6];
 *      stack6/7 -> cmd[7]/cmd[8]; out handle = cmd[3].
 * Callers: 0010DC6C / 0010EC5C (MountRom: archive 3, empty path, binary {0,0,0} = RomFS),
 *          003C8BF0 (ngword: archive 0x2345678A, another title's content -> not available here).
 * Native: the RomFS "file" is the IVFC level-3 image of the ROM; the SDK parses it itself.
 * Confidence: high. */
int32_t func_0010F7EC(const uint32_t *session, uint32_t *out_handle, uint32_t transaction,
                      uint32_t archive_id, uint32_t arch_path_type, const void *arch_path,
                      uint32_t arch_path_size, uint32_t file_path_type, const void *file_path,
                      uint32_t file_path_size, uint32_t open_flags, uint32_t attributes) {
    pthread_mutex_lock(&fs_lock);
    int a;
    int32_t r = archive_open_locked(archive_id, arch_path_type, arch_path, arch_path_size, &a);
    uint32_t h = 0;
    if (r == RES_OK) {
        r = file_open_locked(a, file_path_type, file_path, file_path_size, open_flags, &h);
        archives[a].kind = ARCH_FREE; /* the temporary archive is not kept */
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("OpenFileDirectly archive=0x%08x ptype=%u flags=%u -> 0x%08x handle 0x%08x", archive_id,
           file_path_type, open_flags, (uint32_t)r, h);
    if (r == RES_OK && out_handle)
        *out_handle = h;
    return r;
}

/* ===========================================================================================
 * fs:USER archive commands
 * =========================================================================================== */

/* USA 003C9764 (EU 003C9740) fs:USER OpenArchive (cmd 0x080C00C2).
 * int32 (u32 *session, u32 out_archive[2], u32 archive_id, u32 path_type, const void *path,
 *        u32 path_size)
 * asm: r2,r3 -> cmd[1],cmd[2]; stack1 -> cmd[3]; stack0 = path buffer; u64 out = cmd[2..3]
 *      stored with strd to r1.
 * Callers: 003C95F8 (SaveData 4, empty path), 003C9544 (SharedExtSaveData 7, binary path),
 *          003C7EEC (SelfNCCH 3).  Not-formatted SaveData returns desc 0x154 so that 0025C4BC
 *          formats it (first boot).  Confidence: high. */
int32_t func_003C9764(const uint32_t *session, uint32_t *out_archive, uint32_t archive_id,
                      uint32_t path_type, const void *path, uint32_t path_size) {
    pthread_mutex_lock(&fs_lock);
    int a = -1;
    int32_t r = archive_open_locked(archive_id, path_type, path, path_size, &a);
    pthread_mutex_unlock(&fs_lock);
    fs_log("OpenArchive id=0x%08x ptype=%u -> 0x%08x slot %d", archive_id, path_type, (uint32_t)r,
           a);
    if (r == RES_OK && out_archive) {
        out_archive[0] = ARCH_TAG | (uint32_t)(a + 1);
        out_archive[1] = 0;
    }
    return r;
}

/* USA 003C97C4 (EU 003C97A0) fs:USER CloseArchive (cmd 0x080E0080).
 * int32 (u32 *session, u32 pad, u32 archive_lo, u32 archive_hi)
 * asm: r1 is reloaded with the header, stm {r1,r2,r3} -> cmd[0..2]: the u64 archive handle is in
 *      the r2:r3 pair and the caller's r1 is unused.
 * Confidence: high. */
int32_t func_003C97C4(const uint32_t *session, uint32_t pad, uint32_t archive_lo,
                      uint32_t archive_hi) {
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    if (a >= 0)
        archives[a].kind = ARCH_FREE;
    pthread_mutex_unlock(&fs_lock);
    fs_log("CloseArchive 0x%08x -> %s", archive_lo, a >= 0 ? "ok" : "unknown");
    return a >= 0 ? RES_OK : RES_INVALID_HANDLE;
}

/* USA 003C98E4 (EU 003C98C0) fs:USER ControlArchive (cmd 0x080D0144).
 * int32 (u32 *session, u32 pad, u32 archive_lo, u32 archive_hi, u32 action,
 *        const void *in, u32 in_size, void *out, u32 out_size)
 * asm: cmd[1..3] = r2,r3,stack0; cmd[4] = stack2, cmd[5] = stack4; cmd[7] = stack1 (read buffer),
 *      cmd[9] = stack3 (write buffer).  Caller 003C7C28 uses action 0 = CommitSaveData.
 * Native: writes go straight to the host files, so commit only flushes them.  Confidence: high
 * (signature), medium (semantics: no transactional rollback). */
int32_t func_003C98E4(const uint32_t *session, uint32_t pad, uint32_t archive_lo,
                      uint32_t archive_hi, uint32_t action, const void *in, uint32_t in_size,
                      void *out, uint32_t out_size) {
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    if (a >= 0 && action == 0) {
        for (int i = 0; i < MAX_HANDLES; i++)
            if (handles[i].kind == H_HOST && !handles[i].closed && handles[i].writable)
                fdatasync(handles[i].fd);
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("ControlArchive 0x%08x action=%u", archive_lo, action);
    if (a < 0)
        return RES_INVALID_HANDLE;
    if (out && out_size)
        memset(out, 0, out_size);
    return RES_OK;
}

/* USA 003C9950 (EU 003C992C) fs:USER FormatSaveData (cmd 0x084C0242).
 * int32 (u32 *session, u32 archive_id, u32 path_type, const void *path, u32 path_size,
 *        u32 block_size, u32 dirs, u32 files, u32 dir_buckets, u32 file_buckets, u32 duplicate)
 * asm: cmd[1..8] = r1,r2,stack0..stack5; cmd[9] byte = ldrb stack6; cmd[11] = r3 (path buffer).
 * Caller 003C7C84(files=4, dirs=1, duplicate=1) via 0025C4BC / 0025C280.
 * Native: wipe <save>/savedata and record the format parameters.  Confidence: high. */
int32_t func_003C9950(const uint32_t *session, uint32_t archive_id, uint32_t path_type,
                      const void *path, uint32_t path_size, uint32_t block_size, uint32_t dirs,
                      uint32_t files, uint32_t dir_buckets, uint32_t file_buckets,
                      uint32_t duplicate) {
    if (archive_id != ARCHIVE_SAVE_DATA) {
        fs_warn("FormatSaveData on archive 0x%08x not supported", archive_id);
        return RES_PATH_NOT_FOUND;
    }
    pthread_mutex_lock(&fs_lock);
    char dir[PATH_MAX], fmt[PATH_MAX];
    int32_t r = arch_host_dir_locked(archive_id, path_type, path, path_size, dir, sizeof dir, 0);
    if (r == RES_OK) {
        snprintf(fmt, sizeof fmt, "%s/savedata.format", save_root_locked());
        unlink(fmt);
        if (wipe_dir(dir) != 0) {
            fs_warn("cannot reset %s: %s", dir, strerror(errno));
            r = RES_OUT_OF_RESOURCE;
        } else {
            FILE *f = fopen(fmt, "w");
            if (!f || fprintf(f, "block_size %u\ndirs %u\nfiles %u\ndir_buckets %u\n"
                                 "file_buckets %u\nduplicate %u\n",
                              block_size, dirs, files, dir_buckets, file_buckets,
                              duplicate & 0xFF) < 0) {
                r = RES_OUT_OF_RESOURCE;
            }
            if (f && fclose(f) != 0)
                r = RES_OUT_OF_RESOURCE;
        }
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("FormatSaveData dirs=%u files=%u dup=%u -> 0x%08x", dirs, files, duplicate & 0xFF,
           (uint32_t)r);
    return r;
}

/* USA 003C97F4 (EU 003C97D0) fs:USER GetFormatInfo (cmd 0x084500C2).
 * int32 (u32 *session, u32 *total_size, u32 *dirs, u32 *files, u8 *duplicate,
 *        u32 archive_id, u32 path_type, const void *path, u32 path_size)
 * asm: cmd[1..3] = stack1, stack2, stack4; buffer stack3; outputs cmd[2]->r1, cmd[3]->r2,
 *      cmd[4]->r3, byte cmd[5]->stack0.
 * Caller 004C36DC: files==4 && dirs==1 && duplicate -> save OK, else "needs format".
 * Confidence: high. */
int32_t func_003C97F4(const uint32_t *session, uint32_t *total_size, uint32_t *dirs,
                      uint32_t *files, uint8_t *duplicate, uint32_t archive_id, uint32_t path_type,
                      const void *path, uint32_t path_size) {
    if (archive_id != ARCHIVE_SAVE_DATA)
        return RES_PATH_NOT_FOUND;
    unsigned bs = 0, d = 0, f = 0, db = 0, fb = 0, dup = 0;
    int32_t r = RES_NOT_FORMATTED;
    pthread_mutex_lock(&fs_lock);
    char fmt[PATH_MAX];
    snprintf(fmt, sizeof fmt, "%s/savedata.format", save_root_locked());
    FILE *fp = fopen(fmt, "r");
    if (fp) {
        if (fscanf(fp, "block_size %u dirs %u files %u dir_buckets %u file_buckets %u duplicate %u",
                   &bs, &d, &f, &db, &fb, &dup) == 6)
            r = RES_OK;
        fclose(fp);
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("GetFormatInfo -> 0x%08x files=%u dirs=%u dup=%u", (uint32_t)r, f, d, dup);
    if (r != RES_OK)
        return r;
    if (total_size)
        *total_size = bs; /* only the block size parameter is known; callers ignore it */
    if (dirs)
        *dirs = d;
    if (files)
        *files = f;
    if (duplicate)
        *duplicate = (uint8_t)dup;
    return RES_OK;
}

/* USA 003C99A8 (EU 003C9984) fs:USER OpenFile (cmd 0x080201C2).
 * int32 (u32 *session, u32 *out_handle, u32 transaction, u32 archive_lo, u32 archive_hi,
 *        u32 path_type, const void *path, u32 path_size, u32 open_flags, u32 attributes)
 * asm: cmd[1] = r2; cmd[2..4] = stack0,stack1,stack2; cmd[5] = stack4; cmd[6] = stack5;
 *      cmd[7] = stack6; cmd[9] = stack3 (path buffer); out = cmd[3].  On ARM r3 is unused (the u64
 *      archive handle is 8-aligned, so it starts at stack0); at the C level that is no parameter:
 *      the converted caller 003C9278 (archive object OpenFile) passes 10 arguments, archive handle
 *      first after the transaction. The earlier 11-parameter form with an explicit `pad` shifted
 *      every argument by one (archive handle 0, path type = the path pointer): fatal D8E007F7 at
 *      003CE074 on a first boot (empty save dir), BOOT_LOG64 stop 11.
 * Confidence: high. */
int32_t func_003C99A8(const uint32_t *session, uint32_t *out_handle, uint32_t transaction,
                      uint32_t archive_lo, uint32_t archive_hi, uint32_t path_type,
                      const void *path, uint32_t path_size, uint32_t open_flags,
                      uint32_t attributes) {
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    uint32_t h = 0;
    int32_t r = a < 0 ? RES_INVALID_HANDLE
                      : file_open_locked(a, path_type, path, path_size, open_flags, &h);
    pthread_mutex_unlock(&fs_lock);
    if (fs_log_level) {
        char rel[PATH_MAX] = "?";
        lowpath_to_rel(path_type, path, path_size, rel, sizeof rel);
        fs_log("OpenFile archive=0x%08x '%s' flags=%u -> 0x%08x handle 0x%08x", archive_lo, rel,
               open_flags, (uint32_t)r, h);
    }
    if (r == RES_OK && out_handle)
        *out_handle = h;
    return r;
}

/* USA 003C96B8 (EU 003C9694) fs:USER CreateFile (cmd 0x08080202).
 * int32 (u32 *session, u32 transaction, u32 archive_lo, u32 archive_hi, u32 path_type,
 *        const void *path, u32 path_size, u32 attributes, u32 size_lo, u32 size_hi)
 * asm: cmd[1..5] = r1,r2,r3,stack0,stack2; cmd[6] = stack3; cmd[7..8] = stack4,stack5 (u64 size);
 *      cmd[10] = stack1 (path buffer).  Caller 003C8E50 (archive object CreateFile).
 * Native: new zero-filled file of the requested size; "already exists" (desc 190) as the save code
 * 006522F4 expects.  Confidence: high. */
int32_t func_003C96B8(const uint32_t *session, uint32_t transaction, uint32_t archive_lo,
                      uint32_t archive_hi, uint32_t path_type, const void *path, uint32_t path_size,
                      uint32_t attributes, uint32_t size_lo, uint32_t size_hi) {
    uint64_t size = (uint64_t)size_hi << 32 | size_lo;
    char hp[PATH_MAX] = "";
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    int32_t r = a < 0 ? RES_INVALID_HANDLE
                      : arch_host_path_locked(a, path_type, path, path_size, hp, sizeof hp);
    if (r == RES_OK) {
        int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            r = errno_result(errno, 0);
        } else {
            if (ftruncate(fd, (off_t)size) != 0) {
                r = errno_result(errno, 1);
                close(fd);
                unlink(hp);
            } else {
                close(fd);
            }
        }
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("CreateFile '%s' size=%llu -> 0x%08x", hp, (unsigned long long)size, (uint32_t)r);
    return r;
}

/* USA 003C9714 (EU 003C96F0) fs:USER DeleteFile (cmd 0x08040142).
 * int32 (u32 *session, u32 transaction, u32 archive_lo, u32 archive_hi, u32 path_type,
 *        const void *path, u32 path_size)
 * asm: cmd[1..5] = r1,r2,r3,stack0,stack2; cmd[7] = stack1 (path buffer).  Caller 003C8EC0.
 * Confidence: high. */
int32_t func_003C9714(const uint32_t *session, uint32_t transaction, uint32_t archive_lo,
                      uint32_t archive_hi, uint32_t path_type, const void *path,
                      uint32_t path_size) {
    char hp[PATH_MAX] = "";
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    int32_t r = a < 0 ? RES_INVALID_HANDLE
                      : arch_host_path_locked(a, path_type, path, path_size, hp, sizeof hp);
    if (r == RES_OK) {
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode))
            r = RES_FILE_NOT_FOUND;
        else if (unlink(hp) != 0)
            r = errno_result(errno, 1);
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("DeleteFile '%s' -> 0x%08x", hp, (uint32_t)r);
    return r;
}

/* USA 003C9880 (EU 003C985C) fs:USER OpenDirectory (cmd 0x080B0102).
 * int32 (u32 *session, u32 *out_handle, u32 archive_lo, u32 archive_hi, u32 path_type,
 *        const void *path, u32 path_size)
 * asm: cmd[1..3] = r2,r3,stack0; cmd[4] = stack2; cmd[6] = stack1 (path buffer); out = cmd[3].
 * Caller 003C8F6C.  The game never reads directory entries (no Directory::Read wrapper exists),
 * so the handle only records that the directory exists.  Confidence: high. */
int32_t func_003C9880(const uint32_t *session, uint32_t *out_handle, uint32_t archive_lo,
                      uint32_t archive_hi, uint32_t path_type, const void *path,
                      uint32_t path_size) {
    char hp[PATH_MAX] = "";
    uint32_t h = 0;
    pthread_mutex_lock(&fs_lock);
    int a = arch_lookup_locked(archive_lo, archive_hi);
    int32_t r = a < 0 ? RES_INVALID_HANDLE
                      : arch_host_path_locked(a, path_type, path, path_size, hp, sizeof hp);
    if (r == RES_OK) {
        struct stat st;
        if (stat(hp, &st) != 0 || !S_ISDIR(st.st_mode))
            r = RES_PATH_NOT_FOUND;
        else if (!(h = handle_alloc_locked(H_DIR)))
            r = RES_OUT_OF_RESOURCE;
    }
    pthread_mutex_unlock(&fs_lock);
    fs_log("OpenDirectory '%s' -> 0x%08x", hp, (uint32_t)r);
    if (r == RES_OK && out_handle)
        *out_handle = h;
    return r;
}

/* ===========================================================================================
 * file session commands (the "session" argument is a pointer to the file handle)
 * =========================================================================================== */

/* USA 003C9A84 (EU 003C9A60) File::Read (cmd 0x080200C2).
 * int32 (u32 *handle, u32 *out_read, u32 offset_lo, u32 offset_hi, void *buffer, u32 size)
 * asm: cmd[1..3] = r2,r3,stack1; cmd[5] = stack0 (buffer); out = cmd[2].
 * Caller 003C90F8 (file object vtable[0]); used by the SDK RomFS reader (0010DC6C, 0010F240...)
 * and the save file streams.  Confidence: high. */
int32_t func_003C9A84(const uint32_t *handle, uint32_t *out_read, uint32_t offset_lo,
                      uint32_t offset_hi, void *buffer, uint32_t size) {
    uint64_t off = (uint64_t)offset_hi << 32 | offset_lo;
    pthread_mutex_lock(&fs_lock);
    int i = handle ? handle_lookup_locked(*handle, 0) : -1;
    int kind = i >= 0 ? handles[i].kind : H_FREE;
    int fd = -1;
    uint64_t base = 0, lim = 0;
    if (kind == H_ROM) {
        fd = rom.fd;
        base = handles[i].base;
        lim = handles[i].size;
    } else if (kind == H_HOST) {
        fd = handles[i].fd;
    }
    pthread_mutex_unlock(&fs_lock);
    if (fd < 0)
        return RES_INVALID_HANDLE;

    uint64_t want = size;
    if (kind == H_ROM)
        want = off >= lim ? 0 : (lim - off < want ? lim - off : want);
    /* ALBW_FS_DELAY_US=<n>: sleep n microseconds per RomFS read (debug aid: reproduces the
     * timing of a cold file cache, which exposed load-order races at boot) */
    static long delay_us = -1;
    if (delay_us < 0) {
        const char *e = getenv("ALBW_FS_DELAY_US");
        delay_us = e ? atol(e) : 0;
    }
    if (delay_us > 0 && kind == H_ROM)
        nanosleep(&(struct timespec){delay_us / 1000000, (delay_us % 1000000) * 1000}, NULL);
    uint64_t done = 0;
    while (done < want) {
        ssize_t n = pread(fd, (uint8_t *)buffer + done, (size_t)(want - done),
                          (off_t)(base + off + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fs_warn("read error on handle 0x%08x: %s", *handle, strerror(errno));
            return RES_OUT_OF_RESOURCE;
        }
        if (n == 0)
            break; /* end of file */
        done += (uint64_t)n;
    }
    if (out_read)
        *out_read = (uint32_t)done;
    fs_log("Read handle 0x%08x off 0x%llx size 0x%x -> 0x%llx", handle ? *handle : 0,
           (unsigned long long)off, size, (unsigned long long)done);
    return RES_OK;
}

/* USA 003C9B08 (EU 003C9AE4) File::Write (cmd 0x08030102).
 * int32 (u32 *handle, u32 *out_written, u32 offset_lo, u32 offset_hi, const void *buffer,
 *        u32 size, u32 flags)
 * asm: cmd[1..2] = r2,r3; cmd[3] = stack1; cmd[4] = stack2 (flush flags); cmd[6] = stack0
 *      (buffer); out = cmd[2].  Callers 003C91C0 (vtable[1] write), 003C9138 (flush: size 0,
 *      flags 0x10001).  Confidence: high. */
int32_t func_003C9B08(const uint32_t *handle, uint32_t *out_written, uint32_t offset_lo,
                      uint32_t offset_hi, const void *buffer, uint32_t size, uint32_t flags) {
    uint64_t off = (uint64_t)offset_hi << 32 | offset_lo;
    pthread_mutex_lock(&fs_lock);
    int i = handle ? handle_lookup_locked(*handle, 0) : -1;
    int kind = i >= 0 ? handles[i].kind : H_FREE;
    int fd = kind == H_HOST ? handles[i].fd : -1;
    int writable = kind == H_HOST && handles[i].writable;
    pthread_mutex_unlock(&fs_lock);
    if (kind == H_FREE || kind == H_DIR)
        return RES_INVALID_HANDLE;
    if (!writable)
        return RES_ACCESS_DENIED;
    uint32_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, (const uint8_t *)buffer + done, size - done, (off_t)(off + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fs_warn("write error on handle 0x%08x: %s", *handle, strerror(errno));
            return errno_result(errno, 1);
        }
        done += (uint32_t)n;
    }
    if (flags & 1)
        fdatasync(fd);
    if (out_written)
        *out_written = done;
    return RES_OK;
}

/* USA 003C9B68 (EU 003C9B44) File::GetSize (cmd 0x08040000).
 * int32 (u32 *handle, u32 out_size[2]); u64 out = cmd[2..3] stored with strd.
 * Caller 00561334 (file object vtable[3]).  Confidence: high. */
int32_t func_003C9B68(const uint32_t *handle, uint32_t *out_size) {
    pthread_mutex_lock(&fs_lock);
    int i = handle ? handle_lookup_locked(*handle, 0) : -1;
    int32_t r = RES_OK;
    uint64_t size = 0;
    if (i < 0 || handles[i].kind == H_DIR) {
        r = RES_INVALID_HANDLE;
    } else if (handles[i].kind == H_ROM) {
        size = handles[i].size;
    } else {
        struct stat st;
        if (fstat(handles[i].fd, &st) != 0)
            r = errno_result(errno, 1);
        else
            size = (uint64_t)st.st_size;
    }
    pthread_mutex_unlock(&fs_lock);
    if (r == RES_OK && out_size) {
        out_size[0] = (uint32_t)size;
        out_size[1] = (uint32_t)(size >> 32);
    }
    return r;
}

/* USA 003C9ADC (EU 003C9AB8) File::Close (cmd 0x08080000).
 * int32 (u32 *handle).  Caller 003C9058 (vtable+0x30), which then calls the destructor
 * 003C9250 that releases the handle (svc CloseHandle).  Native: close the host file; the slot is
 * released by 003C9250.  Confidence: high. */
int32_t func_003C9ADC(const uint32_t *handle) {
    pthread_mutex_lock(&fs_lock);
    int i = handle ? handle_lookup_locked(*handle, 0) : -1;
    if (i >= 0)
        handle_close_locked(i);
    pthread_mutex_unlock(&fs_lock);
    return i >= 0 ? RES_OK : RES_INVALID_HANDLE;
}

/* USA 003C9A1C (EU 003C99F8) File::SetPriority (cmd 0x080A0040).
 * int32 (u32 *handle, u32 priority).  Caller 003C9038 (vtable[6]).  Confidence: high. */
int32_t func_003C9A1C(const uint32_t *handle, uint32_t priority) {
    return RES_OK;
}

/* USA 003C9A4C (EU 003C9A28) File::OpenLinkFile (cmd 0x080C0000): a second session on the same
 * file.  int32 (u32 *handle, u32 *out_handle); out = cmd[3].  Caller 003C9018 (vtable[9]).
 * Confidence: high. */
int32_t func_003C9A4C(const uint32_t *handle, uint32_t *out_handle) {
    pthread_mutex_lock(&fs_lock);
    int i = handle ? handle_lookup_locked(*handle, 0) : -1;
    int32_t r = RES_OK;
    uint32_t h = 0;
    if (i < 0 || handles[i].kind == H_DIR) {
        r = RES_INVALID_HANDLE;
    } else if (!(h = handle_alloc_locked(handles[i].kind))) {
        r = RES_OUT_OF_RESOURCE;
    } else {
        int j = handle_lookup_locked(h, 0);
        handles[j].base = handles[i].base;
        handles[j].size = handles[i].size;
        handles[j].writable = handles[i].writable;
        if (handles[i].kind == H_HOST && (handles[j].fd = fcntl(handles[i].fd, F_DUPFD_CLOEXEC, 0)) < 0) {
            r = errno_result(errno, 1);
            handles[j].kind = H_FREE;
        }
    }
    pthread_mutex_unlock(&fs_lock);
    if (r == RES_OK && out_handle)
        *out_handle = h;
    return r;
}

/* USA 003C9BA0 (EU 003C9B7C) Directory::Close (directory session cmd 0x08020000).
 * int32 (u32 *handle).  Caller 003C9338 (directory object close, then destructor 003C93A0).
 * Confidence: high (command), medium (directory session: only caller is the dir object). */
int32_t func_003C9BA0(const uint32_t *handle) {
    return func_003C9ADC(handle);
}

/* ===========================================================================================
 * object destructors that end in svc 0x23 (CloseHandle)
 * =========================================================================================== */

/* Layout of the SDK's file and directory server objects: {vtable, u32 session handle}
 * (003C8BF0 / 0010DC6C: obj[0] = vtable 006EFAFC, obj[1] = handle). */
struct fs_server_object {
    void *ALBW_GP vtable; /* 4-byte game word (gptr.h) */
    uint32_t handle;
};

static void release_handle(uint32_t h) {
    pthread_mutex_lock(&fs_lock);
    int i = handle_lookup_locked(h, 1);
    if (i >= 0) {
        handle_close_locked(i);
        handles[i].kind = H_FREE;
    }
    pthread_mutex_unlock(&fs_lock);
}

/* USA 003C9250 (EU 003C922C) file server object destructor body (vtable+0x34):
 * if (obj->handle) { svc CloseHandle(obj->handle); obj->handle = 0; } return obj;
 * Native: release the handle slot.  Confidence: high (5 instructions). */
struct fs_server_object *func_003C9250(struct fs_server_object *obj) {
    if (obj->handle != 0) {
        release_handle(obj->handle);
        obj->handle = 0;
    }
    return obj;
}

/* USA 003C93A0 (EU 003C937C) directory server object destructor body, same shape as 003C9250.
 * Confidence: high. */
struct fs_server_object *func_003C93A0(struct fs_server_object *obj) {
    if (obj->handle != 0) {
        release_handle(obj->handle);
        obj->handle = 0;
    }
    return obj;
}

/* ================================================================== save states (savestate.c)
 * The archive and handle tables. ROM handles are positionless (offset per read), so they come
 * back as they were. Host files (save data, open only while the game writes its save) keep the
 * current descriptor if that slot is still the same open file; otherwise they come back closed. */
#include "../runtime/runtime.h"
void albw_fs_ss_write(SsBuf *b) {
    pthread_mutex_lock(&fs_lock);
    ss_section(b, 0x31415246u, archives, sizeof archives);   /* "FRA1" */
    ss_section(b, 0x31485346u, handles, sizeof handles);     /* "FSH1" */
    pthread_mutex_unlock(&fs_lock);
}

int albw_fs_ss_check(SsBuf *b, char *why, size_t whylen) {
    size_t n1, n2;
    if (!ss_get_section(b, 0x31415246u, &n1) || n1 != sizeof archives || !ss_get_section(b, 0x31485346u, &n2) ||
        n2 != sizeof handles) {
        snprintf(why, whylen, "damaged state (files)");
        return -1;
    }
    return 0;
}

void albw_fs_ss_load(SsBuf *b) {
    size_t n;
    const void *arch = ss_get_section(b, 0x31415246u, &n);
    const __typeof__(handles[0]) *hnd = ss_get_section(b, 0x31485346u, &n);
    pthread_mutex_lock(&fs_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        int keep = hnd[i].kind == H_HOST && handles[i].kind == H_HOST && handles[i].fd >= 0 && !handles[i].closed;
        if (!keep && handles[i].kind == H_HOST && handles[i].fd >= 0) close(handles[i].fd);
        int fd = keep ? handles[i].fd : -1;
        handles[i] = hnd[i];
        if (handles[i].kind == H_HOST) {
            handles[i].fd = fd;
            if (fd < 0) handles[i].closed = 1;
        }
    }
    memcpy(archives, arch, sizeof archives);
    /* the archive roots were the saving run's ALBW_SAVE_DIR: move them to this run's, so a save after
     * loading a state goes where this run's saves go (suffixes: arch_host_dir_locked) */
    const char *root = save_root_locked();
    for (int i = 0; i < MAX_ARCHIVES; i++) {
        if (archives[i].kind != ARCH_HOST) continue;
        const char *d = archives[i].dir, *suf = NULL;
        static const char *const keys[] = {"/savedata", "/sdmc", "/extdata/", "/sharedext/"};
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
            for (const char *q = strstr(d, keys[k]); q; q = strstr(q + 1, keys[k]))
                if (!suf || q > suf) suf = q;
        if (!suf) continue;
        char nd[PATH_MAX];
        if (snprintf(nd, sizeof nd, "%s%s", root, suf) >= (int)sizeof nd) continue;
        if (strcmp(nd, d)) {
            if (!strcmp(suf, "/savedata")) {   /* the format record goes with it (mount check) */
                char of[PATH_MAX], nf[PATH_MAX];
                struct stat st;
                snprintf(of, sizeof of, "%.*s/savedata.format", (int)(suf - d), d);
                snprintf(nf, sizeof nf, "%s/savedata.format", root);
                FILE *in = stat(nf, &st) != 0 ? fopen(of, "rb") : NULL;
                if (in) {
                    FILE *out = fopen(nf, "wb");
                    char buf[4096];
                    size_t k;
                    while (out && (k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
                    if (out) fclose(out);
                    fclose(in);
                }
            }
            snprintf(archives[i].dir, sizeof archives[i].dir, "%s", nd);
            if (mkdir_p(nd) != 0) fs_warn("cannot create archive directory %s: %s", nd, strerror(errno));
        }
    }
    pthread_mutex_unlock(&fs_lock);
}
