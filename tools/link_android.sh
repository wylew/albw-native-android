#!/usr/bin/env bash
# Link the game for Android armeabi-v7a as build/android/libmain.so (README: Android port, step 4).
#   tools/build_decomp_arm32.sh      (once: decomp objects -> build/decomp-arm32)
#   tools/link_android.sh            -> build/android/jniLibs/armeabi-v7a/{libmain,libSDL3}.so
# Mirrors tools/link.sh. Differences:
#   - NDK clang, -fPIC -fno-stack-protector (runtime/ss_ctx.c), -fsigned-char (as on i386), softfp VFP; a shared library
#     loaded by SDLActivity (entry: SDL_main in native/runtime/android.c), -Bsymbolic;
#   - no fixed section addresses (0x30000000 is inside ART's heap on Android, README memory map);
#     generated game data is writable (it holds pointers: no text relocations on Android);
#   - no boolret adapters (ARM callees widen bool results themselves, unlike i386 %al), no x87
#     relink, no fall-through check (x86 disassembly); returns-this thunks in ARM assembly.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build
D=$B/decomp-arm32            # decomp objects (build_decomp.py --arch arm32)
A=$B/android                 # everything this script makes
NOBJ=$A/nobj
API=24
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
[[ -x $TC/bin/clang ]] || { echo "Android NDK not found (set ANDROID_NDK_HOME)"; exit 1; }
export PATH=$TC/bin:$PATH ALBW_ARCH=arm32 ALBW_NDK_TOOLCHAIN=$TC ALBW_ANDROID_API=$API
SDLP=$B/deps/sdl3-android-armv7
[[ -f $SDLP/lib/libSDL3.so ]] || { echo "SDL3 for Android missing: android/fetch_deps.sh"; exit 1; }
[[ -f $D/report.json ]] || { echo "$D/report.json missing: tools/build_decomp_arm32.sh"; exit 1; }
TARGET=--target=armv7a-linux-androideabi$API
FPFLAGS=(-mfpu=neon -mfloat-abi=softfp -ffp-contract=off)
CFLAGS=($TARGET -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -fPIC
        -fno-stack-protector -fsigned-char "${FPFLAGS[@]}" -DALBW_ANDROID -include native/runtime/android_compat.h)
SDL_CFLAGS=(-I$SDLP/include)
DECOMP_MAP=decomp/eu_usa_map.json
CODE_BIN=${ALBW_CODE_BIN:-$B/rom/eu_code.bin}
[[ -f $CODE_BIN ]] || { echo "$CODE_BIN missing: python3 tools/extract_code.py <your EU rom>"; exit 1; }
mkdir -p $A/native $A/data

NATIVE=()
FAILED=()
for f in native/platform/platform.c native/runtime/*.c native/audio/mixer.c native/audio/fx.c native/audio/output.c $(ls native/render/*.c native/sdk/*.c native/game/*.c native/robust/*.c native/gameplay/*.c native/compilefix/*.c native/framework/*.c native/core/*.c native/route/*.c native/android/*.c 2>/dev/null) $(git ls-files native/hot | grep -E "^native/hot/[^/]+\.c$"); do
    o=$NOBJ/${f%.c}.o
    mkdir -p "$(dirname "$o")"
    EXTRA=()
    [[ "$f" == native/android/* || "$f" == native/game/* || "$f" == native/robust/* || "$f" == native/gameplay/* || "$f" == native/compilefix/* || "$f" == native/framework/* || "$f" == native/core/* || "$f" == native/hot/* || "$f" == native/route/* ]] && EXTRA=(-DALBW_LINK_GAME_003E6AA8 -DALBW_V4 -Inative/route -Inative/robust -Inative/gameplay -Inative/compilefix -Inative/framework -Inative/core -Inative/hot -include native/albw_native.h -I$D/shared/include -w -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types -Wno-error=return-type)
    if [[ "${ALBW_REAL_SND:-1}" == 0 && ( "$f" == native/sdk/snd.c || "$f" == native/audio/* ) ]]; then continue; fi
    if [[ "$f" == native/sdk/snd_silent.c ]]; then
        [[ "${ALBW_REAL_SND:-1}" != 0 ]] && EXTRA+=(-DALBW_REAL_SND)
        rm -f "$o"
    fi
    stale=0
    # also rebuild when a header every native file sees changes (a stale object kept the old
    # ALBW_VRAM_ADDR and referenced a removed symbol, which then became a tripwire stub)
    [[ -f "$o" && "$o" -nt "$f" && "$o" -nt native/runtime/android_compat.h && "$o" -nt native/albw_native.h \
       && "$o" -nt native/runtime/runtime.h ]] || stale=1
    if [[ "$f" == native/platform/platform.c && $stale == 0 ]]; then
        for inc in native/platform/{layout,input,menu,selector}.c native/platform/{menu_font,layout,gl,selector_img}.h native/render/render.h; do [[ "$inc" -nt "$o" ]] && stale=1; done
    fi
    if [[ $stale == 1 ]]; then
        if ! clang "${CFLAGS[@]}" -Inative/include "${EXTRA[@]}" "${SDL_CFLAGS[@]}" -c "$f" -o "$o" 2> $A/cc.err; then
            FAILED+=("$f"); echo "== $f"; grep -m3 'error:' $A/cc.err || head -3 $A/cc.err; continue
        fi
    fi
    NATIVE+=("$o")
done
if (( ${#FAILED[@]} )); then echo "link_android: ${#FAILED[@]} native file(s) failed to compile"; exit 1; fi

llvm-nm -u --format=just-symbols "${NATIVE[@]}" | sort -u > $A/native_syms.txt
python3 -c "import json;print('\n'.join(json.load(open('$D/report.json'))['objects']))" > $A/objs_pre.txt
llvm-nm -A --defined-only @$A/objs_pre.txt 2>/dev/null | awk '$3 ~ /^[VvWw]$/ && $4 ~ /^data_[0-9A-F]{8}$/ {print $4}' \
    | sort -u >> $A/native_syms.txt
{ grep -rhoE "&DAT_00[1-6][0-9a-f]{5}\b" $D/ghidra_fallback $D/src native/game native/robust \
      native/gameplay native/sdk native/compilefix native/hot native/framework native/core native/route 2>/dev/null \
      | sed 's/^&//'; [[ -f decomp/addr_taken.txt ]] && cat decomp/addr_taken.txt; } | sort -u > $A/addr_taken_pool.txt
python3 tools/gen_data.py --addr-taken $A/addr_taken_pool.txt --code "$CODE_BIN" \
    --map "$DECOMP_MAP" --extra $A/native_syms.txt --report $D/report.json --out $A/data/gamedata_gen.s >/dev/null
# ARM assembler syntax: '@' starts a comment (section types are %progbits / %nobits); all game data
# writable (pointer words need dynamic relocations; Android refuses text relocations)
sed -E -e 's/,@(nobits|progbits)/,%\1/' -e 's/^(\s*\.section\s+[^,]+),"a"/\1,"aw"/' \
    $A/data/gamedata_gen.s > $A/data/gamedata.s
# 3DS VRAM addresses in the literal pools (VRAM-A/B starts and ends, read by Ghidra-derived code as
# DAT_ words) -> Android's VRAM at ALBW_ANDROID_VRAM = 0x3E000000 (native/albw_native.h, sdk/gfx.c;
# README: Android boot stops A4, A6).
# Only these exact values, only in pool sections (0x1F000023 etc. are IPC headers, not addresses).
python3 - $A/data/gamedata.s <<'PY'
import re, sys
p = sys.argv[1]
VR = {0x1F000000, 0x1F180000, 0x1F2FFFFF, 0x1F300000, 0x1F380000, 0x1F5FFFFF}
out, sec, n = [], "", 0
for line in open(p):
    m = re.match(r"\s*\.section\s+([^,\s]+)", line)
    if m: sec = m.group(1)
    m = re.match(r"(\s*\.long )0x([0-9A-Fa-f]{8})\s*$", line)
    if m and sec == ".rodata.albw_pool" and int(m.group(2), 16) in VR:
        line = "%s0x%08X\n" % (m.group(1), int(m.group(2), 16) - 0x1F000000 + 0x3E000000); n += 1
    out.append(line)
open(p, "w").writelines(out)
print("link_android: %d VRAM pool words -> 0x3E000000 + offset" % n)
PY
clang $TARGET -c $A/data/gamedata.s -o $A/data/gamedata.o
python3 -c "import json;print('\n'.join(json.load(open('$D/report.json'))['objects']))" > $A/objs.txt
echo $A/data/gamedata.o >> $A/objs.txt
ld.lld -m armelf -r -o $A/albw_game.o @$A/objs.txt

cat native/symbol_aliases.txt native/symbol_aliases_manual.txt 2>/dev/null | grep -v '^#' | grep . > $A/aliases.txt || true
llvm-nm -u --format=just-symbols $A/albw_game.o | python3 -c "
import sys,re
for n in sys.stdin.read().split():
    m=re.match(r'^func_([0-9A-F]{8})$',n)
    if m and 0x693000<=int(m.group(1),16)<0x7AFE34: print(n,'data_'+m.group(1))" >> $A/aliases.txt
[[ -s $A/aliases.txt ]] && llvm-objcopy --redefine-syms=$A/aliases.txt $A/albw_game.o
if [[ -s native/wraps.txt ]]; then
    WRAP=()
    for f in $(grep -v '^#' native/wraps.txt | awk '{print $1}'); do
        WRAP+=(--add-symbol "${f}__decomp=.text.${f}:0,global,function")
    done
    llvm-objcopy "${WRAP[@]}" $A/albw_game.o
fi
llvm-objcopy --remove-section=.init_array --remove-section=.rel.init_array \
             --remove-section=.ctors --remove-section=.rel.ctors $A/albw_game.o

# returns-this thunks (see link.sh): call func_X__impl with the caller's r0-r3 and 16 stack words,
# return the first argument (kept in r4)
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" | sort -u > $A/native_defs.txt
llvm-nm --defined-only --format=just-symbols $A/albw_game.o | grep '__impl$' | sort -u | python3 -c "
import sys
native = set(open('$A/native_defs.txt').read().split())
out = ['.syntax unified', '.text', '.arm']
for impl in sys.stdin.read().split():
    f = impl[:-len('__impl')]
    if f in native: continue
    out += ['.globl %s' % f, '.type %s,%%function' % f, '.p2align 2', '%s:' % f,
            '  push {r4, lr}', '  mov r4, r0', '  sub sp, sp, #64', '  add ip, sp, #72']
    for k in range(0, 64, 4):
        out += ['  ldr lr, [ip, #%d]' % k, '  str lr, [sp, #%d]' % k]
    out += ['  bl %s' % impl, '  add sp, sp, #64', '  mov r0, r4', '  pop {r4, pc}']
print('\n'.join(out))
print('.section .note.GNU-stack,\"\",%progbits')" > $A/thunks.s
clang $TARGET -c $A/thunks.s -o $A/native/thunks.o
NATIVE+=($A/native/thunks.o)

python3 tools/gen_stubs.py $A/albw_game.o "${NATIVE[@]}" | sed 's|"../native/runtime/runtime.h"|"native/runtime/runtime.h"|' > $A/stubs.c
if grep -q '__asm__("ALBW_' $A/stubs.c; then
    echo "link_android: undefined port helper macro(s) used as functions:"; grep -o '__asm__("ALBW_[A-Za-z0-9_]*' $A/stubs.c | sort -u; exit 1
fi
clang "${CFLAGS[@]}" -I. -w -c $A/stubs.c -o $A/native/stubs.o
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" $A/albw_game.o 2>/dev/null | sort -u > $A/defined_syms.txt
NAMES=($(python3 tools/gen_names.py native/names.txt $A/defined_syms.txt))

OUTDIR=$A/jniLibs/armeabi-v7a
mkdir -p $OUTDIR
OUT=$OUTDIR/libmain.so
clang $TARGET -shared -fPIC -o $OUT "${NATIVE[@]}" $A/native/stubs.o $A/albw_game.o \
    -L$SDLP/lib -lSDL3 -lz -lm -ldl -llog -landroid \
    -Wl,-soname,libmain.so -Wl,-Bsymbolic -Wl,--no-undefined -Wl,-z,max-page-size=16384 \
    "${NAMES[@]}" -Wl,-Map=$A/libmain.map
cp $SDLP/lib/libSDL3.so $OUTDIR/
python3 tools/gen_ssmap.py $A/libmain.map $OUT > $A/libmain.ssmap 2>/dev/null || echo "link_android: gen_ssmap failed (save states unavailable)"
echo "linked $OUT ($(du -h $OUT | cut -f1))"
