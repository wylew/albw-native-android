# A Link Between Worlds – native PC port

A native build of *The Legend of Zelda: A Link Between Worlds* (Nintendo 3DS) for Linux, made from
decompiled game code (based on [albw-decomp-ai](https://github.com/7akeem0/albw-decomp-ai): see
[Credits](#credits)). The game logic runs as real C on your PC; graphics, sound, input and files
are rewritten with SDL3 + OpenGL. No emulator.

<p align="center"><img src="docs/screenshots/title.png" alt="Title screen running in the port (top and bottom screen)" width="400"></p>

**Work in progress.** Playable through large parts of the story; expect bugs. See [Status](#status).

**Android:** this fork is working on an Android build. It doesn't run on Android yet. Plan and
progress: [Android port](#android-port-this-fork-in-progress).

## You need your own game

No game data is included: no ROM, keys, assets or saves. You need a **decrypted dump of the
European release** that you made yourself:

```
0487 - Legend of Zelda, The - A Link Between Worlds (Europe) (En,Fr,De,Es,It) Decrypted.3ds
```

| | |
|---|---|
| Size | 748,580,864 bytes |
| SHA-1 | `4d2da91fa0edef9eef07e471527461396cc3faf2` |
| `.code` SHA-1 (what the build checks) | `2aeee593a695f2ce1196b844026567f227472f0b` |

The file name itself does not matter (a renamed file works); the contents must match. The USA and
Japanese releases do not work: the decompiled code is tied to the European release's addresses.

## Play

### Linux: download (easiest)

1. Download `ALBW-native-x86_64.AppImage` from the [latest release](https://github.com/spacestate1/albw-native/releases/latest).
2. `chmod +x ALBW-native-x86_64.AppImage`
3. `./ALBW-native-x86_64.AppImage --rom /path/to/your/game.3ds` (the path is remembered; started
   from a file manager, it asks with a file dialog).

Runs on Ubuntu 22.04 / 24.04, Debian 12+, Fedora, Arch, SteamOS and similar (x86-64). It needs the
system's 32-bit C library and OpenGL driver; if they are missing, it tells you what to install.
No FUSE? Add `--appimage-extract-and-run`.

### Linux: build from source

```
git clone https://github.com/spacestate1/albw-native.git
cd albw-native
./build.sh /path/to/your/game.3ds
./run.sh
```

`build.sh` installs what it needs (asks for sudo), reads the game code from your ROM and compiles
(the first build takes a while: about 26,000 files). Fedora, Ubuntu, Mint, Debian, Arch (enable
`[multilib]`), openSUSE; it needs clang 19 or newer (installed automatically where the distribution
has it; Ubuntu 22.04 / Mint 21: get one from [apt.llvm.org](https://apt.llvm.org)). Rebuild later
with `./build.sh`.

### Steam Deck

In desktop mode, `./build.sh /path/to/game.3ds` builds inside distrobox and makes
`build/ALBW-native-x86_64.AppImage` (or copy the release AppImage over). Add it to Steam as a
non-Steam game to play in Game Mode. L3+R3 opens the game's menu.

### Windows (experimental)

`windows/build_windows.sh`, run on Linux after a normal build, makes `build/ALBW-windows.zip`:
unzip it on Windows, start `ALBW.exe` and choose your `.3ds`. It reaches the title and file select
under Wine but is not tested on real Windows hardware yet. How it works: [windows/README.md](windows/README.md).

## Controls

Keyboard (rebind anything in Esc > Controls); controllers work too, with the buttons where they
are on a 3DS.

| 3DS | Key | | 3DS | Key |
|---|---|---|---|---|
| Circle pad | arrow keys | | L / R | Q / W |
| A | X | | Start | Enter |
| B | Z | | Select | Backspace |
| X | S | | D-pad | I J K L |
| Y | A | | Touch screen | mouse on the bottom screen |

| Key | Action |
|---|---|
| Esc (controller: L3+R3 or Guide) | menu: window, layout, controls, save states, reset game, quit |
| F5 | one window or two windows |
| F6 / F7 | save state / load state |
| F9 | single-screen mode on / off |
| F10 | widescreen top screen (a wider view, not stretched) |
| F12 | screenshot |

### Single-screen mode

One window shows one screen at a time – handy on a laptop, TV or the Steam Deck. Turn it on with
**F9** (or Esc > Video).

- The top screen (the game) is shown; **Tab** (controller: right-stick click) flips to the bottom
  screen and back.
- **Start** brings up the bottom screen (map, Gear, Items); **Select** opens the game's pause menu.
- On the bottom screen the **D-pad** moves a selector between buttons and items and **A** taps the
  selected one – no mouse needed.
- At the file select the save list (bottom screen) is shown by itself; the game switches back to
  the top screen when you start playing.

Saves and save states live in `~/.local/share/albw-port/`, settings in `~/.config/albw-port/`.

**Portable mode:** create a folder named `albw-data` next to the AppImage (or the program) and
everything – saves, save states, settings, screenshots, logs – is kept in it. Or start with
`ALBW_DATA_DIR=/any/folder`.

**Laptops with NVIDIA + Intel / AMD graphics:** the game picks the NVIDIA GPU by itself (it needs the
32-bit NVIDIA OpenGL library: Arch `lib32-nvidia-utils`, Ubuntu / Debian `libnvidia-gl-<version>:i386`).
`ALBW_GPU=integrated` keeps the built-in GPU.

## Save sets (speed runs)

Keep several sets of save files and switch between them in the game. Make a folder `save-sets` next
to the game's `save` folder (`~/.local/share/albw-port/save-sets`) and put one folder per set in it
(for example `Hyrule`, `TurtleLCastle`), each holding `Save_0.bin` to `Save_2.bin` and
`SaveCommon.bin`. In the game press Esc > **File** > **Save set** to pick one, then **Load this save
set** (press it twice). The current save files are backed up to `save-backup/<time>/` (only when they
differ from the set), the set is copied in and the game restarts, so every attempt starts from the
same files.

## Reporting a problem

Press Esc in the game, then **Help > Copy log to clipboard** and paste it (Ctrl+V) into a new
[issue](https://github.com/spacestate1/albw-native/issues/new). If the game stops with an error
before a window opens, the AppImage shows the end of the log in a dialog (and copies it where
`wl-copy` / `xclip` exist); on Windows the log is copied to the clipboard by itself. The log file:
`~/.local/state/albw-port/last-run.log` (AppImage, when not started from a terminal) or
`albw.log` (Windows). In Steam Game Mode, read the file from Desktop Mode. The log has no ROM data.

## Android port (this fork, in progress)

**Does it run on Android yet? It reaches the title screen.** The APK boots the game on a Retroid Pocket 5
to the title screen (3D flyover, logo, "Press A"). Gameplay hasn't been tried yet. This section is the plan and the progress log. Update it with every step forward so
the work can be picked up on any machine.

Target device for testing: **Retroid Pocket 5** (Snapdragon 865 / Adreno 650, Android 13 / API 33,
8 GB RAM, 1920x1080). It runs both `arm64-v8a` and `armeabi-v7a` apps.

### Approach

The playable PC build is a **32-bit** program (i386). The x86-64 build (`native/64BIT.md`) only
boots as far as the title-screen logic. It also depends on `MAP_32BIT` and a non-PIE image at
0x400000, which arm64 Android doesn't offer. So:

1. **First target: `armeabi-v7a` (32-bit ARM).** This reuses the playable 32-bit code base
   unchanged. Pointers are still 4 bytes, and the decompiled code came from 32-bit ARM, so the
   original ABI fits better than i386 does. It runs on the RP5 and most devices up to about 2022.
   Snapdragon 8 Gen 2 and newer phones, plus Pixel 7 and newer, can't run 32-bit apps.
2. **Later: `arm64-v8a`.** This needs the 64-bit build at parity with 32-bit first. It also needs
   low-memory reservations without `MAP_32BIT` (mmap hints / `MAP_FIXED_NOREPLACE` below 2 GiB),
   a check that clang's `__ptr32` (`ALBW_GP`) works on AArch64, and the loader from step 4.

### Constraints (read before starting)

- **The APK contains ROM-derived code.** `build.sh` compiles the game code it extracts from your
  ROM (`tools/extract_code.py`) into the binary. Build the APK locally from your own ROM and never
  publish it or commit build output. The game also reads RomFS from the `.3ds` file at run time,
  so the ROM has to be on the device too.
- **Fixed addresses.** The 32-bit build expects its image at 0x08048000, game data at 0x30000000
  (`--section-start` in `tools/link.sh`), VRAM at 0x1F000000 and game memory at
  0x40000000-0x5C000000 (`native/sdk/memory.c`). Save states store these pointers. A 32-bit
  Android app process already has ART, the linker and system libraries mapped, so these ranges
  may be taken (ART's heap starts near 0x12C00000).
- **Desktop GL only.** All 15 shaders are `#version 330 core`. The renderer uses `glLogicOp` /
  `GL_COLOR_LOGIC_OP` (PICA logic ops; GLES has none) and `GL_CLAMP_TO_BORDER` /
  `GL_TEXTURE_BORDER_COLOR` (GLES 3.2 has them, Adreno 650 supports 3.2).
- **x86-specific code** (the parts to replace for ARM): `-m32` / `--target=i686-linux-gnu` /
  `-m elf_i386` in `tools/link.sh`; the generated `thunks.s` (returns-this thunks) and
  `boolret.s`; the x87 guard (`runtime/x87guard.c`, `x87pop.s`, `tools/x87check.py`); inline asm
  in `runtime/{glibc_compat,ss_ctx,savestate}.c` and `sdk/{memory,os,boot_extra}.c`; register
  reads in `ucontext` (crash handler, save states); `build_decomp.py --arch` (x86 / x86_64 only;
  the i386 ABI checks).

### Steps

| # | Step | Done when | State |
|---|---|---|---|
| 0 | Plan | this section | **done** 2026-11-22 |
| 1 | Toolchain: install the NDK (r27+) and set up a CMake/Gradle project under `android/` with SDL3's Android template (`SDLActivity`). Gradle comes from the wrapper; Android Studio's bundled JDK works | an empty SDL3 app (clears the screen, reads the gamepad) installs with `./gradlew installDebug` and runs on the RP5 | **done** 2026-11-22: `android/` (AGP 9.1.1, Gradle 9.3.1, NDK r30 30.0.16248370, CMake 4.1.2, SDL 3.4.20, `armeabi-v7a`). The app runs on the RP5 with a GLES 3.2 context, and the gamepad is detected ("Retroid Pocket Controller"). Input checked through SDL's gamepad API: A, B, X, the D-pad, both sticks and both stick clicks. Y, L/R, L2/R2, Start and Select weren't pressed in the test, so check them in step 8. The window reports 1920x972 (system bars still shown: fix in step 8) |
| 2 | Memory map: log `/proc/self/maps` from the step-1 app (32-bit) at `SDL_main` time and check whether the fixed ranges above are free | table of free / taken ranges added here | **done** 2026-11-22, see [Memory map](#memory-map-32-bit-app-process-rp5) |
| 3 | ARM32 decomp build: add `--arch arm32` to `tools/build_decomp.py` (target `armv7a-linux-androideabi24`, NDK clang). Compile all of `build/decomp` for ARM and count failures against the i386 baseline (26,001 compiled / 270 failed) | failure count at or near the baseline | **done** 2026-11-22: `tools/build_decomp_arm32.sh` (= `build_decomp.py --snapshot decomp --arch arm32`, NDK clang 21, `-mfpu=neon -mfloat-abi=softfp -fPIC`) -> `build/decomp-arm32`. **26,237 / 26,237 snapshot sources compiled, 0 failed** (ELF32 ARM, 105 MB of objects). 74,822 names left unresolved, which the link fills from game data, native code and stubs |
| 4 | ARM32 link: `tools/link_android.sh`, mirroring `link.sh`. Port `thunks.s` / `boolret.s` to ARM (on ARM `this` and the return value share r0, so returns-this thunks may be trivial), drop the x87 parts, port the inline asm. Output: either (a) a shared library `libalbw.so` (game data relocated by the dynamic linker; save states go through `runtime/ss_convert.c`), or (b) a non-PIE image at the fixed addresses, loaded by a small loader like `windows/loader.c`. Try (a) first and fall back to (b) if step 2 shows clashes that matter | links with 0 unresolved symbols besides the known tripwire stubs | **done** 2026-11-22: option (a). `tools/link_android.sh` -> `build/android/jniLibs/armeabi-v7a/libmain.so` (29 MB, 16 MB stripped in the APK) + `libSDL3.so`; Gradle packages them when present (`-Psmoke` builds the step-1 test instead). 11 function stubs + 20 data blocks. No text relocations, entry `SDL_main` (`native/runtime/android.c`). Open: save states (`tools/gen_ssmap.py` reads GNU ld maps, not lld's; the `.so` load address changes every run) |
| 5 | Boot on the device: install the APK, push the ROM (`adb push <rom> /sdcard/Android/data/org.albw.port/files/rom.3ds`), reach the title-screen logic (`TaskFileSelect` 0029B698 called every frame, as in `native/BOOT_LOG64.md`) | title logic runs 60 s without a fatal | **done** 2026-11-22: the title screen renders on the RP5 (flyover, logo, "Press A") and runs 60 s+ with no errors. Stops A1-A5 in [Boot stops](#boot-stops-android) |
| 6 | GLES renderer: a `#version 300 es` variant of the shaders (precision qualifiers, no `core`), an SDL GLES 3.2 context, logic ops done in the shader (framebuffer fetch on Adreno, `GL_EXT_shader_framebuffer_fetch`, or a fallback), checks for every desktop-only call | title screen draws correctly on the RP5 | todo |
| 7 | Android platform layer: ROM picked with the system file picker (SAF) and copied to app storage (for development: `adb push` to `/sdcard/Android/data/<package>/files/`); saves / states / config under `SDL_GetPrefPath`; audio through SDL (AAudio); log viewable via `adb logcat` | boots from the APK alone, saves survive a restart | todo |
| 8 | Controls and layout: RP5 buttons through the SDL gamepad API; landscape layout (top screen large, bottom screen beside it, or single-screen mode by default); touch input for the bottom screen; a button combo for the Esc menu | playable start to Eastern Palace on the RP5 | todo |
| 9 | Packaging: `android/build_apk.sh` (ROM path argument, like `build.sh`), release-signed local APK, docs in the Play section | one command makes an installable APK | todo |
| 10 | arm64-v8a (see Approach) | runs on a 64-bit-only device | todo |

### Memory map (32-bit app process, RP5)

From the smoke test's `/proc/self/maps`, with SDL loaded and before GL starts:

| Range the PC build uses | What | On Android |
|---|---|---|
| 0x08048000- | program image | free (ignored if the game is a `.so`) |
| 0x1F000000-0x1F600000 | VRAM | **taken**: ART's Java heap (`dalvik-main space`, 0x12C00000-0x32C00000, 512 MiB, at a fixed address) |
| 0x30000000-0x30300000 | game data | **taken**: same ART heap |
| 0x40000000-0x5C000000 | game memory | free |

Free gaps of 64 MiB or more: 0x00010000-0x07714000 (119 MiB), 0x07719000-0x12C00000 (180 MiB),
**0x32C00000-0x7049C000 (984 MiB)**, 0x74AC0000-0xC375C000 (1,260 MiB), 0xF21B9000-0xFF09F000
(206 MiB). For step 4: move VRAM and game data into 0x32C00000-0x3FFFFFFF on Android (for example
game data at 0x3C000000, VRAM at 0x3E000000) and keep the heaps at 0x40000000. Save states from the
PC go through `runtime/ss_convert.c`. Reserve the ranges as early as possible (in `JNI_OnLoad`).

### Build and run the Android app

```
android/fetch_deps.sh                       # SDL sources into build/deps/SDL
echo "sdk.dir=$HOME/Android/Sdk" > android/local.properties
cd android && ./gradlew installDebug        # needs JDK 17+ (Android Studio's bundled JBR works)
adb shell am start -n org.albw.port/.AlbwActivity
adb logcat -s albw:* SDL:* SDL/APP:*
```
SDK packages: `sdkmanager "ndk;30.0.16248370" "cmake;4.1.2" "platforms;android-35"`. With the
screen locked, the app stops right after it starts. Unlock the device first.

### Boot stops (Android)

Crashes: `adb logcat -b crash` (debuggerd tombstone; the port's own handler is off on Android).
Addresses: `pc - (fault pc - tombstone pc)` = offset in the unstripped
`build/android/jniLibs/armeabi-v7a/libmain.so`, then `llvm-addr2line -f -e <that file> 0x<offset>`.
Game code has no unwind tables, so a tombstone shows one frame: symbolize `lr` too.

| # | Stop | Cause | Fix |
|---|---|---|---|
| A1 | `StackOverflowError` from JNI right at start | the game's main runs on the fixed save-state stack (a stack switch on SDL's Java thread); ART checks every JNI call's stack pointer against the thread's bounds | `runtime/lowmem.c`: on Android a new thread whose stack is that slot; the SDL thread joins it |
| A2 | `SDL_CreateWindow: Could not initialize OpenGL / GLES library` | desktop GL 3.3 core context requested | `platform/platform.c`: GLES 3.2 context; shaders translated to `#version 320 es` + default precisions; `render/render.c` stand-ins for `glDepthRange` (-> `glDepthRangef`), `glLogicOp` (ignored for now), `glBindFragDataLocation` |
| A3 | SIGSEGV in `func_00489CE0` (FileDevice::isExistFile) with `this` = 0xE5, from `native/game/func_001E1EDC.c` | the device index comes from a `char` holding -1; plain `char` is **unsigned on ARM**, signed on i386, so it read as 255 (slot 255 held 1). The converted sources mean signed (Ghidra `char` = `ldrsb`), and the playable PC build compiles them signed | `-fsigned-char` for the decomp build and the native code (`tools/build_decomp.py`, `tools/link_android.sh`) |
| A4 | SIGSEGV in `memcpy` inside `glBufferSubData` after ~20 s (vertex upload, `render/h3d.c` `vtx_upload`) | VRAM couldn't be mapped at 0x1F000000 (ART's heap). The GPU base register held the relocated block, but the game's VRAM allocator (`00103D30`) starts at a hard-coded `0x1f000000`, so buffer addresses were 3DS VRAM addresses | VRAM is a 6 MB block `albw_vram` in `libmain.so` (`sdk/gfx.c`). Hard-coded 3DS VRAM addresses go through `ALBW_VRAM_ADDR()` (`albw_native.h`): Android copies of `00103D30` / `001339A4` in `native/android/` (compiled only by `link_android.sh`) and `compilefix/func_00466004.c`. `link_android.sh` rewrites the VRAM pool words (0x1F300000, 0x1F5FFFFF, ...; 5 words) to `albw_vram + offset` |
| A5 | `h3d: draw dropped: index buffer ... outside game memory` | `render/h3d.c` `index_region_ok` accepted only 0x1F000000-0x1F600000 as VRAM | asks `albw_gfx_vram()` for the real block |

### Working on it from another machine

You need: Linux with clang + lld (as `build.sh` installs), Python 3, the Android SDK + NDK, the
EU ROM (never committed) and a device with USB debugging on (`adb devices` lists it). Check the
table above for the next `todo`. Record what you learned in the step's row and in the log below,
then commit.

### Progress log

| Date | What |
|---|---|
| 2026-11-22 | Plan written. Checked: the RP5 lists `arm64-v8a,armeabi-v7a,armeabi`, Android 13 (API 33). On the dev machine the SDK has build-tools 35/36 and platforms 36/37 but no NDK. No Android code in the repo yet |
| 2026-11-22 | Step 1: installed cmdline-tools, NDK r30, CMake 4.1.2. `android/` Gradle project + SDL3 smoke test (`android/app/src/main/cpp/smoke.c`) runs on the RP5: Adreno 650, OpenGL ES 3.2, GLSL ES 3.20. Extensions useful for step 6: `GL_EXT_shader_framebuffer_fetch` (logic ops in the shader), `GL_EXT_texture_border_clamp`, `GL_EXT_clip_control`, `GL_EXT_texture_format_BGRA8888`. Step 2: ART's heap covers the VRAM and game-data addresses (table above) |
| 2026-11-22 | Step 1 done: gamepad events arrive in SDL (buttons, D-pad, sticks, stick clicks). Next: step 3 (ARM32 decomp build) |
| 2026-11-22 | Step 3 done: the whole decomp snapshot compiles for armeabi-v7a with no failures. Code to port in step 4: `runtime/ss_ctx.c` (save-state contexts, i386/x86-64 asm, `#error` otherwise), `albw_ss_fp_tail` asm in `runtime/savestate.c`, PC/SP reads from `ucontext` in `runtime/crash.c` / `prof.c`, `runtime/x87guard.c` (skip on ARM), `glibc_compat.c` (i386-only already). In `link.sh`: `boolret` (i386 `%al`; ARM callees widen bools themselves, so it can likely be dropped), the returns-this `thunks.s` (rewrite for ARM: keep r0 in r4, copy the stack words, call the impl), the x87 relink (drop), `--section-start` / `-no-pie` (-> `-shared -Wl,-Bsymbolic`). Bionic: `backtrace()` needs API 33 (the RP5 has it; minSdk is 24, so add an `_Unwind_Backtrace` shim), no `execv("/proc/self/exe")` restart in an app |
| 2026-11-22 | Step 4 done: `libmain.so` links. Ported for ARM / bionic: save-state contexts (`runtime/ss_ctx.c`: r4-r11, sp, lr, d8-d15; `AlbwCtx` is 26 words on ARM), the low-stack start without `swapcontext` (`runtime/lowmem.c`), crash / profiler PC (`arm_pc`, caller from `arm_lr`), `backtrace` for API < 33 + stdout/stderr -> logcat + `SDL_main` with paths (`runtime/android.c`, `android_compat.h`), `pthread_tryjoin_np` (`sdk/os.c`), x87 / SSE bits guarded. Float mode on ARM: FPSCR DN only. The 3DS also sets FZ, but ARM's FZ flushes denormal inputs too, which breaks the integer-in-float tests (see `albw_fp_set_3ds_mode`). Game and port are built with `-fno-stack-protector` on Android (32-bit ARM bionic has one process-wide canary, so save states couldn't restore it). The app reads the ROM from `/sdcard/Android/data/org.albw.port/files/rom.3ds` |
| 2026-11-22 | Step 5 started: the APK runs the game on the RP5. Paths and logcat work, the controller is found, a GLES 3.2 context comes up on the Adreno 650, the platform's start-up shaders compile as GLSL ES 3.20 (the renderer's shaders, built later, aren't tested yet), and all 1,230 static constructors run. Then stop A3. VRAM is relocated (0x1F000000 is inside ART's heap), so the game's own VRAM range checks will miss: watch for that. Not checked: whether the PC build still compiles with these changes (no i386 toolchain in this environment; everything new is behind `__ANDROID__` / `__arm__`) |
| 2026-11-22 | Step 5 done: **title screen on the RP5**. The 3D title flyover (trees, castle walls, water), the logo and the bottom-screen "Press A" panel all draw through the GLES 3.2 renderer; audio starts on AAudio; 60 s+ with no errors. Fixes: signed `char` (A3), VRAM inside `libmain.so` with the game's hard-coded VRAM addresses remapped (A4, A5). The renderer's shaders compile as GLSL ES 3.20 without changes beyond the version/precision header. Not tried yet: input past the title screen, logic ops (ignored on GLES), save states. Next: press A (file select, new game), then step 8's controls and layout |
