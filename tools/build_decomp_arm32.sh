#!/usr/bin/env bash
# Compile the decompiled game code for Android armeabi-v7a (README: Android port, step 3).
#   tools/build_decomp_arm32.sh [--jobs N]     -> build/decomp-arm32 (report.json: failures)
# Needs build/rom/eu_code.bin (tools/extract_code.py <your EU rom>) and the Android NDK.
set -euo pipefail
cd "$(dirname "$0")/.."
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
NDK=${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[[ -x $BIN/clang ]] || { echo "Android NDK not found (set ANDROID_NDK_HOME)"; exit 1; }
[[ -f build/rom/eu_code.bin ]] || { echo "build/rom/eu_code.bin missing: python3 tools/extract_code.py <rom>"; exit 1; }
export PATH=$BIN:$PATH
echo "NDK: $NDK ($(clang --version | head -1))"
ALBW_CODE_BIN=build/rom/eu_code.bin exec python3 tools/build_decomp.py --snapshot decomp --arch arm32 "$@"
