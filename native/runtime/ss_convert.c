/* Save states across builds (native/SAVESTATES.md "Other builds").
 *
 * A state holds memory at fixed addresses (game heap, device memory, arena, thread stacks) plus the
 * static data ranges of the binary. After a relink, words that point INTO the binary (function
 * pointers, return addresses, pointers to native statics and string literals) and the static data
 * ranges themselves are at other addresses. The game's generated data (vtables, tables, strings,
 * globals: tools/gen_data.py) sits at fixed addresses (tools/link.sh --section-start), so the bulk of
 * the pointers do not move at all.
 *
 * Saving appends a trailer (older loaders ignore it):
 *   XLAY  layout of the state's own records (MainHdr / OsThrHdr sizes, offsets)
 *   RNGA  the saved ranges: name, address, size
 *   SYMS  the binary's allocated sections and its symbol table (.symtab + .strtab)
 *   RODA  the binary's .rodata (string literals have no symbols: matched by content)
 *   CODE  the code of every function with a return address on a saved stack
 * Loading a state of another build converts it in memory before the normal load:
 *   - every word that points into the old binary is mapped to the same symbol + offset in this one
 *     (locals keyed by source file + name). In heap-like memory only exact function starts count as
 *     code pointers (a random word hits one of ~65,000 addresses, not a 9 MB range); on stacks and in
 *     register contexts return addresses map by offset when the function is unchanged (same size,
 *     same bytes except relocated operands); a CHANGED function that was mid-call refuses the load;
 *   - static data moves symbol by symbol into this build's ranges (ranges at the same fixed
 *     address, and the named ranges, whole);
 *   - pointers to string literals are matched by the string's bytes.
 * Limits: a state cannot be converted when a function on a saved stack changed (the message names
 * it), when the state records changed layout (os / gfx / fs / renderer structs: their own checks),
 * or across 32 / 64 bit. */
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime.h"
#include "ss_convert.h"

/* the binary whose symbols describe this build (runtime/savestate.c). Android: the game library,
 * loaded at albw_ss_bias with ELF addresses from 0; this file adds the bias to every section and
 * symbol address of this build, and states store them biased (README: Android save states). */
const char *albw_ss_exe_path(void);
#if defined(__ANDROID__)
extern uint32_t albw_ss_bias;
#define SELF_BIAS albw_ss_bias
#else
#define SELF_BIAS 0u
#endif

#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

#if UINTPTR_MAX == 0xFFFFFFFFu
/* ------------------------------------------------------------------ images */
typedef struct { uint32_t addr, size, flags; char name[24]; } Sec;
typedef struct { uint32_t addr, size; const char *name, *file; uint8_t exec, sized, dup; } Sym;   /* dup: key not unique */
typedef struct {
    Sec sec[96];
    int nsec;
    Sym *sym;          /* sorted by address */
    int nsym;
    uint32_t *hash;    /* key -> index + 1, open addressing */
    uint32_t hcap;
    const uint8_t *ro; /* .rodata bytes and address */
    uint32_t ro_addr, ro_size;
    /* old image only: code of the functions on the saved stacks */
    const uint8_t *code;
    size_t code_n;
} Image;

static uint32_t fnv(const char *a, const char *b) {
    uint32_t h = 2166136261u;
    if (a) for (; *a; a++) h = (h ^ (uint8_t)*a) * 16777619u;
    h = (h ^ 1u) * 16777619u;
    for (; *b; b++) h = (h ^ (uint8_t)*b) * 16777619u;
    return h;
}
static int same_key(const Sym *x, const Sym *y) {
    if (strcmp(x->name, y->name)) return 0;
    if (!x->file != !y->file) return 0;
    return !x->file || !strcmp(x->file, y->file);
}
static int cmp_sym(const void *a, const void *b) {
    const Sym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

static const Sec *sec_of(const Image *im, uint32_t a) {
    for (int i = 0; i < im->nsec; i++)
        if (a >= im->sec[i].addr && a - im->sec[i].addr < im->sec[i].size) return &im->sec[i];
    return NULL;
}

/* build sym[] / hash from an ELF32 symbol table */
static int image_syms(Image *im, const Elf32_Sym *st, uint32_t n, const char *str, uint32_t strn, uint32_t bias) {
    im->sym = calloc(n ? n : 1, sizeof *im->sym);
    if (!im->sym) return -1;
    const char *file = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const Elf32_Sym *s = &st[i];
        if (s->st_name >= strn) continue;
        const char *nm = str + s->st_name;
        int type = ELF32_ST_TYPE(s->st_info), bind = ELF32_ST_BIND(s->st_info);
        if (type == STT_FILE) { file = nm; continue; }
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        if (!*nm || nm[0] == '.' || nm[0] == '$' || s->st_shndx == SHN_UNDEF || s->st_shndx >= SHN_LORESERVE) continue;
        const Sec *sc = sec_of(im, s->st_value + bias);
        if (!sc) continue;
        Sym *d = &im->sym[im->nsym++];
        d->addr = s->st_value + bias;
        d->size = s->st_size;
        d->name = nm;
        d->file = bind == STB_LOCAL ? file : NULL;
        d->exec = (sc->flags & SHF_EXECINSTR) != 0;
        d->sized = s->st_size != 0;
    }
    qsort(im->sym, im->nsym, sizeof *im->sym, cmp_sym);
    /* unsized symbols (asm labels, game-data labels): up to the next symbol or the section end */
    for (int i = 0; i < im->nsym; i++) {
        Sym *s = &im->sym[i];
        if (s->size) continue;
        const Sec *sc = sec_of(im, s->addr);
        uint32_t end = sc->addr + sc->size;
        for (int j = i + 1; j < im->nsym; j++)
            if (im->sym[j].addr > s->addr) { if (im->sym[j].addr < end) end = im->sym[j].addr; break; }
        s->size = end - s->addr;
    }
    im->hcap = 1;
    while (im->hcap < (uint32_t)im->nsym * 2 + 16) im->hcap <<= 1;
    im->hash = calloc(im->hcap, sizeof *im->hash);
    if (!im->hash) return -1;
    for (int i = 0; i < im->nsym; i++) {
        uint32_t h = fnv(im->sym[i].file, im->sym[i].name) & (im->hcap - 1);
        while (im->hash[h]) {
            if (same_key(&im->sym[im->hash[h] - 1], &im->sym[i])) {
                /* the same key twice (generated local copies sharing a name: 24 local
                 * `_Z13func_001E30D0v` in the Android build, one at func_00321650's address):
                 * ambiguous, never mapped through (a state's listener 11 delegate was remapped to
                 * the wrong copy, TaskMainGame calc instead of 00321650: endless recursion, A16) */
                im->sym[im->hash[h] - 1].dup = 1;
                im->sym[i].dup = 1;
                break;
            }
            h = (h + 1) & (im->hcap - 1);
        }
        if (!im->hash[h]) im->hash[h] = (uint32_t)i + 1;
    }
    return 0;
}

static const Sym *find_key(const Image *im, const Sym *k) {
    uint32_t h = fnv(k->file, k->name) & (im->hcap - 1);
    while (im->hash[h]) {
        const Sym *s = &im->sym[im->hash[h] - 1];
        if (same_key(s, k)) return s->dup || k->dup ? NULL : s;
        h = (h + 1) & (im->hcap - 1);
    }
    return NULL;
}

/* the symbols containing a (several can share an address: aliases) */
static int sym_at(const Image *im, uint32_t a, int *first) {
    int lo = 0, hi = im->nsym - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (im->sym[m].addr <= a) { r = m; lo = m + 1; } else hi = m - 1;
    }
    if (r < 0) return 0;
    int f = r;
    while (f > 0 && im->sym[f - 1].addr == im->sym[r].addr) f--;
    *first = f;
    return r - f + 1;
}

/* ------------------------------------------------------------------ this binary */
static Image g_self;
static int g_self_ok = -1;
static uint8_t *g_self_file;   /* .symtab / .strtab / section names of /proc/self/exe */

static int self_image(void) {
    if (g_self_ok >= 0) return g_self_ok;
    g_self_ok = 0;
    FILE *f = fopen(albw_ss_exe_path(), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_self_file = malloc((size_t)n);
    if (!g_self_file || fread(g_self_file, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)g_self_file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32) return 0;
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(g_self_file + eh->e_shoff);
    const char *shs = (const char *)g_self_file + sh[eh->e_shstrndx].sh_offset;
    const Elf32_Shdr *sym = NULL, *str = NULL;
    for (int i = 0; i < eh->e_shnum; i++) {
        const char *nm = shs + sh[i].sh_name;
        if (sh[i].sh_type == SHT_SYMTAB) { sym = &sh[i]; str = &sh[sh[i].sh_link]; }
        if ((sh[i].sh_flags & SHF_ALLOC) && sh[i].sh_addr && sh[i].sh_size && g_self.nsec < 96) {
            Sec *s = &g_self.sec[g_self.nsec++];
            s->addr = sh[i].sh_addr + SELF_BIAS;
            s->size = sh[i].sh_size;
            s->flags = sh[i].sh_flags;
            snprintf(s->name, sizeof s->name, "%s", nm);
        }
        if (!strcmp(nm, ".rodata")) { g_self.ro = (const uint8_t *)(uintptr_t)(sh[i].sh_addr + SELF_BIAS); g_self.ro_addr = sh[i].sh_addr + SELF_BIAS; g_self.ro_size = sh[i].sh_size; }
    }
    if (!sym || !str) return 0;
    if (image_syms(&g_self, (const Elf32_Sym *)(g_self_file + sym->sh_offset), sym->sh_size / sizeof(Elf32_Sym),
                   (const char *)g_self_file + str->sh_offset, str->sh_size, SELF_BIAS))
        return 0;
    g_self_ok = 1;
    return 1;
}

/* ------------------------------------------------------------------ state layout */
typedef struct { uint32_t version, mh_size, mh_len_off, mh_sp_off, th_size, th_len_off, th_sp_off, word; } Lay;
enum { P_SKIP, P_HEAP, P_STACK, P_RANGE };
typedef struct { size_t off, len; int pol; uint32_t addr; uint32_t tag; } Blob;
typedef struct {
    Blob b[96];
    int n;
    size_t rngs_off, rngs_end;        /* the RNGS part */
    int nr;                           /* ranges in it */
    size_t body_end;                  /* where the trailer starts */
    const uint8_t *xlay, *rnga, *syms, *roda, *code;
    size_t xlay_n, rnga_n, syms_n, roda_n, code_n;
} Parse;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int add_blob(Parse *ps, size_t off, size_t len, int pol, uint32_t addr, uint32_t tag) {
    if (ps->n >= 96) return -1;
    ps->b[ps->n++] = (Blob){off, len, pol, addr, tag};
    return 0;
}

/* walk a state body (see savestate.c write_body / os.c albw_os_ss_write) */
static int parse_body(const uint8_t *p, size_t n, const Lay *L, Parse *ps, char *why, size_t wl) {
    memset(ps, 0, sizeof *ps);
    size_t o = 0;
#define NEED(k) do { if (o + (k) > n) { snprintf(why, wl, "damaged state (at %zu)", o); return -1; } } while (0)
    NEED(4);
    if (rd32(p) != TAG('M', 'A', 'I', 'N')) { snprintf(why, wl, "damaged state"); return -1; }
    o = 4;
    NEED(L->mh_size);
    uint32_t mlen = rd32(p + o + L->mh_len_off), msp = rd32(p + o + L->mh_sp_off);
    add_blob(ps, o, L->mh_size, P_STACK, 0, TAG('M', 'H', 'D', 'R'));
    o += L->mh_size;
    NEED(mlen);
    add_blob(ps, o, mlen, P_STACK, msp, TAG('M', 'S', 'T', 'K'));
    o += mlen;
    for (;;) {
        if (o == n) break;
        NEED(4);
        uint32_t tag = rd32(p + o);
        if (tag == TAG('R', 'N', 'G', 'S')) {
            ps->rngs_off = o;
            NEED(8);
            ps->nr = (int)rd32(p + o + 4);
            o += 8;
            for (int i = 0; i < ps->nr; i++) {
                NEED(4);
                uint32_t len = rd32(p + o);
                o += 4;
                NEED(len);
                add_blob(ps, o, len, P_RANGE, 0, (uint32_t)i);
                o += len;
            }
            ps->rngs_end = o;
            continue;
        }
        if (tag == TAG('O', 'S', 'T', 'H')) {
            o += 4;
            NEED(L->th_size);
            uint32_t len = rd32(p + o + L->th_len_off), sp = rd32(p + o + L->th_sp_off);
            add_blob(ps, o, L->th_size, P_STACK, 0, tag);
            o += L->th_size;
            NEED(len);
            add_blob(ps, o, len, P_STACK, sp, TAG('T', 'S', 'T', 'K'));
            o += len;
            continue;
        }
        NEED(8);
        uint32_t len = rd32(p + o + 4);
        size_t at = o + 8;
        o = at;
        NEED(len);
        o += len;
        const uint8_t *d = p + at;
        if (tag == TAG('X', 'L', 'A', 'Y')) { if (!ps->body_end) ps->body_end = at - 8; ps->xlay = d; ps->xlay_n = len; continue; }
        if (tag == TAG('R', 'N', 'G', 'A')) { ps->rnga = d; ps->rnga_n = len; continue; }
        if (tag == TAG('S', 'Y', 'M', 'S')) { ps->syms = d; ps->syms_n = len; continue; }
        if (tag == TAG('R', 'O', 'D', 'A')) { ps->roda = d; ps->roda_n = len; continue; }
        if (tag == TAG('C', 'O', 'D', 'E')) { ps->code = d; ps->code_n = len; continue; }
        /* VRAM holds pixels; the small structured records may hold return addresses (contexts) */
        int pol = tag == TAG('V', 'R', 'A', 'M') ? P_SKIP
                : (tag == TAG('M', 'T', 'L', 'S') || tag == TAG('O', 'S', 'K', '1')) ? P_STACK : P_HEAP;
        add_blob(ps, at, len, pol, 0, tag);
    }
    if (!ps->body_end) ps->body_end = n;
#undef NEED
    return 0;
}

/* ------------------------------------------------------------------ save: the trailer */
static void put_sec(SsBuf *b, uint32_t tag, const void *p, size_t n) { ss_section(b, tag, p, n); }

static uint32_t self_func_of(uint32_t a, uint32_t *start) {
    int f, k = sym_at(&g_self, a, &f);
    for (int i = 0; i < k; i++) {
        const Sym *s = &g_self.sym[f + i];
        if (s->exec && a - s->addr < s->size) { *start = s->addr; return s->size; }
    }
    return 0;
}

void ss_convert_write_trailer(SsBuf *b, const SsLayout *lay, const SsRangeInfo *r, int nr) {
    if (!self_image()) return;   /* no symbols: the state loads in this build only */
    Lay L = {1, lay->mh_size, lay->mh_len_off, lay->mh_sp_off, lay->th_size, lay->th_len_off, lay->th_sp_off, 4};
    /* the functions on the saved stacks (from the body written so far) */
    Parse ps;
    char why[160];
    uint32_t *fn = NULL;
    size_t nfn = 0, cap = 0, code_bytes = 0;
    if (parse_body(b->p, b->n, &L, &ps, why, sizeof why) == 0) {
        for (int i = 0; i < ps.n; i++) {
            const Blob *bl = &ps.b[i];
            if (bl->pol != P_STACK) continue;
            size_t s0 = bl->addr ? (4 - bl->addr % 4) % 4 : 0;
            for (size_t o = s0; o + 4 <= bl->len; o += 4) {
                uint32_t v = rd32(b->p + bl->off + o), st, sz = self_func_of(v, &st);
                if (!sz || v == st) continue;
                size_t k;
                for (k = 0; k < nfn && fn[k] != st; k++) {}
                if (k < nfn) continue;
                if (nfn == cap) { cap = cap ? cap * 2 : 64; fn = realloc(fn, cap * sizeof *fn); if (!fn) return; }
                fn[nfn++] = st;
                code_bytes += 8 + sz;
            }
        }
    }
    put_sec(b, TAG('X', 'L', 'A', 'Y'), &L, sizeof L);
    /* RNGA: per range u32 addr, u32 size, char name[24] */
    size_t ra_n = (size_t)nr * 32;
    uint8_t *ra = calloc(1, ra_n ? ra_n : 1);
    for (int i = 0; ra && i < nr; i++) {
        memcpy(ra + i * 32, &r[i].addr, 4);
        memcpy(ra + i * 32 + 4, &r[i].size, 4);
        snprintf((char *)ra + i * 32 + 8, 24, "%s", r[i].name);
    }
    if (ra) put_sec(b, TAG('R', 'N', 'G', 'A'), ra, ra_n);
    free(ra);
    /* SYMS: u32 nsec, Sec[nsec], u32 nsym, Elf32_Sym[nsym], u32 strn, str */
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)g_self_file;
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(g_self_file + eh->e_shoff);
    const Elf32_Shdr *sym = NULL, *str = NULL;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { sym = &sh[i]; str = &sh[sh[i].sh_link]; }
    size_t sn = 4 + g_self.nsec * sizeof(Sec) + 4 + sym->sh_size + 4 + str->sh_size;
    uint8_t *sp = malloc(sn);
    if (sp) {
        uint8_t *q = sp;
        uint32_t v = (uint32_t)g_self.nsec;
        memcpy(q, &v, 4); q += 4;
        memcpy(q, g_self.sec, g_self.nsec * sizeof(Sec)); q += g_self.nsec * sizeof(Sec);
        v = sym->sh_size / sizeof(Elf32_Sym);
        memcpy(q, &v, 4); q += 4;
        memcpy(q, g_self_file + sym->sh_offset, sym->sh_size);
        if (SELF_BIAS)   /* stored as run-time addresses */
            for (Elf32_Sym *e = (Elf32_Sym *)q; e < (Elf32_Sym *)(q + sym->sh_size); e++)
                if (e->st_shndx != SHN_UNDEF && e->st_shndx < SHN_LORESERVE) e->st_value += SELF_BIAS;
        q += sym->sh_size;
        v = str->sh_size;
        memcpy(q, &v, 4); q += 4;
        memcpy(q, g_self_file + str->sh_offset, str->sh_size);
        put_sec(b, TAG('S', 'Y', 'M', 'S'), sp, sn);
        free(sp);
    }
    /* RODA: u32 addr, bytes */
    uint8_t *ro = malloc(4 + g_self.ro_size);
    if (ro) {
        memcpy(ro, &g_self.ro_addr, 4);
        memcpy(ro + 4, g_self.ro, g_self.ro_size);
        put_sec(b, TAG('R', 'O', 'D', 'A'), ro, 4 + g_self.ro_size);
        free(ro);
    }
    /* CODE: per function u32 addr, u32 size, bytes */
    uint8_t *cd = malloc(code_bytes ? code_bytes : 1), *q = cd;
    for (size_t k = 0; cd && k < nfn; k++) {
        uint32_t st, sz = self_func_of(fn[k], &st);
        memcpy(q, &st, 4); memcpy(q + 4, &sz, 4);
        memcpy(q + 8, (const void *)(uintptr_t)st, sz);
        q += 8 + sz;
    }
    if (cd) put_sec(b, TAG('C', 'O', 'D', 'E'), cd, (size_t)(q - cd));
    free(cd);
    free(fn);
}

/* ------------------------------------------------------------------ load: conversion */
typedef struct {
    Image old;
    Image *cur;
    uint32_t old_lo, old_hi;
    long mapped, strings, code, unmapped;
    char why[200];
    int err;
    const uint8_t *rnga;  /* the state's ranges (u32 addr, u32 size, char name[24]) */
    int nrng;
    FILE *log;            /* ALBW_SS_CONVERT_LOG=<file>: every rewritten word */
    uint32_t log_tag;
    const uint8_t *log_base;
    uint32_t log_addr;
} Conv;

static void logw(Conv *c, const uint8_t *at, uint32_t v, uint32_t w, const char *kind, const char *name, uint32_t off) {
    if (!c->log) return;
    char t[5] = {0};
    memcpy(t, &c->log_tag, 4);
    for (int i = 0; i < 4; i++) if (t[i] < 0x20 || t[i] > 0x7E) t[i] = '.';
    fprintf(c->log, "%s +%06lx addr %08x %08x -> %08x %s %s+%#x\n", t, (long)(at - c->log_base),
            c->log_addr ? c->log_addr + (uint32_t)(at - c->log_base) : 0u, v, w, kind, name ? name : "?", off);
}

static const uint8_t *old_code(const Conv *c, uint32_t start, uint32_t *size) {
    const uint8_t *p = c->old.code, *e = p + c->old.code_n;
    while (p + 8 <= e) {
        uint32_t a = rd32(p), n = rd32(p + 4);
        if (a == start) { *size = n; return p + 8; }
        p += 8 + n;
    }
    return NULL;
}

/* same code except relocated operands: every differing byte lies in a 4-byte operand whose value
 * moved by an address-sized amount (call / jump targets, absolute addresses, the GOT base offset
 * `add $imm, %ebx` and GOT-relative data offsets: they all shift with the layout). A same-size
 * change of an instruction or a small constant can pass; it keeps the frame layout, which is
 * what resuming needs. */
static int same_code(const Conv *c, const uint8_t *o, const uint8_t *n, uint32_t len) {
    (void)c;
    for (uint32_t i = 0; i < len; i++) {
        if (o[i] == n[i]) continue;
        int ok = 0;
        for (uint32_t j = i >= 3 ? i - 3 : 0; j <= i && j + 4 <= len; j++) {
            int32_t d = (int32_t)(rd32(n + j) - rd32(o + j));
            if (d != 0 && d > -0x1000000 && d < 0x1000000) {
                int clean = 1;   /* the bytes before j in this window's reach are equal */
                for (uint32_t k = j; k < i; k++) if (o[k] != n[k]) { clean = 0; break; }
                if (clean) { ok = 1; i = j + 3; break; }
            }
        }
        if (!ok) return 0;
    }
    return 1;
}

/* does the old code have a call instruction ending right before a? */
#if !defined(__arm__)
static int after_call(const uint8_t *code, uint32_t start, uint32_t size, uint32_t a) {
    uint32_t off = a - start;
    if (off >= 5 && off <= size && code[off - 5] == 0xE8) return 1;
    for (int k = 2; k <= 7; k++)   /* ff /2 with modrm / sib / disp */
        if (off >= (uint32_t)k && code[off - k] == 0xFF && ((code[off - k + 1] >> 3) & 7) == 2) return 1;
    return 0;
}

/* A return address into a function that changed: if it follows a direct call (E8 rel32), return
 * after the same call in the new code - the k-th call to the same callee (by symbol), k counted
 * from the start of the function in both versions - provided the prologue (pushes + stack frame
 * size, the bytes up to and including the first `sub $imm, %esp`) is byte-identical, so the saved
 * frame still fits. 0 = no safe mapping. (Edits elsewhere in a function that is on a saved stack,
 * e.g. plat_end_frame where states are taken, no longer refuse every older state.) */
static uint32_t callsite_map(Conv *c, const Sym *s, const Sym *n, const uint8_t *oc, uint32_t osz, uint32_t v) {
    uint32_t off = v - s->addr;
    if (off < 5 || off > osz || oc[off - 5] != 0xE8) return 0;
    const uint8_t *nc = (const uint8_t *)(uintptr_t)n->addr;
    /* prologue: up to the first sub $imm8/$imm32, %esp (83 EC ib / 81 EC id) within 32 bytes */
    uint32_t pl = 0;
    for (uint32_t i = 0; i + 2 < 32 && i + 2 < osz; i++) {
        if (oc[i] == 0x83 && oc[i + 1] == 0xEC) { pl = i + 3; break; }
        if (oc[i] == 0x81 && oc[i + 1] == 0xEC) { pl = i + 6; break; }
    }
    if (!pl || pl > n->size || memcmp(oc, nc, pl)) return 0;
    uint32_t otgt = v + (uint32_t)rd32(oc + off - 4);
    int f, k = sym_at(&c->old, otgt, &f);
    const Sym *ot = NULL;
    for (int i = 0; i < k; i++) if (c->old.sym[f + i].addr == otgt) { ot = &c->old.sym[f + i]; break; }
    if (!ot) return 0;
    const Sym *nt = find_key(c->cur, ot);
    if (!nt) return 0;
    int ord = 0;   /* which call to that callee, counted in the old code */
    for (uint32_t i = 0; i + 5 <= off - 5; i++)
        if (oc[i] == 0xE8 && s->addr + i + 5 + rd32(oc + i + 1) == otgt) ord++;
    for (uint32_t i = 0, seen = 0; i + 5 <= n->size; i++)
        if (nc[i] == 0xE8 && n->addr + i + 5 + rd32(nc + i + 1) == nt->addr) {
            if ((int)seen++ == ord) return n->addr + i + 5;
        }
    return 0;
}

/* The save-state entry albw_ss_frame_point itself changed (its frame size too, so callsite_map
 * cannot reuse the frame): a return address right after its call to do_save / do_load goes to
 * albw_ss_fp_tail (savestate.c), which finishes the old call's work and unwinds the old frame.
 * Only for the prologue `push ebp; push ebx; push edi; push esi; sub $F, %esp`. 0 = no mapping. */
extern uint32_t g_ss_fp_tail_frame;
extern char albw_ss_fp_tail[];
static uint32_t frame_point_tail(Conv *c, const Sym *s, const uint8_t *oc, uint32_t osz, uint32_t v) {
    uint32_t off = v - s->addr, frame;
    if (strcmp(s->name, "albw_ss_frame_point") || osz < 10 || off < 5 || off > osz || oc[off - 5] != 0xE8) return 0;
    if (memcmp(oc, "\x55\x53\x57\x56", 4)) return 0;
    if (oc[4] == 0x81 && oc[5] == 0xEC) frame = rd32(oc + 6);
    else if (oc[4] == 0x83 && oc[5] == 0xEC) frame = oc[6];
    else return 0;
    uint32_t otgt = v + (uint32_t)rd32(oc + off - 4);
    int f, k = sym_at(&c->old, otgt, &f);
    for (int i = 0; i < k; i++) {
        const Sym *t = &c->old.sym[f + i];
        if (t->addr == otgt && (!strcmp(t->name, "do_save") || !strcmp(t->name, "do_load"))) {
            g_ss_fp_tail_frame = frame;
            return (uint32_t)(uintptr_t)albw_ss_fp_tail;
        }
    }
    return 0;
}
#else
/* ARM (Android): return addresses follow a bl / blx (ARM state: every game and port function) */
static int arm_bl(uint32_t w) { return (w >> 28) != 0xF && ((w >> 24) & 0xF) == 0xB; }
static int arm_blx_imm(uint32_t w) { return (w >> 25) == 0x7D; }
static int arm_blx_reg(uint32_t w) { return (w >> 28) != 0xF && (w & 0x0FFFFFF0u) == 0x012FFF30u; }
static uint32_t arm_bl_target(uint32_t at, uint32_t w) {
    uint32_t t = at + 8 + (uint32_t)((int32_t)(w << 8) >> 6);
    return arm_blx_imm(w) ? t + (((w >> 24) & 1) << 1) : t;
}
static int after_call(const uint8_t *code, uint32_t start, uint32_t size, uint32_t a) {
    uint32_t off = a - start;
    if ((a & 3) || off < 4 || off > size) return 0;
    uint32_t w = rd32(code + off - 4);
    return arm_bl(w) || arm_blx_imm(w) || arm_blx_reg(w);
}
/* as the x86 version: after the same call (the k-th bl to the same callee) in the new code, if the
 * prologue up to the first `sub sp, sp, #imm` (8 bytes without one) is identical */
static uint32_t callsite_map(Conv *c, const Sym *s, const Sym *n, const uint8_t *oc, uint32_t osz, uint32_t v) {
    uint32_t off = v - s->addr;
    if (off < 4 || off > osz) return 0;
    uint32_t w = rd32(oc + off - 4);
    if (!arm_bl(w)) return 0;
    const uint8_t *nc = (const uint8_t *)(uintptr_t)n->addr;
    uint32_t pl = 8;
    for (uint32_t i = 0; i < 16 && 4 * i + 4 <= osz; i++)
        if ((rd32(oc + 4 * i) & 0x0FFFF000u) == 0x024DD000u) { pl = 4 * i + 4; break; }
    if (pl > osz || pl > n->size || memcmp(oc, nc, pl)) return 0;
    uint32_t otgt = arm_bl_target(v - 4, w);
    int f, k = sym_at(&c->old, otgt, &f);
    const Sym *ot = NULL;
    for (int i = 0; i < k; i++) if (c->old.sym[f + i].addr == otgt) { ot = &c->old.sym[f + i]; break; }
    if (!ot) return 0;
    const Sym *nt = find_key(c->cur, ot);
    if (!nt) return 0;
    int ord = 0;
    for (uint32_t i = 0; i + 4 <= off - 4; i += 4) {
        uint32_t x = rd32(oc + i);
        if (arm_bl(x) && arm_bl_target(s->addr + i, x) == otgt) ord++;
    }
    for (uint32_t i = 0, seen = 0; i + 4 <= n->size; i += 4) {
        uint32_t x = rd32(nc + i);
        if (arm_bl(x) && arm_bl_target(n->addr + i, x) == nt->addr && (int)seen++ == ord) return n->addr + i + 4;
    }
    return 0;
}
/* the x86 frame-point tail is not ported: a state saved in a changed albw_ss_frame_point is refused */
static uint32_t frame_point_tail(Conv *c, const Sym *s, const uint8_t *oc, uint32_t osz, uint32_t v) {
    (void)c; (void)s; (void)oc; (void)osz; (void)v;
    return 0;
}
#endif

/* Heap-like memory (game heap, arena, renderer pools, static data) also holds asset bytes, floats
 * and indices that fall into the binary's range by chance. Game state points into the binary's
 * data only at saved statics (the ranges: decompiled objects' data, sdk modules) and at constants
 * in .rodata (string literals, s_* strings, tables) - not into host modules (window, renderer,
 * audio, input), .eh_frame, the dynamic symbol tables. In .rodata the target must be an object or
 * a string start. Measured on a title -> vane state: the loose rule rewrote ~1,500 random heap
 * words into host statics (menu_vtx, draw.before, the audio ring ...); this one rewrites none. */
static int heap_target_ok(const Conv *c, const Sec *os, uint32_t v, uint32_t off, int statics) {
    for (int r = 0; r < c->nrng; r++) {
        uint32_t a = rd32(c->rnga + r * 32), n = rd32(c->rnga + r * 32 + 4);
        if (v >= a && v - a < n) return 1;
    }
    if (strcmp(os->name, ".rodata")) return 0;
    if (off == 0) return 1;
    if (!c->old.ro || v < c->old.ro_addr || v - c->old.ro_addr >= c->old.ro_size) return 0;
    const uint8_t *q = c->old.ro + (v - c->old.ro_addr);
    if (statics) return q[0] >= 0x20 && q[0] < 0x7F;   /* static data may point at a merged string tail */
    return v > c->old.ro_addr && q[-1] == 0 && q[0] >= 0x20 && q[0] < 0x7F;
}

/* debug: ALBW_SS_CONVERT_OFF=kind[:heap|:range|:stack],... leaves those rewrites out (kinds: func, ret,
 * data, str) - for bisecting a bad conversion */
static int conv_off(const char *kind, int pol) {
    static const char *e;
    static int init;
    if (!init) { init = 1; e = getenv("ALBW_SS_CONVERT_OFF"); }
    if (!e) return 0;
    const char *pn = pol == P_HEAP ? "heap" : pol == P_RANGE ? "range" : "stack";
    char want1[32], want2[32];
    snprintf(want1, sizeof want1, "%s:%s", kind, pn);
    snprintf(want2, sizeof want2, "%s", kind);
    for (const char *q = e; *q;) {
        size_t n = strcspn(q, ",");
        if ((strlen(want1) == n && !strncmp(q, want1, n)) || (strlen(want2) == n && !strncmp(q, want2, n))) return 1;
        q += n + (q[n] == ',');
    }
    return 0;
}

static uint32_t map_word(Conv *c, const uint8_t *at, uint32_t v, int pol) {
    if (v < c->old_lo || v >= c->old_hi) return v;
    const Sec *os = sec_of(&c->old, v);
    if (!os) return v;
    const Sec *ns = NULL;   /* a fixed section (game data) at the same address: unchanged */
    for (int i = 0; i < c->cur->nsec; i++)
        if (!strcmp(c->cur->sec[i].name, os->name)) { ns = &c->cur->sec[i]; break; }
    if (ns && ns->addr == os->addr && !strncmp(os->name, ".albw_", 6)) return v;
    int f, k = sym_at(&c->old, v, &f);
    const Sym *s = NULL, *n = NULL;
    for (int i = 0; i < k; i++) {
        const Sym *x = &c->old.sym[f + i];
        if (v - x->addr >= x->size) continue;
        const Sym *y = find_key(c->cur, x);
        if (!s) s = x;
        if (y) { s = x; n = y; break; }
    }
    uint32_t off = s ? v - s->addr : 0;
    if (os->flags & SHF_EXECINSTR) {
        if (!s) return v;
        if (off == 0) {
            if (n && !conv_off("func", pol)) { c->mapped++; logw(c, at, v, n->addr, "func", s->name, 0); return n->addr; }
            return v;
        }
        if (pol != P_STACK) return v;
        uint32_t osz;
        const uint8_t *oc = old_code(c, s->addr, &osz);
        int is_ret = oc && after_call(oc, s->addr, osz, v);
        if (!is_ret) return v;
        if (!n) {
            if (!c->err) snprintf(c->why, sizeof c->why, "%s was running and is gone in this build", s->name);
            c->err = 1;
            return v;
        }
        /* functions that never block: a save is taken while every game thread waits in the kernel,
         * so a return address into one of these is a stale word left in a reused stack slot (the
         * thread trampoline calls 00106314 before the thread body), not a live frame: keep it */
        /* (also the wall walker's geometry queries - pure maths, no locks or waits: user slot 1 was
         * refused for func_00650A24 after the on-edge fix, 160bi) */
        static const char *const never_blocks[] = { "func_00106314", "albw_fp_set_3ds_mode", "func_003CC230",
                                                    "func_00650A24", "func_0064CB28", "func_0064E9F4",
                                                    "func_001EA738", "func_00649E18", "func_0064B4A0", NULL };
        for (int i = 0; never_blocks[i]; i++)
            if (!strcmp(s->name, never_blocks[i])) return v;
        /* right after `call <next instruction>` (E8 00 00 00 00, the PIC "get my address" trick at the
         * start of a function: the callee is the next line, it returns at once): never a live frame,
         * a leftover word in a reused stack slot - keep it (user slot 4 was refused for
         * func_0064E9F4+0xF after a rebuild changed that function) */
#if !defined(__arm__)
        if (off >= 5 && oc[off - 5] == 0xE8 && rd32(oc + off - 4) == 0) return v;
#endif
        if (n->size != osz || !same_code(c, oc, (const uint8_t *)(uintptr_t)n->addr, osz)) {
            uint32_t na = callsite_map(c, s, n, oc, osz, v);
            if (!na) na = frame_point_tail(c, s, oc, osz, v);
            if (na) {
                c->code++;
                logw(c, at, v, na, "ret-callsite", s->name, off);
                return na;
            }
            if (!c->err) snprintf(c->why, sizeof c->why, "%s was running and changed in this build", s->name);
            logw(c, at, v, v, "REFUSED", s->name, off);   /* which stack word stopped the load */
            c->err = 1;
            return v;
        }
        c->code++;
        logw(c, at, v, n->addr + off, "ret", s->name, off);
        return n->addr + off;
    }
    static int loose = -1;
    if (loose < 0) loose = getenv("ALBW_SS_CONVERT_LOOSE") != NULL;   /* debug: no heap target rules */
    if (!loose && (pol == P_HEAP || pol == P_RANGE) && !heap_target_ok(c, os, v, off, pol == P_RANGE)) {
        c->unmapped++;
        logw(c, at, v, v, "skip", s ? s->name : os->name, off);
        return v;
    }
    /* a string literal has no symbol; an unsized label before it (asm / pool label, size taken up to
     * the next symbol) would map it by offset into whatever follows that label in this build: string
     * starts in .rodata that are not inside a sized object are matched by their bytes below */
    /* (a pointer to a label's own start - a literal-pool word DAT_*, PTR_* - maps by the label) */
    int lit = !strcmp(os->name, ".rodata") && (!s || (!s->sized && off != 0)) && v > c->old.ro_addr && c->old.ro &&
              v - c->old.ro_addr < c->old.ro_size && c->old.ro[v - c->old.ro_addr - 1] == 0;
    if (n && !lit && (off < n->size || !n->size) && !conv_off("data", pol)) { c->mapped++; logw(c, at, v, n->addr + off, "data", s->name, off); return n->addr + off; }
    /* a string literal (no symbol): the same bytes in this build's .rodata */
    if (c->old.ro && v >= c->old.ro_addr && v - c->old.ro_addr < c->old.ro_size && c->cur->ro) {
        const char *str = (const char *)c->old.ro + (v - c->old.ro_addr);
        size_t room = c->old.ro_size - (v - c->old.ro_addr);
        size_t len = strnlen(str, room < 512 ? room : 512);
        if (len >= 2 && len < 512 && (v == c->old.ro_addr || str[-1] == 0 || pol == P_RANGE)) {
            const char *hay = (const char *)c->cur->ro;
            size_t hn = c->cur->ro_size;
            const char *hit = NULL, *tail = NULL;   /* a string start, else a merged tail ("In" in "...In") */
            for (const char *q = memmem(hay, hn, str, len + 1); q; q = memmem(q + 1, hn - (size_t)(q + 1 - hay), str, len + 1)) {
                if (q == hay || q[-1] == 0) { hit = q; break; }
                if (!tail) tail = q;
            }
            if (!hit) hit = tail;
            if (hit && !conv_off("str", pol)) {
                uint32_t w = c->cur->ro_addr + (uint32_t)(hit - hay);
                c->strings++;
                logw(c, at, v, w, "str", str, 0);
                return w;
            }
        }
    }
    /* constant data without a symbol (anonymous tables of native code: their pointers sit in game
     * objects): the same bytes around the target, found once in this build's .rodata. Without this
     * a relink moved them and the user's Eastern Palace state crashed after loading (BOOT_LOG 160ac) */
    if (c->old.ro && c->cur->ro && v >= c->old.ro_addr + 16 && v - c->old.ro_addr + 32 <= c->old.ro_size) {
        const uint8_t *pat = c->old.ro + (v - c->old.ro_addr) - 16;
        const uint8_t *hay = c->cur->ro;
        size_t hn = c->cur->ro_size, pn = 48;
        const uint8_t *hit = NULL;
        int hits = 0;
        for (const uint8_t *q = memmem(hay, hn, pat, pn); q && hits < 2;
             q = memmem(q + 1, hn - (size_t)(q + 1 - hay), pat, pn)) {
            hit = q;
            hits++;
        }
        if (hits == 1 && !conv_off("rodata", pol)) {
            uint32_t w = c->cur->ro_addr + (uint32_t)(hit - hay) + 16;
            c->strings++;
            logw(c, at, v, w, "rodata", os->name, v - os->addr);
            return w;
        }
    }
    c->unmapped++;
    logw(c, at, v, v, "none", s ? s->name : os->name, s ? off : v - os->addr);
    return v;
}

static void reloc(Conv *c, uint8_t *p, size_t len, uint32_t addr, int pol) {
    size_t s0 = addr ? (4 - addr % 4) % 4 : 0;
    c->log_base = p;
    c->log_addr = addr;
    for (size_t o = s0; o + 4 <= len; o += 4) {
        uint32_t v = rd32(p + o), w = map_word(c, p + o, v, pol);
        if (w != v) memcpy(p + o, &w, 4);
    }
}

static int old_image(Conv *c, const Parse *ps) {
    const uint8_t *p = ps->syms, *e = p + ps->syms_n;
    if (p + 4 > e) return -1;
    uint32_t nsec = rd32(p); p += 4;
    if (nsec > 96 || p + nsec * sizeof(Sec) > e) return -1;
    memcpy(c->old.sec, p, nsec * sizeof(Sec));
    c->old.nsec = (int)nsec;
    p += nsec * sizeof(Sec);
    uint32_t nsym = rd32(p); p += 4;
    if (p + (size_t)nsym * sizeof(Elf32_Sym) + 4 > e) return -1;
    const Elf32_Sym *st = (const Elf32_Sym *)p;
    p += (size_t)nsym * sizeof(Elf32_Sym);
    uint32_t strn = rd32(p); p += 4;
    if (p + strn > e) return -1;
    if (image_syms(&c->old, st, nsym, (const char *)p, strn, 0)) return -1;
    c->old_lo = 0xFFFFFFFFu;
    c->old_hi = 0;
    for (int i = 0; i < c->old.nsec; i++) {
        const Sec *s = &c->old.sec[i];
        if (!strncmp(s->name, ".albw_", 6)) continue;   /* fixed game data: never moves */
        if (s->addr < c->old_lo) c->old_lo = s->addr;
        if (s->addr + s->size > c->old_hi) c->old_hi = s->addr + s->size;
    }
    if (ps->roda && ps->roda_n >= 4) {
        c->old.ro_addr = rd32(ps->roda);
        c->old.ro = ps->roda + 4;
        c->old.ro_size = (uint32_t)(ps->roda_n - 4);
    }
    c->old.code = ps->code;
    c->old.code_n = ps->code_n;
    return 0;
}

/* the executable's initial bytes for [addr, addr+size): PROGBITS sections from /proc/self/exe,
 * NOBITS (.bss) zero; bytes outside any section are left as they are */
static void initial_image(uint8_t *dst, uint32_t addr, uint32_t size) {
    static uint8_t *img;
    static size_t imglen;
    if (!img) {
        FILE *f = fopen(albw_ss_exe_path(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END); imglen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        img = malloc(imglen);
        if (!img || fread(img, 1, imglen, f) != imglen) { fclose(f); free(img); img = NULL; return; }
        fclose(f);
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)img;
    if (imglen < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32) return;
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(img + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || (sh[i].sh_flags & SHF_EXECINSTR)) continue;
        uint32_t sa = sh[i].sh_addr + SELF_BIAS;
        uint32_t lo = sa > addr ? sa : addr;
        uint32_t hi = sa + sh[i].sh_size < addr + size ? sa + sh[i].sh_size : addr + size;
        if (lo >= hi) continue;
        if (sh[i].sh_type == SHT_NOBITS) memset(dst + (lo - addr), 0, hi - lo);
        else if (sh[i].sh_offset + (lo - sa) + (hi - lo) <= imglen)
            memcpy(dst + (lo - addr), img + sh[i].sh_offset + (lo - sa), hi - lo);
    }
}

int ss_convert(SsBuf *st, const SsLayout *lay, const SsRangeInfo *cur, int ncur, char *why, size_t wl) {
    Lay L = {1, lay->mh_size, lay->mh_len_off, lay->mh_sp_off, lay->th_size, lay->th_len_off, lay->th_sp_off, 4};
    Parse ps;
    if (parse_body(st->p, st->n, &L, &ps, why, wl)) return -1;
    if (!ps.xlay || !ps.rnga || !ps.syms) {
        snprintf(why, wl, "made by another build that did not record its symbols");
        return -1;
    }
    if (ps.xlay_n != sizeof L || memcmp(ps.xlay, &L, sizeof L)) {
        snprintf(why, wl, "made by another build with a different save-state layout");
        return -1;
    }
    if (!self_image()) { snprintf(why, wl, "this binary has no symbol table"); return -1; }
    Conv *c = calloc(1, sizeof *c);
    if (!c) { snprintf(why, wl, "out of memory"); return -1; }
    c->cur = &g_self;
    const char *lp = getenv("ALBW_SS_CONVERT_LOG");
    if (lp && *lp) c->log = fopen(lp, "w");
    int rc = -1;
    if (old_image(c, &ps)) { snprintf(why, wl, "damaged state (symbols)"); goto out; }
    int nold = (int)(ps.rnga_n / 32);
    c->rnga = ps.rnga;
    c->nrng = nold;
    if (nold != ps.nr) { snprintf(why, wl, "damaged state (ranges)"); goto out; }

    /* 1. words in the memory parts */
    for (int i = 0; i < ps.n; i++) {
        const Blob *b = &ps.b[i];
        c->log_tag = b->tag;
        if (b->pol == P_HEAP || b->pol == P_STACK) reloc(c, st->p + b->off, b->len, b->addr, b->pol);
    }
    if (c->err) { snprintf(why, wl, "%s", c->why); goto out; }

    /* 2. the static ranges, in this build's layout: start from this run's values, move the
     *    state's data in (whole ranges or symbol by symbol), relocate what came from the state.
     *    Native statics ("static" ranges) start from the executable's INITIAL image instead
     *    (.data bytes from the file, .bss zero): a variable that did not exist when the state was
     *    made must not keep this run's pre-load value - e.g. a pointer into the pmem arena, which
     *    the load replaces (crash after loading an older state, BOOT_LOG 155). */
    uint8_t **nb = calloc(ncur ? ncur : 1, sizeof *nb);
    size_t total = 8;
    for (int i = 0; i < ncur; i++) {
        nb[i] = malloc(cur[i].size ? cur[i].size : 1);
        memcpy(nb[i], (const void *)(uintptr_t)cur[i].addr, cur[i].size);
        if (!strcmp(cur[i].name, "static")) initial_image(nb[i], cur[i].addr, cur[i].size);
        total += 4 + cur[i].size;
    }
    const uint8_t *od[256];
    {
        int j = 0;
        for (int i = 0; i < ps.n && j < 256; i++)
            if (ps.b[i].pol == P_RANGE) od[j++] = st->p + ps.b[i].off;
    }
    long moved = 0, lost = 0;
    for (int r = 0; r < nold; r++) {
        uint32_t oa = rd32(ps.rnga + r * 32), on = rd32(ps.rnga + r * 32 + 4);
        const char *oname = (const char *)ps.rnga + r * 32 + 8;
        int whole = -1;
        for (int i = 0; i < ncur && whole < 0; i++) {
            if (cur[i].size != on) continue;
            if (strcmp(oname, "static") ? !strcmp(cur[i].name, oname) : cur[i].addr == oa && !strcmp(cur[i].name, "static")) {
                const Sec *os = sec_of(&c->old, oa), *ns = sec_of(&g_self, cur[i].addr);
                if (strcmp(oname, "static") || (os && ns && !strncmp(os->name, ".albw_", 6) && !strcmp(os->name, ns->name)))
                    whole = i;
            }
        }
        if (whole >= 0) {
            memcpy(nb[whole], od[r], on);
            c->log_tag = TAG('R', 'N', 'G', 'S');
            reloc(c, nb[whole], on, cur[whole].addr, P_RANGE);
            moved += on;
            continue;
        }
        if (strcmp(oname, "static")) { lost += on; continue; }
        if (conv_off("statics", P_RANGE)) continue;   /* debug: keep this run's values */
        for (int s = 0; s < c->old.nsym; s++) {
            const Sym *x = &c->old.sym[s];
            if (x->exec || x->addr < oa || x->addr >= oa + on) continue;
            const Sym *y = find_key(&g_self, x);
            if (!y) { lost += x->size; continue; }
            uint32_t len = x->size < y->size ? x->size : y->size;
            if (x->addr + len > oa + on) len = oa + on - x->addr;
            int done = 0;
            for (int i = 0; i < ncur && !done; i++)
                if (!strcmp(cur[i].name, "static") && y->addr >= cur[i].addr && y->addr + len <= cur[i].addr + cur[i].size) {
                    uint8_t *dst = nb[i] + (y->addr - cur[i].addr);
                    memcpy(dst, od[r] + (x->addr - oa), len);
                    c->log_tag = TAG('R', 'N', 'G', 'S');
                    reloc(c, dst, len, y->addr, P_RANGE);
                    moved += len;
                    done = 1;
                }
            if (!done) lost += len;
        }
    }
    /* 3. the new body: [..RNGS) + new RNGS + [RNGS end..trailer) */
    size_t head = ps.rngs_off, tail = ps.body_end - ps.rngs_end;
    uint8_t *nbody = malloc(head + total + tail);
    if (!nbody) { snprintf(why, wl, "out of memory"); goto out2; }
    memcpy(nbody, st->p, head);
    uint8_t *q = nbody + head;
    uint32_t v = TAG('R', 'N', 'G', 'S');
    memcpy(q, &v, 4); q += 4;
    v = (uint32_t)ncur;
    memcpy(q, &v, 4); q += 4;
    for (int i = 0; i < ncur; i++) {
        memcpy(q, &cur[i].size, 4); q += 4;
        memcpy(q, nb[i], cur[i].size); q += cur[i].size;
    }
    memcpy(q, st->p + ps.rngs_end, tail);
    free(st->p);
    st->p = nbody;
    st->n = st->cap = head + total + tail;
    st->rd = 0;
    fprintf(stderr, "savestate: converted from another build: %ld pointers, %ld return addresses, %ld strings "
                    "remapped, %ld unmapped words; static data %ld bytes moved, %ld not found\n",
            c->mapped, c->code, c->strings, c->unmapped, moved, lost);
    rc = 0;
out2:
    for (int i = 0; i < ncur; i++) free(nb[i]);
    free(nb);
out:
    if (c->log) fclose(c->log);
    free(c->old.sym);
    free(c->old.hash);
    free(c);
    return rc;
}

#else   /* 64-bit: states stay tied to their build */
void ss_convert_write_trailer(SsBuf *b, const SsLayout *lay, const SsRangeInfo *r, int nr) { (void)b; (void)lay; (void)r; (void)nr; }
int ss_convert(SsBuf *st, const SsLayout *lay, const SsRangeInfo *cur, int ncur, char *why, size_t wl) {
    (void)st; (void)lay; (void)cur; (void)ncur;
    snprintf(why, wl, "made by another build of the game");
    return -1;
}
#endif
