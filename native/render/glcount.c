/* Android debug aid: what the renderer asks of the GPU per frame (README: Android performance).
 *
 *   ALBW_GL_COUNT=1   (albw.env) every 2 s to the log: per presented frame, render-target switches,
 *                     blits, clears, glReadPixels, texture uploads (count + KiB), buffer uploads (KiB),
 *                     draws, program switches.
 *
 * android_compat.h maps SDL_GL_GetProcAddress to albw_gl_getproc for the port's own code; with
 * ALBW_GL_COUNT unset this hands out the real functions (no overhead after start-up). A tile-based
 * GPU (Adreno) pays for every switch of render target, blit and partial clear with a write-out and
 * reload of its tile memory, which scales with the resolution. */
#if defined(__ANDROID__)
#undef SDL_GL_GetProcAddress   /* this file wraps the real one */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef intptr_t GLintptr, GLsizeiptr;

static struct {
    unsigned long fbo_switch, blit, clear, readpix, tex_up, draw, prog_switch;
    unsigned long long tex_bytes, buf_bytes;
} C;
static GLuint g_cur_fbo[2] = {~0u, ~0u}, g_cur_prog = ~0u;

#define REAL(name, type) static __typeof__(type) real_##name
REAL(glBindFramebuffer, void (*)(GLenum, GLuint));
REAL(glBlitFramebuffer, void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum));
REAL(glClear, void (*)(GLbitfield));
REAL(glClearBufferfv, void (*)(GLenum, GLint, const float *));
REAL(glClearBufferfi, void (*)(GLenum, GLint, float, GLint));
REAL(glReadPixels, void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
REAL(glTexImage2D, void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
REAL(glTexSubImage2D, void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *));
REAL(glDrawElements, void (*)(GLenum, GLsizei, GLenum, const void *));
REAL(glDrawArrays, void (*)(GLenum, GLint, GLsizei));
REAL(glDrawElementsBaseVertex, void (*)(GLenum, GLsizei, GLenum, const void *, GLint));
REAL(glUseProgram, void (*)(GLuint));
REAL(glBufferData, void (*)(GLenum, GLsizeiptr, const void *, GLenum));
REAL(glBufferSubData, void (*)(GLenum, GLintptr, GLsizeiptr, const void *));

static void w_glBindFramebuffer(GLenum t, GLuint f) {
    int k = t == 0x8CA8 /* READ */ ? 1 : 0;
    if (t == 0x8D40 /* GL_FRAMEBUFFER */) { if (g_cur_fbo[0] != f) C.fbo_switch++; g_cur_fbo[0] = g_cur_fbo[1] = f; }
    else { if (k == 0 && g_cur_fbo[0] != f) C.fbo_switch++; g_cur_fbo[k] = f; }
    real_glBindFramebuffer(t, f);
}
static void w_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl) {
    C.blit++; real_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}
static void w_glClear(GLbitfield m) { C.clear++; real_glClear(m); }
static void w_glClearBufferfv(GLenum b, GLint d, const float *v) { C.clear++; real_glClearBufferfv(b, d, v); }
static void w_glClearBufferfi(GLenum b, GLint d, float x, GLint s) { C.clear++; real_glClearBufferfi(b, d, x, s); }
static void w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p) { C.readpix++; real_glReadPixels(x, y, w, h, f, t, p); }
static void w_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p) {
    if (p) { C.tex_up++; C.tex_bytes += (unsigned long long)w * h * 4; }
    real_glTexImage2D(t, l, i, w, h, b, f, ty, p);
}
static void w_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p) {
    C.tex_up++; C.tex_bytes += (unsigned long long)w * h * 4; real_glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
}
static void w_glDrawElements(GLenum m, GLsizei n, GLenum t, const void *i) { C.draw++; real_glDrawElements(m, n, t, i); }
static void w_glDrawArrays(GLenum m, GLint f, GLsizei n) { C.draw++; real_glDrawArrays(m, f, n); }
static void w_glDrawElementsBaseVertex(GLenum m, GLsizei n, GLenum t, const void *i, GLint b) { C.draw++; real_glDrawElementsBaseVertex(m, n, t, i, b); }
static void w_glUseProgram(GLuint p) { if (p != g_cur_prog) C.prog_switch++; g_cur_prog = p; real_glUseProgram(p); }
static void w_glBufferData(GLenum t, GLsizeiptr n, const void *d, GLenum u) { if (d) C.buf_bytes += (unsigned long long)n; real_glBufferData(t, n, d, u); }
static void w_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void *d) { C.buf_bytes += (unsigned long long)n; real_glBufferSubData(t, o, n, d); }

static int enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ALBW_GL_COUNT"); on = e && *e && strcmp(e, "0"); }
    return on;
}

SDL_FunctionPointer albw_gl_getproc(const char *name) {
    SDL_FunctionPointer real = SDL_GL_GetProcAddress(name);
    if (!real || !enabled()) return real;
#define WRAP(n) if (!strcmp(name, #n)) { real_##n = (void *)real; return (SDL_FunctionPointer)w_##n; }
    WRAP(glBindFramebuffer) WRAP(glBlitFramebuffer) WRAP(glClear) WRAP(glClearBufferfv) WRAP(glClearBufferfi)
    WRAP(glReadPixels) WRAP(glTexImage2D) WRAP(glTexSubImage2D) WRAP(glDrawElements) WRAP(glDrawArrays)
    WRAP(glDrawElementsBaseVertex) WRAP(glUseProgram) WRAP(glBufferData) WRAP(glBufferSubData)
#undef WRAP
    return real;
}

/* called once per presented frame (platform.c, next to the fps log) */
void albw_gl_count_frame(void) {
    static unsigned frames;
    static uint64_t t0;
    if (!enabled()) return;
    frames++;
    uint64_t t = SDL_GetTicksNS();
    if (!t0) t0 = t;
    if (t - t0 < 2000000000ull) return;
    double f = frames ? (double)frames : 1;
    fprintf(stderr, "glcount: per frame: fbo switches %.1f blits %.1f clears %.1f readpix %.1f tex uploads %.1f (%.0f KiB) "
            "buffer %.0f KiB draws %.1f program switches %.1f\n",
            C.fbo_switch / f, C.blit / f, C.clear / f, C.readpix / f, C.tex_up / f, C.tex_bytes / 1024.0 / f,
            C.buf_bytes / 1024.0 / f, C.draw / f, C.prog_switch / f);
    memset(&C, 0, sizeof C);
    frames = 0;
    t0 = t;
}
#else
void albw_gl_count_frame(void) {}
#endif
