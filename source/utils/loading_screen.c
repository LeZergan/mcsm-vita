/*
 * Loading screen for MCSM Vita loader
 * GLES2-rendered status overlay with 8x8 bitmap font.
 */
#include "utils/loading_screen.h"
#ifdef USE_PVR_PSP2
#include "utils/pvr_init.h"
#endif
#include "utils/logger.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#ifndef USE_PVR_PSP2
/* Forward-declare only the one vitaGL entry we need; including the full vitaGL
 * header here conflicts with the EGL and GLES2 headers above (it re-declares
 * the same egl and gl symbols). */
extern void vglSwapBuffers(GLboolean has_commondialog);
#endif

/* Embedded 8x8 font: 95 glyphs (ASCII 32-126), 8 bytes each = 760 bytes */
static const unsigned char g_font[95*8] = {
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00,
0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00,
0x0C,0x3F,0x68,0x3E,0x0B,0x7E,0x18,0x00,0x60,0x66,0x0C,0x18,0x30,0x66,0x06,0x00,
0x38,0x6C,0x6C,0x38,0x6D,0x66,0x3B,0x00,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,
0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00,
0x00,0x18,0x7E,0x3C,0x7E,0x18,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,
0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,
0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00,0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00,
0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00,0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,
0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00,0x7E,0x06,0x06,0x0C,0x18,0x30,0x30,0x00,
0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00,0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00,
0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x30,
0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,
0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00,0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00,
0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00,
0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00,0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00,
0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00,
0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00,0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00,
0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,
0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00,
0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00,
0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,
0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00,0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00,
0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x00,0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00,
0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,
0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00,
0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00,
0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,
0x00,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,
0x10,0x38,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,
0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3E,0x00,
0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,
0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00,0x3C,0x66,0x7E,0x60,0x66,0x3C,0x00,
0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C,0x00,
0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,
0x0C,0x00,0x0C,0x0C,0x0C,0x4C,0x38,0x00,0x60,0x60,0x6C,0x78,0x78,0x6C,0x66,0x00,
0x18,0x18,0x18,0x18,0x18,0x18,0x0C,0x00,0x00,0x76,0x7F,0x6B,0x6B,0x6B,0x63,0x00,
0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,
0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06,0x00,
0x00,0x7C,0x66,0x60,0x60,0x60,0x60,0x00,0x00,0x3E,0x60,0x3C,0x06,0x66,0x3C,0x00,
0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,
0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x63,0x63,0x6B,0x7F,0x36,0x22,0x00,
0x00,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00,
0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00,
0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00,
0x31,0x6B,0x46,0x00,0x00,0x00,0x00,0x00
};


/* GLES2 shader for textured quads (font rendering) */
static const char *g_vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

/* Inline precision qualifiers on every declaration — the standalone
 * "precision mediump float;" statement was being dropped before the PVR
 * compiler saw it ("No precision defined for vec2/vec4/float"), so qualify
 * each type directly instead. */
static const char *g_fs_src =
    "varying mediump vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform lowp vec4 u_color;\n"
    "void main() {\n"
    "  lowp float a = texture2D(u_tex, v_uv).a;\n"
    "  gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "}\n";

/* Solid color shader (background, progress bar) */
static const char *g_vs_solid =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *g_fs_solid =
    "uniform lowp vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

static GLuint g_prog_text = 0;
static GLuint g_prog_solid = 0;
static GLuint g_font_tex = 0;
static GLuint g_vbo = 0;
static int g_fb_w = 960;
static int g_fb_h = 544;

/* Current status string and progress */
static char  g_status[256] = "Initializing...";
static float g_progress = 0.0f;

/* Asset-load tracking + timer */
static char     g_asset_line[200] = "";
static int      g_asset_count = 0;
static uint64_t g_load_start_us = 0;
static uint64_t g_load_end_us = 0;
static int      g_loading_done = 0;
static uint64_t g_last_render_us = 0;

static uint64_t ls_now_us(void) { return sceKernelGetProcessTimeWide(); }

static unsigned ls_elapsed_ms(void) {
    if (!g_load_start_us) return 0;
    uint64_t end = g_loading_done ? g_load_end_us : ls_now_us();
    return (unsigned)((end - g_load_start_us) / 1000);
}
static int   g_initialized = 0;

#define LS_MAX_SAVED_ATTRIBS 16

typedef struct ls_attrib_state {
    GLint enabled;
    GLint size;
    GLint stride;
    GLint type;
    GLint normalized;
    GLint buffer;
    void *pointer;
} ls_attrib_state;

typedef struct ls_gl_state {
    GLint program;
    GLint array_buffer;
    GLint element_buffer;
    GLint framebuffer;
    GLint active_texture;
    GLint texture0;
    GLint viewport[4];
    GLfloat clear_color[4];
    GLboolean color_mask[4];
    GLboolean blend;
    GLboolean depth_test;
    GLboolean cull_face;
    GLboolean scissor_test;
    GLint blend_src_rgb;
    GLint blend_dst_rgb;
    GLint blend_src_alpha;
    GLint blend_dst_alpha;
    GLint blend_equation_rgb;
    GLint blend_equation_alpha;
    GLint attrib_count;
    ls_attrib_state attribs[LS_MAX_SAVED_ATTRIBS];
} ls_gl_state;

static void ls_save_gl_state(ls_gl_state *s) {
    GLint max_attribs = 0;

    memset(s, 0, sizeof(*s));
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->element_buffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->framebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture0);

    s->blend = glIsEnabled(GL_BLEND);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->cull_face = glIsEnabled(GL_CULL_FACE);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_equation_alpha);

    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
    if (max_attribs < 0) {
        max_attribs = 0;
    }
    if (max_attribs > LS_MAX_SAVED_ATTRIBS) {
        max_attribs = LS_MAX_SAVED_ATTRIBS;
    }
    s->attrib_count = max_attribs;

    for (GLint i = 0; i < s->attrib_count; ++i) {
        ls_attrib_state *a = &s->attribs[i];
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->normalized);
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buffer);
        glGetVertexAttribPointerv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->pointer);
    }

    (void)glGetError();
}

static void ls_restore_cap(GLenum cap, GLboolean enabled) {
    if (enabled) {
        glEnable(cap);
    } else {
        glDisable(cap);
    }
}

static void ls_restore_gl_state(const ls_gl_state *s) {
    for (GLint i = 0; i < s->attrib_count; ++i) {
        const ls_attrib_state *a = &s->attribs[i];
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buffer);
        glVertexAttribPointer((GLuint)i,
                              a->size,
                              (GLenum)a->type,
                              (GLboolean)a->normalized,
                              a->stride,
                              a->pointer);
        if (a->enabled) {
            glEnableVertexAttribArray((GLuint)i);
        } else {
            glDisableVertexAttribArray((GLuint)i);
        }
    }

    glUseProgram((GLuint)s->program);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s->element_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->framebuffer);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glBlendFuncSeparate((GLenum)s->blend_src_rgb,
                        (GLenum)s->blend_dst_rgb,
                        (GLenum)s->blend_src_alpha,
                        (GLenum)s->blend_dst_alpha);
    glBlendEquationSeparate((GLenum)s->blend_equation_rgb,
                            (GLenum)s->blend_equation_alpha);
    ls_restore_cap(GL_BLEND, s->blend);
    ls_restore_cap(GL_DEPTH_TEST, s->depth_test);
    ls_restore_cap(GL_CULL_FACE, s->cull_face);
    ls_restore_cap(GL_SCISSOR_TEST, s->scissor_test);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture0);
    glActiveTexture((GLenum)s->active_texture);
    (void)glGetError();
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetShaderInfoLog(shader, sizeof(log)-1, NULL, log);
        l_error("LS: shader compile failed (type=%d): %s", type, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(const char *vs, const char *fs) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;
    GLuint vsh  = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint fsh  = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vsh || !fsh) {
        if (vsh) glDeleteShader(vsh);
        if (fsh) glDeleteShader(fsh);
        glDeleteProgram(prog);
        return 0;
    }
    glAttachShader(prog, vsh);
    glAttachShader(prog, fsh);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetProgramInfoLog(prog, sizeof(log)-1, NULL, log);
        l_error("LS: program link failed: %s", log);
    }
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    return prog;
}


void loading_screen_init(int fb_w, int fb_h) {
    if (g_initialized) return;
    g_fb_w = fb_w;
    g_fb_h = fb_h;

    g_prog_text = link_program(g_vs_src, g_fs_src);
    g_prog_solid = link_program(g_vs_solid, g_fs_solid);
    l_info("LS: prog_text=%u prog_solid=%u", g_prog_text, g_prog_solid);

    /* If either program failed to build, do NOT touch any more GL state — the
     * texture/buffer setup below was crashing the boot. Disable the loading
     * screen entirely and let the game render through its own (working) GL. */
    if (!g_prog_text || !g_prog_solid) {
        l_error("LS: program build failed (text=%u solid=%u); loading screen disabled",
                g_prog_text, g_prog_solid);
        if (g_prog_text)  { glDeleteProgram(g_prog_text);  g_prog_text  = 0; }
        if (g_prog_solid) { glDeleteProgram(g_prog_solid); g_prog_solid = 0; }
        g_initialized = 0;
        return;
    }

    /* Build the 760x8 (95 glyphs * 8px) RGBA font atlas by UNPACKING the
     * 8x8-bitmap font: each of a glyph's 8 bytes is a row, each bit a pixel.
     * The old code only wrote 760 texels (one per byte) into a 760-texel buffer
     * but declared a 760x8 = 6080-texel texture, so glTexImage2D over-read ~21KB
     * past the buffer → Data abort inside libGLESv2. Buffer is now correctly
     * sized (static to avoid a 24KB stack frame). */
    {
        static unsigned char rgba_font[760 * 8 * 4];
        int g, row, col;
        memset(rgba_font, 0, sizeof(rgba_font));
        for (g = 0; g < 95; g++) {
            for (row = 0; row < 8; row++) {
                unsigned char bits = g_font[g * 8 + row];
                for (col = 0; col < 8; col++) {
                    int on = (bits >> (7 - col)) & 1;
                    int x = g * 8 + col;            /* 0..759 */
                    int idx = (row * 760 + x) * 4;  /* row-major, 760 wide */
                    rgba_font[idx + 0] = 255;
                    rgba_font[idx + 1] = 255;
                    rgba_font[idx + 2] = 255;
                    rgba_font[idx + 3] = on ? 255 : 0;
                }
            }
        }
        glGenTextures(1, &g_font_tex);
        glBindTexture(GL_TEXTURE_2D, g_font_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 760, 8, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba_font);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenBuffers(1, &g_vbo);

    g_initialized = 1;
    loading_screen_start_timer();
    l_info("LS: loading screen init done (%dx%d)", fb_w, fb_h);
    loading_screen_render();
}

void loading_screen_set_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    l_info("LS: %s", g_status);
}

void loading_screen_set_progress(float progress) {
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    g_progress = progress;
}

/* Draw a solid-color quad in NDC (-1..1) */
static void draw_quad_solid(float x0, float y0, float x1, float y1,
                             float r, float g, float b, float a) {
    if (!g_prog_solid) return;
    float verts[] = {
        x0, y0,  x1, y0,  x1, y1,
        x0, y0,  x1, y1,  x0, y1,
    };
    glDisable(GL_BLEND);
    glUseProgram(g_prog_solid);
    GLint loc = glGetUniformLocation(g_prog_solid, "u_color");
    glUniform4f(loc, r, g, b, a);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    GLint apos = glGetAttribLocation(g_prog_solid, "a_pos");
    glVertexAttribPointer(apos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(apos);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(apos);
}


/* Base glyph cell height in NDC at scale 1.0. The width is aspect-corrected
 * against the framebuffer so the 8x8 font keeps roughly square pixels instead
 * of being horizontally squished (the old fixed 0.018x0.050 was cramped). */
#define GLYPH_BASE_H 0.052f

static float glyph_w(float scale) {
    float h = GLYPH_BASE_H * scale;
    return h * ((float)g_fb_h / (float)g_fb_w);
}
static float glyph_h(float scale) { return GLYPH_BASE_H * scale; }

static float text_width(const char *s, float scale) {
    size_t n = s ? strlen(s) : 0;
    /* 0.92 advance so glyphs sit close but not touching. */
    return (float)n * glyph_w(scale) * 0.92f;
}

static void draw_text(float x, float y, float scale, const char *str,
                      float r, float gr, float b, float a) {
    if (!str || !*str || !g_prog_text || !g_font_tex) return;
    float ch_w = glyph_w(scale);
    float ch_h = glyph_h(scale);
    float adv  = ch_w * 0.92f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog_text);
    GLint uloc = glGetUniformLocation(g_prog_text, "u_color");
    GLint tloc = glGetUniformLocation(g_prog_text, "u_tex");
    GLint apos = glGetAttribLocation(g_prog_text, "a_pos");
    GLint auv  = glGetAttribLocation(g_prog_text, "a_uv");

    glUniform4f(uloc, r, gr, b, a);
    glUniform1i(tloc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    float cx = x;
    for (const char *p = str; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) {
            if (*p == '\n') { cx = x; y -= ch_h * 1.5f; }
            continue;
        }
        int glyph = ch - 32;
        /* Sample 7/8 of the cell so the blank guard column between glyphs
         * isn't stretched into the letter. */
        float u0 = (float)glyph / 95.0f;
        float u1 = ((float)glyph + 0.875f) / 95.0f;

        float verts[] = {
            cx,        y,         u0, 1.0f,
            cx+ch_w,   y,         u1, 1.0f,
            cx+ch_w,   y+ch_h,    u1, 0.0f,
            cx,        y,         u0, 1.0f,
            cx+ch_w,   y+ch_h,    u1, 0.0f,
            cx,        y+ch_h,    u0, 0.0f,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

        glVertexAttribPointer(apos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0);
        glVertexAttribPointer(auv,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float),
                              (void*)(2*sizeof(float)));
        glEnableVertexAttribArray(apos);
        glEnableVertexAttribArray(auv);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(apos);
        glDisableVertexAttribArray(auv);

        cx += adv;
    }
}

/* Draw horizontally centered around cx_center. */
static void draw_text_c(float cx_center, float y, float scale, const char *str,
                        float r, float g, float b, float a) {
    draw_text(cx_center - text_width(str, scale) * 0.5f, y, scale, str, r, g, b, a);
}

static void loading_screen_draw_current(void) {
    char status[sizeof(g_status)];
    char asset_line[sizeof(g_asset_line)];
    float progress;
    int loading_done;
    ls_gl_state saved;

    snprintf(status, sizeof(status), "%s", g_status);
    snprintf(asset_line, sizeof(asset_line), "%s", g_asset_line);
    progress = g_progress;
    loading_done = g_loading_done;

    ls_save_gl_state(&saved);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_fb_w, g_fb_h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    /* Dark, slightly green-tinted background for contrast. */
    glClearColor(0.04f, 0.06f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Header band (Minecraft-grass green) with title. */
    draw_quad_solid(-1.0f, 0.66f, 1.0f, 1.0f, 0.18f, 0.42f, 0.20f, 1.0f);
    draw_quad_solid(-1.0f, 0.655f, 1.0f, 0.66f, 0.45f, 0.78f, 0.40f, 1.0f);
    draw_text_c(0.0f, 0.80f, 1.7f, "MINECRAFT STORY MODE", 1.0f, 1.0f, 1.0f, 1.0f);
    draw_text_c(0.0f, 0.70f, 0.75f, "PS Vita  -  PowerVR SGX driver",
                0.80f, 0.92f, 0.80f, 1.0f);

    /* Big status line. */
    draw_text_c(0.0f, 0.34f, 1.25f, status, 0.95f, 0.95f, 1.0f, 1.0f);

    /* Current asset being loaded. */
    if (asset_line[0]) {
        draw_text_c(0.0f, 0.14f, 0.85f, asset_line, 0.55f, 0.95f, 0.55f, 1.0f);
    }

    /* Animated "working" dots (so it reads as alive even on a static asset). */
    if (!loading_done) {
        unsigned phase = (ls_elapsed_ms() / 400u) % 4u;
        char dots[8] = "   ";
        for (unsigned i = 0; i < phase && i < 3; i++) dots[i] = '.';
        draw_text_c(0.0f, -0.02f, 1.0f, dots, 0.6f, 0.9f, 0.6f, 1.0f);
    }

    /* Progress bar (border + fill). When we have no real fraction the timer is
     * the real signal, so keep the bar subtle. */
    draw_quad_solid(-0.72f, -0.34f, 0.72f, -0.24f, 0.10f, 0.14f, 0.10f, 1.0f);
    draw_quad_solid(-0.715f, -0.335f, 0.715f, -0.245f, 0.06f, 0.08f, 0.06f, 1.0f);
    {
        float p = progress;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        float fill = -0.71f + 1.42f * p;
        if (fill > -0.71f)
            draw_quad_solid(-0.71f, -0.33f, fill, -0.25f, 0.40f, 0.80f, 0.35f, 1.0f);
    }

    /* Loading time, centered + prominent. */
    {
        unsigned ms = ls_elapsed_ms();
        char tline[64];
        snprintf(tline, sizeof(tline), "%s %u.%u s",
                 loading_done ? "Loaded in" : "Loading time:",
                 ms / 1000, (ms % 1000) / 100);
        draw_text_c(0.0f, -0.52f, 1.0f, tline, 0.95f, 0.90f, 0.55f, 1.0f);
    }

    /* Footer. */
    draw_text_c(0.0f, -0.92f, 0.7f, "Minecraft Story Mode  -  PS Vita port",
                0.45f, 0.55f, 0.45f, 1.0f);

#ifdef USE_PVR_PSP2
    eglSwapBuffers(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
#else
    /* vitaGL: present FB 0 directly. Do NOT route through the egl reimpl's
     * eglSwapBuffers (which calls gl_swap and would blit the render-scale FBO
     * over our loading-screen image). We drew straight to the native FB 0. */
    vglSwapBuffers(GL_FALSE);
#endif
    ls_restore_gl_state(&saved);
}

void loading_screen_render(void) {
    if (!g_initialized) return;
    if (!g_prog_solid && !g_prog_text) return;
#ifdef USE_PVR_PSP2
    if (!pvr_make_current()) return;
#endif

    loading_screen_draw_current();
}

int loading_screen_try_render(void) {
    if (!g_initialized) return 0;
    if (!g_prog_solid && !g_prog_text) return 0;
    /* The game render thread (tid 0x40010409) owns the GL context for swaps.
     * Stealing it here for a loading-screen redraw causes the PVR DIAG ctx
     * owner ping-pong (0x40010225 <-> 0x40010409) that corrupts every frame.
     * The screen is drawn only once during boot (loading_screen_render from
     * the run_game thread) and the status text is only for the log. */
    return 0;
}

void loading_screen_start_timer(void) {
    if (!g_load_start_us) {
        g_load_start_us = ls_now_us();
        l_info("LS: asset-load timer started");
    }
}

void loading_screen_begin(void) {
    /* Restart the per-load timer + animation for a fresh scene load. */
    g_load_start_us = 0;
    g_load_end_us = 0;
    g_loading_done = 0;
    g_asset_line[0] = '\0';
    loading_screen_start_timer();
}

void loading_screen_set_asset(const char *name) {
    if (!name || !*name) return;
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    g_asset_count++;
    snprintf(g_asset_line, sizeof(g_asset_line), "[%d] %s", g_asset_count, base);
}

int loading_screen_is_ready(void) {
    return g_initialized;
}

void loading_screen_mark_loaded(void) {
    if (g_loading_done) return;
    g_loading_done = 1;
    g_load_end_us = ls_now_us();
    unsigned ms = ls_elapsed_ms();
    l_info("LS: *** assets loaded: %d items in %u.%03us ***",
           g_asset_count, ms / 1000, ms % 1000);
    snprintf(g_status, sizeof(g_status), "Loaded %d assets - starting game...",
             g_asset_count);
}

void loading_screen_tick(void) {
    static volatile int rendering = 0;
    if (!g_initialized || g_loading_done) return;
    uint64_t now = ls_now_us();
    /* Throttle to ~150ms so swap-vsync waits don't slow asset loading. */
    if (g_last_render_us && (now - g_last_render_us) < 150000) return;
    /* Prevent two threads (LoadResource vs the Lua loading callback) from
     * driving GL/eglMakeCurrent concurrently. */
    if (__atomic_test_and_set(&rendering, __ATOMIC_ACQUIRE)) return;
    g_last_render_us = now;
    if (g_asset_line[0]) {
        snprintf(g_status, sizeof(g_status), "Loading game assets...");
    }
    (void)loading_screen_try_render();
    __atomic_clear(&rendering, __ATOMIC_RELEASE);
}

