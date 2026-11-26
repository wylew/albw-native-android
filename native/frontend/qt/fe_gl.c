/* Qt frontend GL module: the gl.h loader symbols (the game and the test content call gl*()
 * through them, exactly as with the SDL backend), the screen render targets and the compositor.
 * gl_load / gl_program / gl_enable_debug are the same as in native/platform/platform.c, which the
 * Qt build does not link. */
#include "fe_gl.h"
#include "../../platform/gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hidden: gl.h names its function pointers like the GL functions themselves (glGetString...).
 * Exported from the executable they would interpose the real GL symbols that Qt's own GL code
 * (EGL/GLX platform plugins, RHI) calls by name -> crash. Hidden, they stay private to the
 * binary while all of its code (game, frontend) still uses them through gl.h. */
#define GL_DEFINE(ret, name, args) __attribute__((visibility("hidden"))) ret (*name) args;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

int gl_load(void *(*get_proc)(const char *)) {
    int missing = 0;
#define GL_LOAD(ret, name, args) \
    name = (ret(*) args)get_proc(#name); \
    if (!name) { fprintf(stderr, "GL: missing %s\n", #name); missing++; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return missing;
}

typedef void (*GLDebugProc)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
static void debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei len, const GLchar *msg,
                     const void *user) {
    (void)source, (void)id, (void)len, (void)user;
    if (severity == 0x826B) return;  /* GL_DEBUG_SEVERITY_NOTIFICATION */
    fprintf(stderr, "GL debug [type %#x sev %#x]: %s\n", type, severity, msg);
}

void gl_enable_debug(void *(*get_proc)(const char *)) {
    void (*cb)(GLDebugProc, const void *) = (void (*)(GLDebugProc, const void *))get_proc("glDebugMessageCallback");
    if (!cb) {
        fprintf(stderr, "GL: KHR_debug not available, ALBW_GL_DEBUG ignored\n");
        return;
    }
    glEnable(0x92E0);  /* GL_DEBUG_OUTPUT */
    glEnable(0x8242);  /* GL_DEBUG_OUTPUT_SYNCHRONOUS */
    cb(debug_cb, NULL);
    fprintf(stderr, "GL: debug output enabled\n");
}

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
        abort();
    }
    return s;
}

GLuint gl_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program link failed:\n%s\n", log);
        abort();
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* ------------------------------------------------------------------ extra entry points */
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_SAMPLER_BINDING 0x8919
#define GL_VIEWPORT 0x0BA2
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_STENCIL_TEST 0x0B90
#define GL_FRAMEBUFFER_SRGB 0x8DB9
static GLboolean (*p_glIsEnabled)(GLenum);
static void (*p_glGetFloatv)(GLenum, GLfloat *);
static void (*p_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
static void (*p_glBindSampler)(GLuint, GLuint);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (*p_glDeleteRenderbuffers)(GLsizei, const GLuint *);

/* ------------------------------------------------------------------ targets + compositor */
typedef struct { GLuint fbo, color, depth; int w, h; } Target;
static Target screens[2];
static GLuint blit_prog, blit_vao;
static GLint blit_rect_loc;
static int g_ready, g_scale = 1, g_cur = -1;
static char g_renderer[256];

static void target_free(Target *t) {
    if (t->fbo) p_glDeleteFramebuffers(1, &t->fbo);
    if (t->depth) p_glDeleteRenderbuffers(1, &t->depth);
    if (t->color) glDeleteTextures(1, &t->color);
    memset(t, 0, sizeof *t);
}

static void target_init(Target *t, int w, int h) {
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->color);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "screen render target incomplete\n");
        abort();
    }
    /* start black, not undefined */
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void targets_create(int scale) {
    target_free(&screens[0]);
    target_free(&screens[1]);
    target_init(&screens[0], LAYOUT_TOP_W * scale, LAYOUT_SCREEN_H * scale);
    target_init(&screens[1], LAYOUT_BOTTOM_GAME_W * scale, LAYOUT_SCREEN_H * scale);
    g_scale = scale;
}

static const char *BLIT_VS =
    "#version 330 core\n"
    "uniform vec4 rect; out vec2 uv;\n"
    "void main(){ vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); uv = p;\n"
    "  gl_Position = vec4(rect.xy + p * rect.zw, 0, 1); }\n";
static const char *BLIT_FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o; uniform sampler2D tex;\n"
    "void main(){ o = vec4(texture(tex, uv).rgb, 1.0); }\n";

int fegl_init(void *(*get_proc)(const char *), int render_scale, int debug) {
    if (g_ready) return 0;
    if (gl_load(get_proc)) return -1;
    p_glIsEnabled = (GLboolean (*)(GLenum))get_proc("glIsEnabled");
    p_glGetFloatv = (void (*)(GLenum, GLfloat *))get_proc("glGetFloatv");
    p_glColorMask = (void (*)(GLboolean, GLboolean, GLboolean, GLboolean))get_proc("glColorMask");
    p_glBindSampler = (void (*)(GLuint, GLuint))get_proc("glBindSampler");
    p_glReadPixels = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))get_proc("glReadPixels");
    p_glDeleteFramebuffers = (void (*)(GLsizei, const GLuint *))get_proc("glDeleteFramebuffers");
    p_glDeleteRenderbuffers = (void (*)(GLsizei, const GLuint *))get_proc("glDeleteRenderbuffers");
    if (!p_glIsEnabled || !p_glGetFloatv || !p_glColorMask || !p_glBindSampler || !p_glReadPixels ||
        !p_glDeleteFramebuffers || !p_glDeleteRenderbuffers) {
        fprintf(stderr, "GL: missing core 3.3 entry points\n");
        return -1;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    snprintf(g_renderer, sizeof g_renderer, "%s | %s | %s (context %d.%d)", (const char *)glGetString(GL_VENDOR),
             (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), major, minor);
    fprintf(stderr, "GL: %s\n", g_renderer);
    if (major * 10 + minor < 33) {
        fprintf(stderr, "OpenGL %d.%d is too old: 3.3 is required.\n", major, minor);
        return -1;
    }
    if (debug) gl_enable_debug(get_proc);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    targets_create(render_scale < 1 ? 1 : render_scale > 8 ? 8 : render_scale);
    blit_prog = gl_program(BLIT_VS, BLIT_FS);
    blit_rect_loc = glGetUniformLocation(blit_prog, "rect");
    glGenVertexArrays(1, &blit_vao);
    g_ready = 1;
    return 0;
}

int fegl_ready(void) { return g_ready; }
int fegl_render_scale(void) { return g_scale; }
const char *fegl_renderer_string(void) { return g_renderer; }

void fegl_set_render_scale(int s) {
    if (!g_ready) return;
    s = s < 1 ? 1 : s > 8 ? 8 : s;
    if (s == g_scale) return;
    targets_create(s);
    if (g_cur >= 0) fegl_rebind();
}

void fegl_screen_size(int s, int *w, int *h) {
    *w = screens[s].w;
    *h = screens[s].h;
}

void fegl_begin_screen(int s) {
    g_cur = s;
    glBindFramebuffer(GL_FRAMEBUFFER, screens[s].fbo);
    glViewport(0, 0, screens[s].w, screens[s].h);
}

void fegl_rebind(void) {
    if (g_cur >= 0) fegl_begin_screen(g_cur);
}

unsigned fegl_new_vao(void) {
    GLuint v = 0;
    if (g_ready) glGenVertexArrays(1, &v);
    return v;
}

void fegl_composite(unsigned fbo, const Layout *l, int linear) { fegl_composite_vao(fbo, blit_vao, l, linear); }

void fegl_composite_vao(unsigned fbo, unsigned own_vao, const Layout *l, int linear) {
    if (!g_ready || !own_vao) return;
    /* save */
    GLint prog, vao, active, tex, sampler, vp[4], mask[4];
    GLfloat cc[4];
    static const GLenum caps[] = {GL_DEPTH_TEST, GL_BLEND, GL_SCISSOR_TEST, GL_CULL_FACE, GL_STENCIL_TEST,
                                  GL_FRAMEBUFFER_SRGB};
    GLboolean en[sizeof caps / sizeof caps[0]];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_COLOR_WRITEMASK, mask);
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        en[i] = p_glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, l->ww, l->wh);
    p_glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindSampler(0, 0);
    glUseProgram(blit_prog);
    glBindVertexArray(own_vao);
    GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    for (int k = 0; k < 2; k++) {
        int s = l->order[k];
        if (!l->visible[s] || l->r[s].w <= 0 || l->r[s].h <= 0) continue;
        float ndc[4];
        layout_rect_ndc(l, s, ndc);
        glBindTexture(GL_TEXTURE_2D, screens[s].color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glUniform4fv(blit_rect_loc, 1, ndc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* restore */
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        if (en[i]) glEnable(caps[i]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    p_glColorMask((GLboolean)mask[0], (GLboolean)mask[1], (GLboolean)mask[2], (GLboolean)mask[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    p_glBindSampler(0, (GLuint)sampler);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
}

void fegl_read_screen(int s, uint8_t *rgba) {
    Target *t = &screens[s];
    size_t row = (size_t)t->w * 4;
    uint8_t *tmp = malloc(row * t->h);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    p_glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < t->h; y++) memcpy(rgba + (size_t)y * row, tmp + (size_t)(t->h - 1 - y) * row, row);
    for (size_t i = 0; i < (size_t)t->w * t->h; i++) rgba[i * 4 + 3] = 255;
    free(tmp);
    fegl_rebind();
}
