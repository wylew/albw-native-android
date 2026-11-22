# Android port notes

Build: `../build_android.sh /path/to/rom.3ds` (see the main README). `../build_android.sh --check`
builds everything that does not need the ROM (CI does this on every push).

## How it fits together

- `tools/build_decomp.py --arch android-arm` compiles the decompiled game for `armv7a-linux-androideabi24`:
  ARM (not Thumb), `-fPIC`, softfp (float arguments and results in core registers, so the int/float
  prototype mismatches in Ghidra code are harmless on this ABI).
- `tools/link_android.sh` links `libmain.so`: the game data from the ROM in writable sections (it holds
  relocated pointers; Android refuses text relocations), ARM returns-this thunks, no bool / x87 adapters.
- The renderer runs on OpenGL ES 3.2 (`ALBW_GLES`, `native/platform/gl.h`); every shader starts with
  `GLSL_HDR`.
- `GameActivity` is SDL's `SDLActivity` in its own process (`:game`). `native/runtime/entry.c` sets the
  data folder, the ROM path and the log, and runs the game on its own thread with a 16 MiB stack.
- Defaults on Android (`native/platform/layout.c`): single-screen, 16:9, internal resolution x3,
  full-screen session. All of them can be changed in the menu (Video) and are saved in `config/`.

## Known differences / open items

- Save states only load in the run that saved them (a shared library cannot have the PC build's fixed
  data addresses). Normal game saves are unaffected.
- No logic ops in OpenGL ES: PICA passes that use them are drawn as a plain copy; texture LOD bias is
  ignored.
- VRAM may not get its 3DS address (0x1F000000) when the Java heap sits there; the game's own VRAM
  range checks then miss (the log says "relocated").
- Pausing the app (Home) is not handled specially yet: if the GL context is lost on resume, restart
  the game.
- No on-screen controls: a gamepad (built in or Bluetooth) is needed.
