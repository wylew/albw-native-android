package io.github.wylew.albw;

import org.libsdl.app.SDLActivity;

/** The game: SDL loads libSDL3.so, then libmain.so, and calls its SDL_main (native/runtime/entry.c).
 *  Runs in its own process (":game", AndroidManifest.xml): the native game keeps global state that
 *  only a fresh process resets, and it ends the process when it quits. */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
