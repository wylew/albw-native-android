package org.albw.port;

import org.libsdl.app.SDLActivity;

public class AlbwActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
