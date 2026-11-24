/* HD texture pack user interface (#included by platform.c after states_ui.c): Esc menu > Textures
 * (HD textures on / off, Import pack, status) and the Android import's progress.
 * Import (Android): AlbwActivity.importTexturePack() opens the system folder picker and copies the
 * pack's tex1_*.png into the app's files/textures/<folder name>/ on a Java thread; its state is
 * polled here once per frame (and in the menu) from static fields; when it finishes the textures
 * folder is scanned again and every 2D texture uploaded again (render/hdtex.c, render.c).
 * Elsewhere Import only tells where to put a pack (the textures folder, hdtex_root()). */
#include "../render/hdtex.h"
#if defined(__ANDROID__)
#include <jni.h>
#endif

static int g_hd_importing;

#if defined(__ANDROID__)
static jclass hd_cls(JNIEnv **env_out) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return NULL;
    jclass c = (*env)->GetObjectClass(env, act);
    (*env)->DeleteLocalRef(env, act);
    *env_out = env;
    return c;
}
static int hd_field(const char *name) {
    JNIEnv *env;
    jclass c = hd_cls(&env);
    if (!c) return 0;
    jfieldID f = (*env)->GetStaticFieldID(env, c, name, "I");
    int v = f ? (*env)->GetStaticIntField(env, c, f) : 0;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); v = 0; }
    (*env)->DeleteLocalRef(env, c);
    return v;
}
static void hd_set_state(int v) {
    JNIEnv *env;
    jclass c = hd_cls(&env);
    if (!c) return;
    jfieldID f = (*env)->GetStaticFieldID(env, c, "sImportState", "I");
    if (f) (*env)->SetStaticIntField(env, c, f, v);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
}
#endif

static void hdui_import(void) {
#if defined(__ANDROID__)
    JNIEnv *env;
    jclass c = hd_cls(&env);
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "importTexturePack", "()V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, c, m);
    if (c && (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); m = NULL; }
    if (c) (*env)->DeleteLocalRef(env, c);
    if (!m) { snprintf(g_menu_note, sizeof g_menu_note, "Import is not available"); return; }
    g_hd_importing = 1;
    snprintf(g_menu_note, sizeof g_menu_note, "Choose the pack's folder (any level above its tex1_*.png files)");
#else
    snprintf(g_menu_note, sizeof g_menu_note, "Copy the pack's tex1_*.png files into %s", hdtex_root());
#endif
}

/* once per frame (plat_end_frame) and in the menu loop: the import's progress and outcome */
static __attribute__((noinline)) void hdui_poll(void) {
#if defined(__ANDROID__)
    if (!g_hd_importing) return;
    int st = hd_field("sImportState");
    char l1[96], l2[96];
    if (st == 2) {
        static uint64_t last;
        uint64_t now = plat_ticks_ns();
        if (now - last > 250000000ull) {
            last = now;
            int done = hd_field("sImportDone"), total = hd_field("sImportTotal");
            snprintf(l1, sizeof l1, "Importing HD textures");
            snprintf(l2, sizeof l2, "%d / %d files", done, total);
            ssui_toast(-1, 1000000000ull, l1, l2);
            snprintf(g_menu_note, sizeof g_menu_note, "Importing: %d / %d files", done, total);
        }
        return;
    }
    if (st == 0 || st == 1) return;   /* picker still open */
    g_hd_importing = 0;
    hd_set_state(0);
    if (st == 3) {
        hdtex_rescan();
        render_hdtex_refresh();
        snprintf(l1, sizeof l1, "HD textures imported");
        snprintf(l2, sizeof l2, "%s", hdtex_status());
    } else {
        snprintf(l1, sizeof l1, "%s", st == -2 ? "Import cancelled" : st == -3 ? "No HD textures in that folder" : "Import failed");
        snprintf(l2, sizeof l2, "%s", st == -3 ? "Pick the folder that holds the tex1_*.png files" : "");
    }
    ssui_toast(-1, 3000000000ull, l1, l2);
    snprintf(g_menu_note, sizeof g_menu_note, "%s%s%s", l1, l2[0] ? ": " : "", l2);
#endif
}

static void hdui_set_enabled(int on) {
    g_layout_cfg.hd_textures = on != 0;
    hdtex_set_enabled(on);
    render_hdtex_refresh();
    plat_layout_changed();
}
