#!/usr/bin/env bash
# Build the Android app (32-bit ARM, armeabi-v7a: Retroid Pocket 5 and other devices that still run
# 32-bit apps) from source and your own ROM.
#
#   ./build_android.sh /path/to/your/game.3ds          build build/android/albw-android.apk
#   ./build_android.sh                                  rebuild (ROM path saved by the first run)
#   ./build_android.sh --install ...                    also adb install it on the connected device
#   ./build_android.sh --check                          no ROM: SDL3, the native C code and the APK
#                                                       shell only (CI: .github/workflows/android.yml)
#
# Needs (Linux x86-64): python3, git, cmake, ninja, a JDK 17+, the Android SDK (ANDROID_HOME or
# ANDROID_SDK_ROOT) with platform 35 and build-tools, and the Android NDK r28 or newer (clang 19+;
# ANDROID_NDK_HOME, or the newest one in $ANDROID_HOME/ndk). Gradle downloads itself on the first
# run (android/gradlew). The ROM is the decrypted European dump, like ./build.sh; the APK contains
# game code built from it, so keep the APK to yourself.
#
# What it does: SDL3 for Android (shared), the game code from your ROM, the decompiled code compiled
# for ARM (tools/build_decomp.py --arch android-arm), libmain.so (tools/link_android.sh), then the
# APK (android/: launcher with a ROM chooser + SDLActivity). On the device, the app copies the ROM
# you choose to its own folder (Android/data/io.github.wylew.albw/files/rom.3ds).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/albw-port"
INSTALL=0 CHECK=0 ROM=""
for a in "$@"; do
    case "$a" in
        --install) INSTALL=1 ;;
        --check) CHECK=1 ;;
        -h|--help) sed -n 2,21p "$0"; exit 0 ;;
        *) ROM="$a" ;;
    esac
done
if [[ $CHECK == 0 ]]; then
    [[ -n "$ROM" ]] || ROM="${ALBW_ROM:-$(cat "$CFG/rom_path" 2>/dev/null || true)}"
    [[ -f "$ROM" ]] || { echo "Usage: ./build_android.sh /path/to/your/decrypted.3ds"; exit 1; }
    ROM=$(realpath "$ROM")
fi
JOBS=${ALBW_JOBS:-$(nproc)}
API=24
TARGET=armv7a-linux-androideabi$API
say() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------- tools
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
[[ -d "$SDK" ]] || { echo "Android SDK not found: set ANDROID_HOME (Android Studio: ~/Android/Sdk)"; exit 1; }
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z "$NDK" ]]; then NDK=$(ls -d "$SDK"/ndk/*/ 2>/dev/null | sort -V | tail -1); NDK=${NDK%/}; fi
[[ -n "$NDK" && -f "$NDK/source.properties" ]] || {
    echo "Android NDK not found: install NDK r28+ (Android Studio > SDK Manager > SDK Tools > NDK) or set ANDROID_NDK_HOME"; exit 1; }
NDK_REV=$(sed -nE 's/^Pkg.Revision *= *([0-9]+).*/\1/p' "$NDK/source.properties")
(( NDK_REV >= 28 )) || { echo "NDK $NDK_REV is too old: r28 or newer is needed (clang 19+)"; exit 1; }
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
export PATH="$TC/bin:$PATH"   # clang, ld.lld, llvm-nm / objcopy / readelf / objdump of the NDK
for t in clang ld.lld llvm-nm llvm-objcopy llvm-readelf python3 cmake ninja git java; do
    command -v $t >/dev/null || { echo "missing tool: $t"; exit 1; }
done
SYSROOT_LIB=$TC/sysroot/usr/lib/arm-linux-androideabi/$API
say "NDK r$NDK_REV ($NDK), target $TARGET"

# ---------------------------------------------------------------- 1. SDL3 for Android
JNI=$ROOT/android/app/src/main/jniLibs/armeabi-v7a
SDLJ=$ROOT/android/app/src/sdl/java
mkdir -p "$JNI" "$SDLJ" build/deps
[[ -d build/deps/SDL ]] || git clone --depth 1 --branch release-3.4.20 https://github.com/libsdl-org/SDL build/deps/SDL
if [[ ! -f build/deps/sdl3-android/lib/libSDL3.so ]]; then
    say "building SDL3 for Android (once)"
    cmake -S build/deps/SDL -B build/deps/SDL/build-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=armeabi-v7a -DANDROID_ARM_MODE=arm -DANDROID_PLATFORM=android-$API \
        -DCMAKE_INSTALL_PREFIX="$ROOT/build/deps/sdl3-android" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build build/deps/SDL/build-android
    cmake --install build/deps/SDL/build-android
fi
cp build/deps/sdl3-android/lib/libSDL3.so "$JNI/"
rm -rf "$SDLJ/org"; mkdir -p "$SDLJ/org/libsdl"
cp -r build/deps/SDL/android-project/app/src/main/java/org/libsdl/app "$SDLJ/org/libsdl/"

if [[ $CHECK == 1 ]]; then
    say "check: compiling the native C code for ARM"
    mkdir -p build/decomp-android/shared
    rm -rf build/decomp-android/shared/include && cp -r decomp/include build/decomp-android/shared/include
    ALBW_ANDROID_TARGET=$TARGET ALBW_SDL_INC="$ROOT/build/deps/sdl3-android/include" ALBW_COMPILE_ONLY=1 \
        tools/link_android.sh
    say "check: packaging the APK shell (no game library)"
    export ANDROID_HOME=$SDK
    (cd android && ./gradlew --no-daemon -q assembleDebug)
    say "check passed"
    exit 0
fi

# ---------------------------------------------------------------- 2. game code from your ROM
say "reading the game code from your ROM"
python3 tools/extract_code.py "$ROM" -o build/rom/eu_code.bin
mkdir -p "$CFG"
printf '%s\n' "$ROM" > "$CFG/rom_path"

# ---------------------------------------------------------------- 3. compile + link (ARM)
say "compiling the decompiled game code for ARM ($JOBS jobs; the first build takes a while)"
ALBW_ANDROID_TARGET=$TARGET ALBW_CODE_BIN=build/rom/eu_code.bin \
    python3 tools/build_decomp.py --snapshot decomp --arch android-arm --jobs "$JOBS"
say "linking libmain.so"
ALBW_ANDROID_TARGET=$TARGET ALBW_CODE_BIN=build/rom/eu_code.bin \
ALBW_SDL_INC="$ROOT/build/deps/sdl3-android/include" ALBW_SDL_LIB="$ROOT/build/deps/sdl3-android/lib" \
ALBW_SYSLIBS="$SYSROOT_LIB/libc.so:$SYSROOT_LIB/libm.so:$SYSROOT_LIB/libdl.so:$SYSROOT_LIB/libz.so:$SYSROOT_LIB/liblog.so:$SYSROOT_LIB/libandroid.so:$ROOT/build/deps/sdl3-android/lib/libSDL3.so" \
    tools/link_android.sh
cp build/android/libmain.so "$JNI/"

# ---------------------------------------------------------------- 4. APK
say "packaging the APK"
export ANDROID_HOME=$SDK
(cd android && ./gradlew --no-daemon -q assembleDebug)
cp android/app/build/outputs/apk/debug/app-debug.apk build/android/albw-android.apk
say "done: build/android/albw-android.apk"
if [[ $INSTALL == 1 ]]; then
    ADB=$(command -v adb || echo "$SDK/platform-tools/adb")
    "$ADB" install -r build/android/albw-android.apk
fi
echo "Install it (adb install -r build/android/albw-android.apk), open the app, choose your ROM, Play.
In game: Start = the bottom screen (map / items), Back or L3+R3 = the menu (single-screen, 16:9 and
the internal resolution are under Video). Log of the last run: Android/data/io.github.wylew.albw/files/albw.log"
