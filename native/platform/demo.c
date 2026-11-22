/* Platform-layer test: a spinning triangle on each screen, coloured by input.
 * Not game code; checks window, GL context, both render targets, input and pacing. */
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"

static const char *VS =
    GLSL_HDR
    "layout(location=0) in vec2 p; uniform float t; uniform float aspect;\n"
    "void main(){ float c=cos(t), s=sin(t); vec2 q=vec2(c*p.x-s*p.y, s*p.x+c*p.y);\n"
    "  gl_Position=vec4(q.x/aspect, q.y, 0, 1); }\n";
static const char *FS =
    GLSL_HDR
    "uniform vec4 col; out vec4 o; void main(){ o=col; }\n";

int main(int argc, char **argv) {
    PlatConfig cfg = {2, 2, 1};
    int frames = argc > 1 ? atoi(argv[1]) : 0;  /* run N frames then exit (0 = until closed) */
    if (plat_init("ALBW native: platform test", &cfg)) return 1;
    GLuint prog = gl_program(VS, FS), vao, vbo;
    float tri[] = {0, 0.7f, -0.6f, -0.5f, 0.6f, -0.5f};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, 0);
    glEnableVertexAttribArray(0);
    uint64_t t0 = plat_ticks_ns();
    int n = 0;
    while (plat_poll() && (!frames || n < frames)) {
        const PlatInput *in = plat_input();
        float t = (plat_ticks_ns() - t0) / 1e9f;
        for (int s = 0; s < 2; s++) {
            int w, h;
            plat_begin_screen(s);
            plat_screen_size(s, &w, &h);
            glClearColor(s ? 0.1f : 0.05f, 0.15f, s ? 0.1f : 0.25f, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUseProgram(prog);
            glUniform1f(glGetUniformLocation(prog, "t"), s ? -t : t);
            glUniform1f(glGetUniformLocation(prog, "aspect"), (float)w / h);
            float col[4] = {in->held & KEY_A ? 1.f : 0.9f, in->held & KEY_B ? 1.f : 0.6f,
                            in->touch && s ? 1.f : 0.2f, 1};
            glUniform4fv(glGetUniformLocation(prog, "col"), 1, col);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (in->down) fprintf(stderr, "keys %08x  cpad %d,%d  touch %d (%d,%d)\n", in->held, in->cpad_x,
                              in->cpad_y, in->touch, in->touch_x, in->touch_y);
        plat_end_frame();
        n++;
    }
    double secs = (plat_ticks_ns() - t0) / 1e9;
    fprintf(stderr, "%d frames in %.2f s = %.1f fps\n", n, secs, n / secs);
    plat_shutdown();
    return 0;
}
