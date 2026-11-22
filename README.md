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

**Does it run on Android yet? No.** This fork adds an Android build; nothing installs on a
device yet. This section is the plan and the progress log. Update it with every step forward so
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
| 1 | Toolchain: install the NDK (r27+) and set up a CMake/Gradle project under `android/` with SDL3's Android template (`SDLActivity`). Gradle comes from the wrapper; Android Studio's bundled JDK works | an empty SDL3 app (clears the screen, reads the gamepad) installs with `./gradlew installDebug` and runs on the RP5 | **mostly done** 2026-11-22: `android/` (AGP 9.1.1, Gradle 9.3.1, NDK r30 30.0.16248370, CMake 4.1.2, SDL 3.4.20, `armeabi-v7a`). The app runs on the RP5 with a GLES 3.2 context, and the gamepad is detected ("Retroid Pocket Controller"). Button presses not checked yet. The window reports 1920x972 (system bars still shown: fix in step 8) |
| 2 | Memory map: log `/proc/self/maps` from the step-1 app (32-bit) at `SDL_main` time and check whether the fixed ranges above are free | table of free / taken ranges added here | **done** 2026-11-22, see [Memory map](#memory-map-32-bit-app-process-rp5) |
| 3 | ARM32 decomp build: add `--arch arm32` to `tools/build_decomp.py` (target `armv7a-linux-androideabi24`, NDK clang). Compile all of `build/decomp` for ARM and count failures against the i386 baseline (26,001 compiled / 270 failed) | failure count at or near the baseline | todo |
| 4 | ARM32 link: `tools/link_android.sh`, mirroring `link.sh`. Port `thunks.s` / `boolret.s` to ARM (on ARM `this` and the return value share r0, so returns-this thunks may be trivial), drop the x87 parts, port the inline asm. Output: either (a) a shared library `libalbw.so` (game data relocated by the dynamic linker; save states go through `runtime/ss_convert.c`), or (b) a non-PIE image at the fixed addresses, loaded by a small loader like `windows/loader.c`. Try (a) first and fall back to (b) if step 2 shows clashes that matter | links with 0 unresolved symbols besides the known tripwire stubs | todo |
| 5 | Headless boot on the device: push the binary + ROM via adb, run with `SDL_VIDEO_DRIVER=offscreen` (or with the GL draw path off) and reach the title-screen logic (`TaskFileSelect` 0029B698 called every frame, as in `native/BOOT_LOG64.md`) | title logic runs 60 s without a fatal | todo |
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

## Status

Working: boot, title, file select, saving and loading; the story's first areas, the overworld,
houses, items, enemies, combat; the Eastern Palace and its boss (Yuga, wall merging as a
painting); late-game saves load (Death Mountain, Lorule).

Known problems: the Fire / Tornado Rod flame cones are not drawn yet; some effects and cutscenes
are still wrong; most dungeons after the Eastern Palace are untested. Full list:
[MISSING.md](MISSING.md). Please report what you find.

## How it works (short)

- `decomp/` – the decompiled game code, converted so it compiles natively.
- `native/` – written by hand: window, input, audio, the renderer, replacements for the 3DS system
  libraries, and fixes for decompiled functions (`native/compilefix/`, each with a note).
- `windows/` – the Windows loader.
- The game's data is read from your ROM at build time and never stored in the repository.

Notes: [native/BOOT_LOG.md](native/BOOT_LOG.md) (every bug and its fix),
[native/PAINTING.md](native/PAINTING.md) (wall merging step by step),
[native/DIDNT_WORK.md](native/DIDNT_WORK.md) (what was tried and failed).

Rules: no emulation (no CPU interpreter or recompiler, no GPU or OS emulation; 3DS library calls are
replaced function by function), hand-written GLSL shaders, no game data in the repository.

## Credits

Built on **[albw-decomp-ai](https://github.com/7akeem0/albw-decomp-ai)** by
**[7akeem0](https://github.com/7akeem0)** and contributors, the matching decompilation of
*A Link Between Worlds*: its matched sources (about 16,000 functions), address map and headers are
the base of the game code here (`decomp/` is a snapshot of them). Thanks also to
**[matthis](https://github.com/mmaupas/albw-decomp-ai)** for the decomp's automation tools, shared
headers and many matches. Unmatched functions come from [Ghidra](https://github.com/NationalSecurityAgency/ghidra)
output. Windowing, input and audio use [SDL3](https://www.libsdl.org/).

## License

The port's own code (`native/`, `tools/`, `windows/`, scripts) is MIT: see [LICENSE](LICENSE). It
does not cover the game or its data. The decompiled sources in `decomp/` come from albw-decomp-ai
(and Ghidra); that project has no license file yet, so the MIT license does not cover them.

## Legal

A fan project for preservation and study, not affiliated with or endorsed by Nintendo. *The Legend
of Zelda* and *A Link Between Worlds* are trademarks of Nintendo. You must own the game and dump it
yourself; do not ask for ROMs or keys.
