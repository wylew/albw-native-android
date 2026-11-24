#!/usr/bin/env bash
# Link the native game: decomp objects + game data + native runtime/platform/SDK + tripwire stubs.
#   tools/link.sh            (after tools/build_decomp.py and tools/gen_data.py)
set -euo pipefail
cd "$(dirname "$0")/.."
B=build
# float model = the 3DS VFP: IEEE single precision in SSE registers (no x87 excess precision),
# no FMA contraction (VFPv2 vmla rounds the product), see native/gameplay/AUDIT.md
FPFLAGS=(-msse2 -mfpmath=sse -ffp-contract=off)
CFLAGS=(-m32 -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing "${FPFLAGS[@]}")
# ALBW_SANITIZE=1: AddressSanitizer + UndefinedBehaviorSanitizer on the native layer (platform,
# runtime, SDK replacements, native game functions) -> build/albw-san, objects in build/nobj-san;
# the decompiled objects stay uninstrumented (ASan still checks their malloc / memcpy / memset
# calls). The normal build/albw is untouched.
NOBJ=$B/nobj OUT=$B/albw LDSAN=()
# ALBW_LINK_OUT=<path>: link a debug / experimental binary there instead of build/albw (the binary the
# player runs: relinking it mid-session means save states taken meanwhile come from a build that
# later disappears; its <path>.ssmap is written next to it)
[[ -n "${ALBW_LINK_OUT:-}" ]] && OUT=$ALBW_LINK_OUT
# ALBW_NOBJ=<dir> + ALBW_EXTRA_CFLAGS="...": native objects built with other flags in their own directory
# (the Windows variant: windows/build_windows.sh, -femulated-tls -fno-stack-protector -DALBW_WINDOWS)
[[ -n "${ALBW_NOBJ:-}" ]] && NOBJ=$ALBW_NOBJ
[[ -n "${ALBW_EXTRA_CFLAGS:-}" ]] && CFLAGS+=($ALBW_EXTRA_CFLAGS)
if [[ "${ALBW_SANITIZE:-0}" == 1 ]]; then
    # globals not instrumented: the link renames / patches objects after compilation and ASan's global
    # registration then reads a broken descriptor at start-up (odr-violation + nested crash)
    SAN=(-fsanitize=address,undefined -fno-sanitize=alignment,function,vptr -fno-sanitize-recover=address -fno-omit-frame-pointer -mllvm -asan-globals=0)
    CFLAGS+=("${SAN[@]}")
    LDSAN=("${SAN[@]}")
    NOBJ=$B/nobj-san OUT=$B/albw-san
fi
# 32-bit SDL3: the distribution's (Arch lib32: /usr/lib32/pkgconfig) or build.sh's own (build/deps)
# the clang build.sh chose (Debian/Ubuntu: /usr/lib/llvm-N/bin, newer than the default clang)
[[ -f build/llvm_bin && -d $(cat build/llvm_bin) ]] && export PATH="$(cat build/llvm_bin):$PATH"
PCP=${ALBW_PKG_CONFIG_PATH:-$PWD/build/deps/sdl3-i686/lib/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig}
SDL=$(PKG_CONFIG_LIBDIR=$PCP pkg-config --cflags --libs sdl3)
# SDL3 built by build.sh: found at run time without installing it
[[ -d build/deps/sdl3-i686/lib ]] && SDL+=" -Wl,-rpath,$PWD/build/deps/sdl3-i686/lib"
# game inputs: the decomp repository next to this one (development) or the in-repo snapshot +
# the .code extracted from your ROM (tools/extract_code.py, build.sh)
DECOMP_MAP=../albw-decomp-ai/shared/eu_usa_map.json; [[ -f $DECOMP_MAP ]] || DECOMP_MAP=decomp/eu_usa_map.json
CODE_BIN=${ALBW_CODE_BIN:-build/rom/eu_code.bin}; [[ -f $CODE_BIN ]] || CODE_BIN=../albw-decomp-ai/rom/eu_code.bin

mkdir -p $B/native
# round 3 stage 1 builds lack FCONV (float<->int Ghidra copies as bit copies): rebuild the affected
# objects with it in place, once per decomp build, until the stage-2 rebuild (tools/fconv_interim.py)
if [[ "${ALBW_FCONV_INTERIM:-1}" == 1 && ( ! -f $B/fconv_interim.stamp || $B/decomp/report.json -nt $B/fconv_interim.stamp ) ]]; then
    python3 tools/fconv_interim.py | tail -1
fi
NATIVE=()
for f in native/platform/platform.c native/runtime/*.c native/audio/mixer.c native/audio/fx.c native/audio/output.c $(ls native/render/*.c native/sdk/*.c native/game/*.c native/robust/*.c native/gameplay/*.c native/compilefix/*.c native/framework/*.c native/core/*.c native/route/*.c 2>/dev/null) $(git ls-files native/hot | grep -E "^native/hot/[^/]+\.c$"); do
    # one object per source path (native/render/h3d.c and native/sdk/h3d.c share a basename)
    o=$NOBJ/${f%.c}.o
    mkdir -p "$(dirname "$o")"
    EXTRA=()
    [[ "$f" == native/game/* || "$f" == native/robust/* || "$f" == native/gameplay/* || "$f" == native/compilefix/* || "$f" == native/framework/* || "$f" == native/core/* || "$f" == native/hot/* || "$f" == native/route/* ]] && EXTRA=(-DALBW_LINK_GAME_003E6AA8 -DALBW_V4 -Inative/route -Inative/robust -Inative/gameplay -Inative/compilefix -Inative/framework -Inative/core -Inative/hot -include native/albw_native.h -I$B/decomp/shared/include -I../albw-decomp-ai/shared/include -w -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types -Wno-error=return-type)  # decompiled code only
    # sound: the real nn::snd layer (native/sdk/snd.c + native/audio) by default; with
    # ALBW_REAL_SND=0 the silent stopgap in snd_silent.c instead (its dsp:: part is always used)
    if [[ "${ALBW_REAL_SND:-1}" == 0 && ( "$f" == native/sdk/snd.c || "$f" == native/audio/* ) ]]; then continue; fi
    if [[ "$f" == native/sdk/snd_silent.c ]]; then
        [[ "${ALBW_REAL_SND:-1}" != 0 ]] && EXTRA+=(-DALBW_REAL_SND)
        rm -f "$o"
    fi
    stale=0
    [[ -f "$o" && "$o" -nt "$f" ]] || stale=1
    # platform.c #includes layout.c, input.c, menu.c (+ menu_font.h): rebuild when any changed
    if [[ "$f" == native/platform/platform.c && $stale == 0 ]]; then
        # every file platform.c #includes: all of native/platform (a fixed list missed new files)
        for inc in native/platform/*.c native/platform/*.h native/render/render.h; do [[ "$inc" -nt "$o" ]] && stale=1; done
    fi
    [[ $stale == 0 ]] || clang "${CFLAGS[@]}" -Inative/include "${EXTRA[@]}" \
        $(PKG_CONFIG_LIBDIR=$PCP pkg-config --cflags sdl3) -c "$f" -o "$o"
    NATIVE+=("$o")
done
# data symbols the native code uses get defined by the data generator too
llvm-nm -u --format=just-symbols "${NATIVE[@]}" | sort -u > $B/native_syms.txt
# game globals that a decomp object defines itself (C++ inline/static-init variables, weak 'V'):
# the generated data must define them too, or the object's copy and the literal-pool references
# (relocated into the generated data) are two different variables (data_00708A50, a singleton)
python3 -c "import json;print('\n'.join(json.load(open('$B/decomp/report.json'))['objects']))" > $B/objs_pre.txt
llvm-nm -A --defined-only @$B/objs_pre.txt 2>/dev/null | awk '$3 ~ /^[VvWw]$/ && $4 ~ /^data_[0-9A-F]{8}$/ {print $4}' \
    | sort -u >> $B/native_syms.txt
# (+ decomp/addr_taken.txt: the list the snapshot's build used. A fresh clone has no Ghidra dump and
# missed 18 entries, e.g. the strings "In" / "Out" of 00254EFC: data placed differently, crash at boot)
{ grep -rhoE "&DAT_00[1-6][0-9a-f]{5}\b" $B/decomp/ghidra_fallback $B/decomp/src native/game native/robust \
      native/gameplay native/sdk native/compilefix native/hot native/framework native/core native/route 2>/dev/null \
      | sed 's/^&//'; [[ -f decomp/addr_taken.txt ]] && cat decomp/addr_taken.txt; } | sort -u > $B/addr_taken_pool.txt
python3 tools/gen_data.py --addr-taken $B/addr_taken_pool.txt --code "$CODE_BIN" \
    --map "$DECOMP_MAP" --extra $B/native_syms.txt >/dev/null
clang --target=i686-linux-gnu -c $B/data/gamedata.s -o $B/data/gamedata.o
# exactly the objects of the current source selection (stale ones from older selections excluded)
python3 -c "import json;print('\n'.join(json.load(open('$B/decomp/report.json'))['objects']))" > $B/objs.txt
echo $B/data/gamedata.o >> $B/objs.txt
ld.lld -m elf_i386 -r -o $B/albw_game.o @$B/objs.txt
# bool results come back in %al only: zero-extending adapters for the decomp's bool functions
# (tools/boolret.py), the converted callers read the whole register
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" | sort -u > $B/native_defs.txt
python3 tools/boolret.py $B/albw_game.o $B/objs.txt $B/objs_bool.txt $B/boolret.s $B/native_defs.txt
ld.lld -m elf_i386 -r -o $B/albw_game.o @$B/objs_bool.txt
clang --target=i686-linux-gnu -c $B/boolret.s -o $B/native/boolret.o
NATIVE+=($B/native/boolret.o)
# names the decomp calls by C++/ad-hoc names -> the function they are (tools/resolve_names.py +
# hand-verified ones)
cat native/symbol_aliases.txt native/symbol_aliases_manual.txt 2>/dev/null | grep -v '^#' | grep . > $B/aliases.txt || true
# a "function" name whose address lies in the data sections is data (prefix-based guesses like g_)
llvm-nm -u --format=just-symbols $B/albw_game.o | python3 -c "
import sys,re
for n in sys.stdin.read().split():
    m=re.match(r'^func_([0-9A-F]{8})$',n)
    if m and 0x693000<=int(m.group(1),16)<0x7AFE34: print(n,'data_'+m.group(1))" >> $B/aliases.txt
[[ -s $B/aliases.txt ]] && llvm-objcopy --redefine-syms=$B/aliases.txt $B/albw_game.o
# wrapped decomp functions (native/wraps.txt): the decompiled body stays reachable as
# func_<ADDR>__decomp (its -ffunction-sections section start) for a native func_<ADDR> wrapper,
# whose strong definition replaces the decomp's weak one for every caller (native/sdk/eft.c)
if [[ -s native/wraps.txt ]]; then
    WRAP=()
    for f in $(grep -v '^#' native/wraps.txt | awk '{print $1}'); do
        WRAP+=(--add-symbol "${f}__decomp=.text.${f}:0,global,function")
    done
    llvm-objcopy "${WRAP[@]}" $B/albw_game.o
fi
# the game runs its static constructors itself (albw_ctors, in its own order): drop the native
# constructor lists the C++ sources generated, so nothing game-side runs before main()
llvm-objcopy --remove-section=.init_array --remove-section=.rel.init_array \
             --remove-section=.ctors --remove-section=.rel.ctors $B/albw_game.o

# functions that return their first argument on ARM (tools/returns_this.py) but whose matched
# source is void: build_decomp exported them as func_<ADDR>__impl; the thunk calls the impl with
# the caller's argument words and returns the first argument (unless a native definition exists)
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" | sort -u > $B/native_defs.txt
llvm-nm --defined-only --format=just-symbols $B/albw_game.o | grep '__impl$' | sort -u | python3 -c "
import sys
native = set(open('$B/native_defs.txt').read().split())
out = ['.text']
for impl in sys.stdin.read().split():
    f = impl[:-len('__impl')]
    if f in native: continue
    out += ['.globl %s' % f, '.type %s,@function' % f, '.p2align 4', '%s:' % f,
            '  push %ebx', '  push %esi', '  push %edi', '  mov 16(%esp), %ebx', '  sub \$64, %esp',
            '  lea 80(%esp), %esi', '  mov %esp, %edi', '  mov \$16, %ecx', '  cld', '  rep movsl',
            '  call %s' % impl, '  add \$64, %esp', '  mov %ebx, %eax', '  pop %edi', '  pop %esi',
            '  pop %ebx', '  ret']
print('\\n'.join(out))
print('.section .note.GNU-stack,\\\"\\\",@progbits')" > $B/thunks.s
clang --target=i686-linux-gnu -c $B/thunks.s -o $B/native/thunks.o
NATIVE+=($B/native/thunks.o)
python3 tools/gen_stubs.py $B/albw_game.o "${NATIVE[@]}" > $B/stubs.c
# a port helper macro (ALBW_*) that a file used without its definition became a call: never stub it
if grep -q '__asm__("ALBW_' $B/stubs.c; then
    echo "link: undefined port helper macro(s) used as functions:"; grep -o '__asm__("ALBW_[A-Za-z0-9_]*' $B/stubs.c | sort -u; exit 1
fi
clang "${CFLAGS[@]}" -w -c $B/stubs.c -o $B/native/stubs.o
# descriptive names (native/names.txt) as aliases of the func_<ADDR> they name
llvm-nm --defined-only --format=just-symbols "${NATIVE[@]}" $B/albw_game.o 2>/dev/null | sort -u > $B/defined_syms.txt
NAMES=($(python3 tools/gen_names.py native/names.txt $B/defined_syms.txt))
# the game's generated data at fixed addresses (tools/gen_data.py sections .albw_ro / .albw_rw /
# .albw_zi): pointers into it keep their values across builds (save states, runtime/ss_convert.c)
# (not in the sanitizer build: i386 ASan needs 0x20000000-0x3FFFFFFF for its shadow memory)
[[ "${ALBW_SANITIZE:-0}" == 1 ]] || \
NAMES+=(-Wl,--section-start=.albw_ro=0x30000000 -Wl,--section-start=.albw_rw=0x30100000
        -Wl,--section-start=.albw_zi=0x30200000)
clang -m32 -no-pie "${LDSAN[@]}" -o $OUT "${NATIVE[@]}" $B/native/stubs.o $B/albw_game.o $SDL -lm -lz -rdynamic "${NAMES[@]}" -Wl,-Map=$OUT.map
# i386 only: float results come back in x87 st(0); direct calls that ignore one (void/int
# prototype of a float-returning function) would leak x87 slots until every float is NaN.
# Relink with pop adapters and redirect those call sites (tools/x87check.py; ALBW_X87FIX=0: off)
if [[ "${ALBW_X87FIX:-1}" != 0 ]]; then
    python3 tools/x87check.py --thunks $OUT > $B/x87pop.s 2>/dev/null
    clang --target=i686-linux-gnu -c $B/x87pop.s -o $B/native/x87pop.o
    clang -m32 -no-pie "${LDSAN[@]}" -o $OUT "${NATIVE[@]}" $B/native/stubs.o $B/albw_game.o $B/native/x87pop.o $SDL -lm -lz -rdynamic "${NAMES[@]}" -Wl,-Map=$OUT.map
    python3 tools/x87check.py --patch $OUT
fi
# functions that run on into the next one (uninitialised Ghidra register stand-ins, BOOT_LOG 137)
python3 tools/fallthrough_bin.py $OUT || echo "link: WARNING: functions without a return (above)"
# save states: the static ranges that hold game state (native/runtime/savestate.c, tools/gen_ssmap.py)
python3 tools/gen_ssmap.py $OUT.map $OUT > $OUT.ssmap || echo "link: gen_ssmap failed (save states unavailable)"
echo "linked $OUT ($(du -h $OUT | cut -f1))"
