/* HD texture packs (render/hdtex.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>
uint64_t hdtex_cityhash64(const void *data, size_t len);   /* CityHash64 v1.1 */
int hdtex_active(void);            /* enabled and a pack with at least one texture */
int hdtex_enabled(void);
void hdtex_set_enabled(int on);
void hdtex_rescan(void);           /* re-read the textures folder (after an import) */
const char *hdtex_status(void);    /* "538 HD textures" / "no pack installed" */
const char *hdtex_root(void);      /* the textures folder */
/* 1: the HD image (RGBA, bottom row first) in *px, *pw x *ph; 0: being decoded (ask again after
 * hdtex_poll returns 1); -1: no replacement */
int hdtex_get(uint64_t hash, int w, int h, int fmt, const uint8_t **px, int *pw, int *ph);
int hdtex_poll(void);              /* GL thread, once per frame */
