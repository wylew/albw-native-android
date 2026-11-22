# A Link Between Worlds – native PC port

A native build of *The Legend of Zelda: A Link Between Worlds* (Nintendo 3DS) for Linux, made from
decompiled game code (based on [albw-decomp-ai](https://github.com/7akeem0/albw-decomp-ai): see
[Credits](#credits)). The game logic runs as real C on your PC; graphics, sound, input and files
are rewritten with SDL3 + OpenGL. No emulator.

<p align="center"><img src="docs/screenshots/title.png" alt="Title screen running in the port (top and bottom screen)" width="400"></p>

**Work in progress.** Playable through large parts of the story; expect bugs. See [Status](#status).

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

### Android (work in progress: 32-bit ARM)

This fork adds an Android build for devices that still run 32-bit apps (Retroid Pocket 5 and other
Snapdragon 865-class handhelds). Phones with 64-bit-only CPUs (Snapdragon 8 Elite, Pixel 7 and
newer) need the 64-bit build, which is not ready yet (native/64BIT.md).

Build it on a Linux PC with the Android SDK, NDK r28 or newer and your ROM:

```
./build_android.sh /path/to/your/game.3ds        # -> build/android/albw-android.apk
adb install -r build/android/albw-android.apk
```

On the device, open the app, **Choose ROM** (it is copied to the app's folder,
`Android/data/io.github.wylew.albw/files/rom.3ds`), then **Play**. It starts in single-screen mode
with a 16:9 top screen: **Start** shows the bottom screen (map, Gear, Items; touch works there),
**Back** or **L3+R3** opens the menu. The log of the last run is `albw.log` in the same folder.
The APK contains code built from your ROM: keep it to yourself.

Not tested on hardware yet; see `android/README.md` for what is known to differ from the PC build.

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
