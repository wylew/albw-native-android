#!/usr/bin/env bash
# Fetch the sources the Android build needs into build/deps (not committed).
# SDL: the same release build.sh uses for the PC build.
set -euo pipefail
cd "$(dirname "$0")/.."
SDL_TAG=release-3.4.20
if [[ ! -d build/deps/SDL ]]; then
    git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL build/deps/SDL
fi
echo "SDL: $(git -C build/deps/SDL describe --tags 2>/dev/null || echo present)"
