#!/usr/bin/env bash
# Link the game as an Android shared library (32-bit ARM, armeabi-v7a): libmain.so, which SDL's
# SDLActivity loads and whose SDL_main (native/runtime/entry.c) it calls.
# The Android counterpart of tools/link.sh; run by tools/build_android.sh, which sets:
#   ALBW_ANDROID_TARGET   clang target, e.g. armv7a-linux-androideabi24 (NDK clang first in PATH)
#   ALBW_SDL_INC          SDL3 include folder;  ALBW_SDL_LIB  folder with the Android libSDL3.so
#   ALBW_SYSLIBS          NDK sysroot libraries for tools/gen_stubs.py (colon separated)
#   ALBW_CODE_BIN         the game code read from the ROM (tools/extract_code.py)
#   ALBW_LINK_OUT         output path (default build/android/libmain.so)
# ALBW_COMPILE_ONLY=1 compiles the native sources and stops (used to check the C code alone).
#
# Differences from the desktop link: ARM (not Thumb), position independent, no boolret adapters
# (ARM returns a whole 0/1 r0 for bool), no x87 fixes, ARM returns-this thunks, game data sections
# writable (they hold relocated pointers: no text relocations in a shared library), no fixed
# section addresses (save states then only load within the run that saved them).
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/android
TARGET=${ALBW_ANDROID_TARGET:-armv7a-linux-androideabi24}
ARCHF=(--target=$TARGET -marm -fPIC -mfloat-abi=softfp -mfpu=neon ${ALBW_ANDROID_EXTRA_FLAGS:-})
CFLAGS=("${ARCHF[@]}" -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffp-contract=off
        -Inative/include-android -DALBW_GLES=1)
NOBJ=$B/nobj OUT=${ALBW_LINK_OUT:-$B/libmain.so}
SDL_INC=${ALBW_SDL_INC:?ALBW_SDL_INC: SDL3 include folder}
SDL_LIB=${ALBW_SDL_LIB:-}
CODE_BIN=${ALBW_CODE_BIN:-build/rom/eu_code.bin}
DECOMP=build/decomp-android
DECOMP_MAP=decomp/eu_usa_map.json
mkdir -p $B/native $B/data

# ---------------------------------------------------------------- native sources
NATIVE=()
for f in native/platform/platform.c native/runtime/*.c native/audio/mixer.c native/audio/fx.c native/audio/output.c \
         $(ls native/render/*.c native/sdk/*.c native/game/*.c native/robust/*.c native/gameplay/*.c native/compilefix/*.c \
              native/framework/*.c native/core/*.c native/route/*.c 2>/dev/null) \
         $(git ls-files native/hot | grep -E "^native/hot/[^/]+\.c$"); do
    case "$f" in native/runtime/debug_spawn.c) continue ;; esac   # desktop debugging helper (gdb)
    [[ "${ALBW_REAL_SND:-1}" == 0 && ( "$f" == native/sdk/snd.c || "$f" == native/audio/* ) ]] && continue
    o=$NOBJ/${f%.c}.o
    mkdir -p "$(dirname "$o")"
    EXTRA=()
    [[ "$f" == native/game/* || "$f" == native/robust/* || "$f" == native/gameplay/* || "$f" == native/compilefix/* || "$f" == native/framework/* || "$f" == native/core/* || "$f" == native/hot/* || "$f" == native/route/* ]] && EXTRA=(-DALBW_LINK_GAME_003E6AA8 -DALBW_V4 -Inative/route -Inative/robust -Inative/gameplay -Inative/compilefix -Inative/framework -Inative/core -Inative/hot -include native/albw_native.h -I$DECOMP/shared/include -w -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types -Wno-error=return-type)
    if [[ "$f" == native/sdk/snd_silent.c ]]; then
        [[ "${ALBW_REAL_SND:-1}" != 0 ]] && EXTRA+=(-DALBW_REAL_SND)
        rm -f "$o"
    fi
    stale=0
    [[ -f "$o" && "$o" -nt "$f" ]] || stale=1
    if [[ "$f" == native/platform/platform.c && $stale == 0 ]]; then
        for inc in native/platform/{layout,input,menu,selector}.c native/platform/{menu_font,layout,gl,selector_img}.h native/render/render.h; do [[ "$inc" -nt "$o" ]] && stale=1; done
    fi
    [[ $stale == 0 ]] || clang "${CFLAGS[@]}" -Inative/include "${EXTRA[@]}" -I"$SDL_INC" -c "$f" -o "$o"
    NATIVE+=("$o")
done
if [[ "${ALBW_COMPILE_ONLY:-0}" == 1 ]]; then echo "compiled ${#NATIVE[@]} native sources"; exit 0; fi

# ---------------------------------------------------------------- game data (from the ROM)
llvm-nm -u --format=just-symbols "${NATIVE[@]}" | sort -u > $B/native_syms.txt
python3 -c "import json;print('\n'.join(json.load(open('$DECOMP/report.json'))['objects']))" > $B/objs_pre.txt
llvm-nm -A --defined-only @$B/objs_pre.txt 2>/dev/null | awk '$3 ~ /^[VvWw]$/ && $4 ~ /^data_[0-9A-F]{8}$/ {print $4}' \
    | sort -u >> $B/native_syms.txt
{ grep -rhoE "&DAT_00[1-6][0-9a-f]{5}\b" $DECOMP/ghidra_fallback $DECOMP/src native/game native/robust \
      native/gameplay native/sdk native/compilefix native/hot native/framework native/core native/route 2>/dev/null \
      | sed 's/^&//'; [[ -f decomp/addr_taken.txt ]] && cat decomp/addr_taken.txt; } | sort -u > $B/addr_taken_pool.txt
# gen_data.py writes build/data/gamedata.s (fixed path): convert it for ARM
python3 tools/gen_data.py --addr-taken $B/addr_taken_pool.txt --code "$CODE_BIN" \
    --map "$DECOMP_MAP" --extra $B/native_syms.txt >/dev/null
# ARM assembler syntax: '@' starts a comment, so section types are %progbits / %nobits; every
# game data section is writable (relocated pointers; Android refuses text relocations)
sed -e 's/@nobits/%nobits/g; s/@progbits/%progbits/g; s/@function/%function/g; s/@object/%object/g' \
    -e 's/^\(\s*\.section [^,]*\),"a"$/\1,"aw"/' build/data/gamedata.s > $B/data/gamedata.s
clang "${ARCHF[@]}" -c $B/data/gamedata.s -o $B/data/gamedata.o

# ---------------------------------------------------------------- game objects
python3 -c "import json;print('\n'.join(json.load(open('$DECOMP/report.json'))['objects']))" > $B/objs.txt
echo $B/data/gamedata.o >> $B/objs.txt
ld.lld -m armelf_linux_eabi -r -o $B/albw_game.o @$B/objs.txt
cat native/symbol_aliases.txt native/symbol_aliases_manual.txt 2>/dev/null | grep -v '^#' | grep . > $B/aliases.txt || true
llvm-nm -u --format=just-symbols $B/albw_game.o | python3 -c "
import sys,re
for n in sys.stdin.read().split():
    m=re.match(r'^func_([0-9A-F]{8})$',n)
    if m and 0x693000<=int(m.group(1),16)<0x7AFE34: print(n,'data_'+m.group(1))" >> $B/aliases.txt
[[ -s $B/aliases.txt ]] && llvm-objcopy --redefine-syms=$B/aliases.txt $B/albw_game.o
if [[ -s native/wraps.txt ]]; then
    WRAP=()
    for f in $(grep -v '^#' native/wraps.txt | awk '{print $1}'); do
        WRAP+=(--add-symbol "${f}__decomp=.text.${f}:0,global,function")
    done
    llvm-objcopy "${WRAP[@]}" $B/albw_game.o
fi
llvm-objcopy --remove-section=.init_array --remove-section=.rel.init_array \
             --remove-section=.ctors --remove-section=.rel.ctors $B/albw_game.o

# ---------------------------------------------------------------- returns-this thunks (ARM)
# like link.sh: call func_X__impl with the caller's argument words (r0-r3 + 12 stack words) and
# return the first argument
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" | sort -u > $B/native_defs.txt
llvm-nm --defined-only --format=just-symbols $B/albw_game.o | grep '__impl$' | sort -u | python3 -c "
import sys
native = set(open('$B/native_defs.txt').read().split())
out = ['.text', '.syntax unified', '.arm']
for impl in sys.stdin.read().split():
    f = impl[:-len('__impl')]
    if f in native: continue
    out += ['.globl %s' % f, '.type %s,%%function' % f, '.p2align 2', '%s:' % f,
            '  push {r4, r5, r6, lr}', '  mov r4, r0', '  sub sp, sp, #48', '  add r5, sp, #64',
            '  mov r12, #0', '1:', '  ldr r6, [r5, r12]', '  str r6, [sp, r12]', '  add r12, r12, #4',
            '  cmp r12, #48', '  blt 1b', '  bl %s' % impl, '  add sp, sp, #48', '  mov r0, r4',
            '  pop {r4, r5, r6, pc}']
print('\n'.join(out))
print('.section .note.GNU-stack,\"\",%progbits')" > $B/thunks.s
clang "${ARCHF[@]}" -c $B/thunks.s -o $B/native/thunks.o
NATIVE+=($B/native/thunks.o)

# ---------------------------------------------------------------- tripwire stubs, names, link
ALBW_ARCH=android-arm python3 tools/gen_stubs.py $B/albw_game.o "${NATIVE[@]}" > $B/stubs.c
if grep -q '__asm__("ALBW_' $B/stubs.c; then
    echo "link: undefined port helper macro(s) used as functions:"; grep -o '__asm__("ALBW_[A-Za-z0-9_]*' $B/stubs.c | sort -u; exit 1
fi
sed -i 's|#include "../native/runtime/runtime.h"|#include "../../native/runtime/runtime.h"|' $B/stubs.c
clang "${CFLAGS[@]}" -w -c $B/stubs.c -o $B/native/stubs.o
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" $B/albw_game.o 2>/dev/null | sort -u > $B/defined_syms.txt
NAMES=($(python3 tools/gen_names.py native/names.txt $B/defined_syms.txt))
LIBS=(-lm -lz -ldl -llog -landroid)
[[ -n "$SDL_LIB" ]] && LIBS=(-L"$SDL_LIB" -lSDL3 "${LIBS[@]}")
clang "${ARCHF[@]}" -shared -o "$OUT" "${NATIVE[@]}" $B/native/stubs.o $B/albw_game.o "${LIBS[@]}" \
    -Wl,--export-dynamic -Wl,-soname,libmain.so -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    "${NAMES[@]}" -Wl,-Map=$OUT.map
echo "linked $OUT ($(du -h "$OUT" | cut -f1))"
