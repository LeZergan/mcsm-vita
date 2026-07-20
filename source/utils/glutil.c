/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "reimpl/egl.h"
#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/launch_state.h"
#include "utils/loading_screen.h"
#include "java_runtime.h"
#ifdef USE_PVR_PSP2
#include <GLES2/gl2ext.h>
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH_STENCIL
#endif
#endif

#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelPowerTick — keep the Vita awake while the game runs */
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

static const char k_gl_vendor[] = "Imagination Technologies";
static const char k_gl_renderer[] = "PowerVR SGX 543MP";
static const char k_gl_version[] = "OpenGL ES 2.0 build 1.10@2516585";
static const char k_glsl_version[] = "OpenGL ES GLSL ES 1.00";
static int g_gl_identity_logged = 0;
static int g_gl_real_identity_logged = 0;
static int g_gl_extensions_logged = 0;
static char g_sync_sentinel = 0;
static char g_gl_extensions_spoof[4096];

enum {
    SHADER_FLAG_EXT_STANDARD_DERIVATIVES = 1u << 0,
    SHADER_FLAG_EXT_SHADOW_SAMPLERS = 1u << 1,
    SHADER_FLAG_EXT_SHADER_FRAMEBUFFER_FETCH = 1u << 2,
    SHADER_FLAG_EXT_FRAG_DEPTH = 1u << 3,
    SHADER_FLAG_EXT_SHADER_TEXTURE_LOD = 1u << 4,
    SHADER_FLAG_ARB_SHADER_TEXTURE_LOD = 1u << 5,
    SHADER_FLAG_USES_DERIVATIVES = 1u << 6,
    SHADER_FLAG_USES_FRAG_DEPTH = 1u << 7,
    SHADER_FLAG_USES_TEXTURE_LOD = 1u << 8,
    SHADER_FLAG_USES_SHADOW_SAMPLER = 1u << 9,
    SHADER_FLAG_USES_FRAMEBUFFER_FETCH = 1u << 10,
};

typedef struct shader_diag_entry {
    GLuint shader;
    uint32_t flags;
    char stage[8];
    char sha1[48];
    char source_path[256];
    char preview[224];
    char *owned_source;
    size_t owned_source_len;
} shader_diag_entry;

/* RAISED 512->2048 (2026-07-17): the progcache key hashes each attached shader's
 * owned_source held here; if this table evicts a live shader (shader_id % CAP
 * collision) before glLinkProgram, its source is freed and the key can't be
 * computed -> that program becomes permanently UNCACHEABLE (recompiles every
 * session). A full-game playthrough compiles ~1001 unique shaders, so 512 was far
 * too small. 2048 covers the whole shader population with no eviction. */
#define SHADER_DIAG_CAP 2048
static shader_diag_entry g_shader_diag[SHADER_DIAG_CAP];

#define PROGRAM_CACHE_CAP 32
#define PROGRAM_UNIFORM_CAP 192
#define PROGRAM_UNIFORM_NAME_CAP 64

typedef struct program_uniform_entry {
    GLint location;
    char name[PROGRAM_UNIFORM_NAME_CAP];
} program_uniform_entry;

typedef struct program_uniform_cache {
    GLuint program;
    int valid;
    int uniform_count;
    program_uniform_entry uniforms[PROGRAM_UNIFORM_CAP];
} program_uniform_cache;

static program_uniform_cache g_program_uniform_cache[PROGRAM_CACHE_CAP];
static GLuint g_uniform_current_program = 0;

#ifndef MCSM_FAST_FINAL_RUNTIME
#define MCSM_FAST_FINAL_RUNTIME 1
#endif

#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#ifndef GL_ACTIVE_UNIFORMS
#define GL_ACTIVE_UNIFORMS 0x8B86
#endif

#ifndef GL_ACTIVE_UNIFORM_MAX_LENGTH
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#endif

#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

#ifndef GL_RGB565
#define GL_RGB565 0x8D62
#endif

#ifndef GL_RGBA4
#define GL_RGBA4 0x8056
#endif

#ifndef GL_RGB5_A1
#define GL_RGB5_A1 0x8057
#endif

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

static int has_extension_token(const char *extensions, const char *name) {
    const size_t name_len = strlen(name);
    const char *cur = extensions;

    if (!extensions || !name || name_len == 0) {
        return 0;
    }

    while ((cur = strstr(cur, name)) != NULL) {
        const int starts_at_token = (cur == extensions) || (cur[-1] == ' ');
        const int ends_at_token = (cur[name_len] == '\0') || (cur[name_len] == ' ');
        if (starts_at_token && ends_at_token) {
            return 1;
        }
        cur += name_len;
    }

    return 0;
}

static const char *get_augmented_extension_string(const char *extensions) {
    const char *base = extensions ? extensions : "";
    char augmented[512] = {0};

    if (!has_extension_token(base, "GL_EXT_discard_framebuffer")) {
        strncat(augmented, " GL_EXT_discard_framebuffer", sizeof(augmented) - strlen(augmented) - 1);
    }
    if (!has_extension_token(base, "GL_EXT_texture_format_BGRA8888")) {
        strncat(augmented, " GL_EXT_texture_format_BGRA8888", sizeof(augmented) - strlen(augmented) - 1);
    }
    if (!has_extension_token(base, "GL_IMG_texture_format_BGRA8888")) {
        strncat(augmented, " GL_IMG_texture_format_BGRA8888", sizeof(augmented) - strlen(augmented) - 1);
    }
    if (augmented[0] == '\0') {
        return base;
    }
    if (augmented[0] == ' ' && base[0] == '\0') {
        memmove(augmented, augmented + 1, strlen(augmented));
    }
    snprintf(g_gl_extensions_spoof,
             sizeof(g_gl_extensions_spoof),
             "%s%s%s",
             base,
             (base[0] != '\0' && augmented[0] != '\0') ? " " : "",
             augmented[0] == ' ' ? augmented + 1 : augmented);
    return g_gl_extensions_spoof;
}

static shader_diag_entry *get_shader_diag_entry(GLuint shader, int create) {
    shader_diag_entry *free_slot = NULL;

    for (size_t i = 0; i < SHADER_DIAG_CAP; ++i) {
        if (g_shader_diag[i].shader == shader) {
            return &g_shader_diag[i];
        }
        if (create && !free_slot && g_shader_diag[i].shader == 0) {
            free_slot = &g_shader_diag[i];
        }
    }

    if (!create) {
        return NULL;
    }

    if (!free_slot) {
        free_slot = &g_shader_diag[shader % SHADER_DIAG_CAP];
        if (free_slot->owned_source) {
            free(free_slot->owned_source);
            free_slot->owned_source = NULL;
            free_slot->owned_source_len = 0;
        }
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->shader = shader;
    return free_slot;
}

static uint32_t analyze_shader_source_flags(const char *source) {
    uint32_t flags = 0;

    if (!source) {
        return 0;
    }

    if (strstr(source, "GL_OES_standard_derivatives")) {
        flags |= SHADER_FLAG_EXT_STANDARD_DERIVATIVES;
    }
    if (strstr(source, "GL_EXT_shadow_samplers")) {
        flags |= SHADER_FLAG_EXT_SHADOW_SAMPLERS;
    }
    if (strstr(source, "GL_EXT_shader_framebuffer_fetch")) {
        flags |= SHADER_FLAG_EXT_SHADER_FRAMEBUFFER_FETCH;
    }
    if (strstr(source, "GL_EXT_frag_depth")) {
        flags |= SHADER_FLAG_EXT_FRAG_DEPTH;
    }
    if (strstr(source, "GL_EXT_shader_texture_lod")) {
        flags |= SHADER_FLAG_EXT_SHADER_TEXTURE_LOD;
    }
    if (strstr(source, "GL_ARB_shader_texture_lod")) {
        flags |= SHADER_FLAG_ARB_SHADER_TEXTURE_LOD;
    }
    if (strstr(source, "dFdx") || strstr(source, "dFdy") || strstr(source, "fwidth")) {
        flags |= SHADER_FLAG_USES_DERIVATIVES;
    }
    if (strstr(source, "gl_FragDepth") || strstr(source, "gl_FragDepthEXT")) {
        flags |= SHADER_FLAG_USES_FRAG_DEPTH;
    }
    if (strstr(source, "texture2DLod") || strstr(source, "textureCubeLod") || strstr(source, "texture2DGrad")) {
        flags |= SHADER_FLAG_USES_TEXTURE_LOD;
    }
    if (strstr(source, "shadow2D") || strstr(source, "sampler2DShadow")) {
        flags |= SHADER_FLAG_USES_SHADOW_SAMPLER;
    }
    if (strstr(source, "gl_LastFragData")) {
        flags |= SHADER_FLAG_USES_FRAMEBUFFER_FETCH;
    }

    return flags;
}

static void build_shader_preview(const char *source, char *dst, size_t dst_size) {
    size_t out = 0;
    int prev_space = 1;

    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!source) {
        return;
    }

    while (*source && out + 1 < dst_size) {
        const unsigned char ch = (unsigned char)*source++;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            if (!prev_space && out + 1 < dst_size) {
                dst[out++] = ' ';
                prev_space = 1;
            }
            continue;
        }

        if (ch == ' ') {
            if (!prev_space && out + 1 < dst_size) {
                dst[out++] = ' ';
                prev_space = 1;
            }
            continue;
        }

        dst[out++] = (char)ch;
        prev_space = 0;
    }

    if (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
}

static size_t get_shader_source_part_length(const GLchar *part, const GLint *_length, int index) {
    if (!part) {
        return 0;
    }

    if (!_length || _length[index] < 0) {
        return strlen(part);
    }

    /* CRITICAL (2026-07-20, device-proven): the Telltale engine passes an explicit
     * _length equal to the source BUFFER size, not the text length — the real GLSL
     * is NUL-terminated inside it and followed by UNINITIALIZED padding that varies
     * every process launch (missdump proof: identical GLSL + a trailing "\0" then
     * bytes like ff f0 ff f8...). Capturing that padding into owned_source made the
     * progcache FNV key non-deterministic across reboots, so byte-identical shaders
     * hashed to different keys -> the shipped cache NEVER matched -> every launch
     * recompiled every shader (the 152-3428ms ShaccCg freezes / "hard stutters").
     * Clamp to the NUL terminator when one falls within the claimed length. This is
     * a no-op for genuinely non-NUL-terminated exact-length sources, and the GLSL
     * compiler already stopped at that NUL, so rendering is unchanged. */
    const size_t claimed = (size_t)_length[index];
    const void *nul = memchr(part, '\0', claimed);
    return nul ? (size_t)((const char *)nul - part) : claimed;
}

static int shader_file_diag_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        initialized = 1;
        enabled = file_exists(DATA_PATH "shader_diag.txt") ||
                  file_exists("ux0:data/mcsm/shader_diag.txt") ||
                  file_exists("ux0:/data/mcsm/shader_diag.txt");
    }
    return enabled;
}

static int gl_verbose_diag_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        initialized = 1;
        enabled = file_exists(DATA_PATH "megadiag.txt") ||
                  file_exists("ux0:data/mcsm/megadiag.txt") ||
                  file_exists("ux0:/data/mcsm/megadiag.txt") ||
                  file_exists(DATA_PATH "texdiag.txt") ||
                  file_exists("ux0:data/mcsm/texdiag.txt") ||
                  file_exists("ux0:/data/mcsm/texdiag.txt");
    }
    return enabled;
}

static void track_shader_source(GLuint shader, const char *source, size_t length) {
    shader_diag_entry *entry;
    char *sha1 = NULL;

    if (!source || length == 0) {
        return;
    }

    /* CRITICAL (2026-07-20b, device-proven): the buffer that reaches here can still
     * carry a NUL terminator followed by NON-SOURCE bytes that vary every launch —
     * device missdumps showed clean vertex shaders but FRAGMENTS ending "...}\n\n\n\0"
     * then garbage (e.g. 48 a4 97 90...). Two upstream paths cause it: the engine's
     * oversized glShaderSource _length, AND glsl_replace_alloc (fragment-only: it
     * copies with strlen but sizes with arithmetic, leaving an uninitialized tail).
     * Hashing that tail made the progcache key non-deterministic across reboots so
     * the cache never matched -> perpetual recompiles. Clamp the HASHED source to the
     * first embedded NUL here, the final step before it is stored/hashed — this
     * catches every path. The GLSL compiler already stops at that NUL, so the
     * compiled program is unchanged; only the (now deterministic) key differs. */
    {
        const void *nul = memchr(source, '\0', length);
        if (nul) {
            length = (size_t)((const char *)nul - source);
        }
    }
    if (length == 0) {
        return;
    }

    entry = get_shader_diag_entry(shader, 1);
    if (!entry) {
        return;
    }

    if (entry->owned_source) {
        free(entry->owned_source);
        entry->owned_source = NULL;
        entry->owned_source_len = 0;
    }

    entry->owned_source = malloc(length + 1);
    if (entry->owned_source) {
        memcpy(entry->owned_source, source, length);
        entry->owned_source[length] = '\0';
        entry->owned_source_len = length;
    }

    entry->flags = analyze_shader_source_flags(source);
    build_shader_preview(source, entry->preview, sizeof(entry->preview));

    entry->sha1[0] = '\0';
    entry->source_path[0] = '\0';
    if (!shader_file_diag_enabled()) {
        return;
    }

    sha1 = str_sha1sum(source, length);
    if (sha1) {
        snprintf(entry->sha1, sizeof(entry->sha1), "%s", sha1);
        snprintf(entry->source_path, sizeof(entry->source_path), DATA_PATH "diag/shaders/%s.glsl", sha1);
        if (!file_exists(entry->source_path)) {
            file_mkpath(entry->source_path, 0777);
            file_save(entry->source_path, (const uint8_t *)source, length);
        }
        free(sha1);
    } else {
        entry->sha1[0] = '\0';
        entry->source_path[0] = '\0';
    }
}

static void log_shader_diag_context(GLuint shader, GLenum shader_type) {
    shader_diag_entry *entry = get_shader_diag_entry(shader, 0);

    if (!entry) {
        return;
    }

    snprintf(entry->stage, sizeof(entry->stage), "%s",
             (shader_type == GL_FRAGMENT_SHADER) ? "frag" :
             (shader_type == GL_VERTEX_SHADER) ? "vert" : "unknown");

    l_error("glCompileShader(%u) diag: stage=%s sha1=%s source=%s",
            shader,
            entry->stage[0] ? entry->stage : "unknown",
            entry->sha1[0] ? entry->sha1 : "(none)",
            entry->source_path[0] ? entry->source_path : "(unsaved)");

    l_error("glCompileShader(%u) features: ext_std_deriv=%u ext_shadow=%u ext_fb_fetch=%u ext_frag_depth=%u ext_tex_lod=%u arb_tex_lod=%u use_deriv=%u use_frag_depth=%u use_tex_lod=%u use_shadow=%u use_fb_fetch=%u",
            shader,
            (entry->flags & SHADER_FLAG_EXT_STANDARD_DERIVATIVES) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_EXT_SHADOW_SAMPLERS) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_EXT_SHADER_FRAMEBUFFER_FETCH) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_EXT_FRAG_DEPTH) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_EXT_SHADER_TEXTURE_LOD) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_ARB_SHADER_TEXTURE_LOD) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_USES_DERIVATIVES) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_USES_FRAG_DEPTH) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_USES_TEXTURE_LOD) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_USES_SHADOW_SAMPLER) ? 1U : 0U,
            (entry->flags & SHADER_FLAG_USES_FRAMEBUFFER_FETCH) ? 1U : 0U);

    if (entry->preview[0] != '\0') {
        l_error("glCompileShader(%u) preview: %s", shader, entry->preview);
    }
}

static void log_relevant_extension_support(const char *extensions) {
    l_info("GL extensions: OES_mapbuffer=%d EXT_map_buffer_range=%d OES_depth_texture=%d OES_packed_depth_stencil=%d",
           has_extension_token(extensions, "GL_OES_mapbuffer"),
           has_extension_token(extensions, "GL_EXT_map_buffer_range"),
           has_extension_token(extensions, "GL_OES_depth_texture"),
           has_extension_token(extensions, "GL_OES_packed_depth_stencil"));
    l_info("GL extensions: OES_standard_derivatives=%d EXT_shadow_samplers=%d EXT_discard_framebuffer=%d EXT_texture_filter_anisotropic=%d",
           has_extension_token(extensions, "GL_OES_standard_derivatives"),
           has_extension_token(extensions, "GL_EXT_shadow_samplers"),
           has_extension_token(extensions, "GL_EXT_discard_framebuffer"),
           has_extension_token(extensions, "GL_EXT_texture_filter_anisotropic"));
    l_info("GL extensions: EXT_shader_framebuffer_fetch=%d EXT_frag_depth=%d EXT_shader_texture_lod=%d ARB_shader_texture_lod=%d",
           has_extension_token(extensions, "GL_EXT_shader_framebuffer_fetch"),
           has_extension_token(extensions, "GL_EXT_frag_depth"),
           has_extension_token(extensions, "GL_EXT_shader_texture_lod"),
           has_extension_token(extensions, "GL_ARB_shader_texture_lod"));
    l_info("GL extensions: PVRTC=%d ETC1=%d BGRA_EXT=%d BGRA_IMG=%d sRGB=%d",
           has_extension_token(extensions, "GL_IMG_texture_compression_pvrtc"),
           has_extension_token(extensions, "GL_OES_compressed_ETC1_RGB8_texture"),
           has_extension_token(extensions, "GL_EXT_texture_format_BGRA8888"),
           has_extension_token(extensions, "GL_IMG_texture_format_BGRA8888"),
           has_extension_token(extensions, "GL_EXT_sRGB"));
}

static void log_shader_compile_failure(GLuint shader) {
    GLint status = GL_TRUE;
    GLint shader_type = 0;
    GLint log_len = 0;
    GLsizei out_len = 0;
    char stack_log[512];
    char *log_buf = stack_log;

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return;
    }

    glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);

    if (log_len > (GLint)sizeof(stack_log)) {
        log_buf = malloc((size_t)log_len);
        if (!log_buf) {
            log_buf = stack_log;
            log_len = (GLint)sizeof(stack_log);
        }
    }

    if (log_len <= 0) {
        log_len = (GLint)sizeof(stack_log);
    }

    log_buf[0] = '\0';
    glGetShaderInfoLog(shader, log_len, &out_len, log_buf);
    l_error("glCompileShader(%u,type=0x%X) failed: %s",
            shader,
            (unsigned)shader_type,
            (out_len > 0 && log_buf[0] != '\0') ? log_buf : "(no info log)");
    log_shader_diag_context(shader, (GLenum)shader_type);

    if (log_buf != stack_log) {
        free(log_buf);
    }
}

static void log_shader_compile_runtime_state(GLuint shader, const char *phase, GLenum err_code) {
    EGLDisplay dpy = NULL;
    EGLContext ctx = NULL;
    EGLSurface draw = NULL;
    EGLSurface read = NULL;
    GLint source_len = 0;
    GLint shader_type = 0;
    shader_diag_entry *entry = get_shader_diag_entry(shader, 0);

#ifndef USE_PVR_PSP2
    mcsm_egl_get_current_state(&dpy, &ctx, &draw, &read);
#else
    dpy = eglGetCurrentDisplay();
    ctx = eglGetCurrentContext();
    draw = eglGetCurrentSurface(EGL_DRAW);
    read = eglGetCurrentSurface(EGL_READ);
#endif
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &source_len);
    glGetShaderiv(shader, GL_SHADER_TYPE, &shader_type);

    l_info("glCompileShader(%u) %s: tid=0x%X type=0x%X srcLen=%d ownedLen=%u egl_dpy=%p egl_ctx=%p draw=%p read=%p glerr=0x%X",
           shader,
           phase ? phase : "state",
           (unsigned)sceKernelGetThreadId(),
           (unsigned)shader_type,
           source_len,
           entry ? (unsigned)entry->owned_source_len : 0U,
           dpy,
           ctx,
           draw,
           read,
           (unsigned)err_code);
}

static void log_program_link_failure(GLuint program) {
    GLint status = GL_TRUE;
    GLint log_len = 0;
    GLsizei out_len = 0;
    char stack_log[512];
    char *log_buf = stack_log;

    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) {
        return;
    }

    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
    if (log_len > (GLint)sizeof(stack_log)) {
        log_buf = malloc((size_t)log_len);
        if (!log_buf) {
            log_buf = stack_log;
            log_len = (GLint)sizeof(stack_log);
        }
    }

    if (log_len <= 0) {
        log_len = (GLint)sizeof(stack_log);
    }

    log_buf[0] = '\0';
    glGetProgramInfoLog(program, log_len, &out_len, log_buf);
    l_error("glLinkProgram(%u) failed: %s",
            program,
            (out_len > 0 && log_buf[0] != '\0') ? log_buf : "(no info log)");

    if (log_buf != stack_log) {
        free(log_buf);
    }
}

static void normalize_uniform_name(const GLchar *src, char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    size_t len = strlen(src);
    if (len >= 3 && strcmp(src + len - 3, "[0]") == 0) {
        len -= 3;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static program_uniform_cache *program_cache_get(GLuint program, int create) {
    program_uniform_cache *free_slot = NULL;

    if (program == 0) {
        return NULL;
    }

    for (int i = 0; i < PROGRAM_CACHE_CAP; ++i) {
        if (g_program_uniform_cache[i].program == program) {
            return &g_program_uniform_cache[i];
        }
        if (create && !free_slot && g_program_uniform_cache[i].program == 0) {
            free_slot = &g_program_uniform_cache[i];
        }
    }

    if (!create) {
        return NULL;
    }
    if (!free_slot) {
        free_slot = &g_program_uniform_cache[program % PROGRAM_CACHE_CAP];
    }
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->program = program;
    return free_slot;
}

static void program_cache_add_uniform(GLuint program, const char *name, GLint location) {
    if (!name || name[0] == '\0' || location < 0) {
        return;
    }

    program_uniform_cache *cache = program_cache_get(program, 1);
    if (!cache) {
        return;
    }

    for (int i = 0; i < cache->uniform_count; ++i) {
        if (cache->uniforms[i].location == location ||
            strcmp(cache->uniforms[i].name, name) == 0) {
            cache->uniforms[i].location = location;
            return;
        }
    }

    if (cache->uniform_count >= PROGRAM_UNIFORM_CAP) {
        return;
    }

    program_uniform_entry *entry = &cache->uniforms[cache->uniform_count++];
    entry->location = location;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
}

static const char *program_cache_name_for_location(GLuint program, GLint location) {
    program_uniform_cache *cache = program_cache_get(program, 0);
    if (!cache || !cache->valid || location < 0) {
        return NULL;
    }
    for (int i = 0; i < cache->uniform_count; ++i) {
        if (cache->uniforms[i].location == location) {
            return cache->uniforms[i].name;
        }
    }
    return NULL;
}

static GLint program_cache_location_for_name(GLuint program, const char *name) {
    program_uniform_cache *cache = program_cache_get(program, 0);
    if (!name || name[0] == '\0') {
        return -1;
    }
    if (cache && cache->valid) {
        for (int i = 0; i < cache->uniform_count; ++i) {
            if (strcmp(cache->uniforms[i].name, name) == 0) {
                return cache->uniforms[i].location;
            }
        }
    }

    GLint location = glGetUniformLocation(program, name);
    if (location >= 0) {
        program_cache_add_uniform(program, name, location);
    }
    return location;
}

static int parse_telltale_register_name(const char *name, int *bank_out, int *index_out) {
    if (!name || name[0] != 'U') {
        return 0;
    }

    char *end = NULL;
    long bank = strtol(name + 1, &end, 10);
    if (!end || *end != '_') {
        return 0;
    }
    long index = strtol(end + 1, &end, 10);
    if (!end || *end != '\0' || bank < 0 || bank > 999 || index < 0 || index > 4096) {
        return 0;
    }

    if (bank_out) {
        *bank_out = (int)bank;
    }
    if (index_out) {
        *index_out = (int)index;
    }
    return 1;
}

static void program_cache_refresh(GLuint program) {
    GLint link_status = GL_FALSE;
    GLint active_uniforms = 0;
    GLint max_name_len = 0;
    char name_buf[PROGRAM_UNIFORM_NAME_CAP];
    char norm_name[PROGRAM_UNIFORM_NAME_CAP];
    GLsizei out_len = 0;
    GLint size = 0;
    GLenum type = 0;

    program_uniform_cache *cache = program_cache_get(program, 1);
    if (!cache) {
        return;
    }

    cache->valid = 0;
    cache->uniform_count = 0;

    glGetProgramiv(program, GL_LINK_STATUS, &link_status);
    if (link_status != GL_TRUE) {
        return;
    }

    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active_uniforms);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_name_len);
    if (active_uniforms <= 0) {
        cache->valid = 1;
        return;
    }
    if (active_uniforms > PROGRAM_UNIFORM_CAP) {
        active_uniforms = PROGRAM_UNIFORM_CAP;
    }

    for (GLint i = 0; i < active_uniforms; ++i) {
        name_buf[0] = '\0';
        glGetActiveUniform(program,
                           (GLuint)i,
                           (GLsizei)sizeof(name_buf),
                           &out_len,
                           &size,
                           &type,
                           name_buf);
        normalize_uniform_name(name_buf, norm_name, sizeof(norm_name));
        if (norm_name[0] == '\0') {
            continue;
        }
        GLint location = glGetUniformLocation(program, norm_name);
        if (location >= 0) {
            program_cache_add_uniform(program, norm_name, location);
        }
    }

    cache->valid = 1;
    static unsigned s_logged = 0;
    if (s_logged++ < 64U || program == 19U) {
        l_info("glLinkProgram(%u): cached %d active uniforms (reported=%d maxName=%d)",
               program,
               cache->uniform_count,
               active_uniforms,
               max_name_len);
    }
}

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name) {
    if (!name) {
        static unsigned s_null_logged = 0;
        if (s_null_logged++ < 8U) {
            l_warn("glGetUniformLocation(%u, NULL) skipped", program);
        }
        return -1;
    }

    GLint location = glGetUniformLocation(program, name);
    if (location >= 0) {
        char norm_name[PROGRAM_UNIFORM_NAME_CAP];
        normalize_uniform_name(name, norm_name, sizeof(norm_name));
        program_cache_add_uniform(program, norm_name, location);
    }
    return location;
}

static int gl_uniform4fv_split_telltale(GLint location, GLsizei count, const GLfloat *value) {
    if (count <= 1 || location < 0 || !value || count > 64) {
        return 0;
    }

    GLuint program = g_uniform_current_program;
    if (program == 0) {
        GLint queried = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &queried);
        if (queried > 0) {
            program = (GLuint)queried;
        }
    }
    if (program == 0) {
        return 0;
    }

    /* MEMOIZE (2026-07-17): the split verdict + resolved element locations are stable
     * per (program, base location, count). Resolving them every call ran snprintf +
     * failing GXM param scans ~860x per heavy frame (log: prog=1 U2_0 count=3 fell
     * back to plain glUniform4fv EVERY frame, re-searching each time). Cache the
     * verdict: SPLIT replays cached locations, NO_SPLIT returns 0 immediately, both
     * with zero snprintf/lookup. 256-entry LRU keyed by (program, location, count). */
    /* Capacity 256 (was 64): a full-game playthrough uses more than 64 distinct
     * (prog,loc,count) split keys; at 64 the LRU thrashed — evicting live entries
     * forced per-call re-resolve (snprintf + GXM param scans) on the ~860-call/frame
     * hot path. 256 covers the live key set so steady state is pure cached replay.
     * el[64] stays (count is capped at 64 by the guard above). ~70KB extra BSS. */
    static struct split_memo {
        GLuint prog; GLint loc; GLsizei cnt; signed char state; /* 0 empty, 1 split, 2 no-split */
        unsigned used; GLint el[64];
    } s_memo[256];
    static unsigned s_memo_clock = 0;
    int hit = -1, empty = -1;
    for (int i = 0; i < 256; ++i) {
        if (s_memo[i].state && s_memo[i].prog == program &&
            s_memo[i].loc == location && s_memo[i].cnt == count) { hit = i; break; }
        if (!s_memo[i].state && empty < 0) empty = i;
    }
    if (hit >= 0) {
        s_memo[hit].used = ++s_memo_clock;
        if (s_memo[hit].state == 2) return 0;               /* cached NO_SPLIT */
        for (GLsizei i = 0; i < count; ++i)
            glUniform4fv(s_memo[hit].el[i], 1, value + ((size_t)i * 4U));
        return 1;                                            /* cached SPLIT */
    }

    /* First encounter: resolve once, then cache the verdict. */
    const char *base_name = program_cache_name_for_location(program, location);
    int bank = 0, base_index = 0;
    GLint element_locations[64];
    int splittable = parse_telltale_register_name(base_name, &bank, &base_index);
    for (GLsizei i = 0; splittable && i < count; ++i) {
        char element_name[PROGRAM_UNIFORM_NAME_CAP];
        snprintf(element_name, sizeof(element_name), "U%d_%d", bank, base_index + (int)i);
        GLint el = program_cache_location_for_name(program, element_name);
        if (el < 0) { splittable = 0; break; }
        element_locations[i] = el;
    }

    int slot = empty;
    if (slot < 0) {                                          /* full -> evict LRU */
        unsigned oldest = UINT_MAX;
        slot = 0;
        for (int i = 0; i < 256; ++i) if (s_memo[i].used < oldest) { oldest = s_memo[i].used; slot = i; }
    }
    s_memo[slot].prog = program; s_memo[slot].loc = location; s_memo[slot].cnt = count;
    s_memo[slot].used = ++s_memo_clock;
    if (splittable) {
        s_memo[slot].state = 1;
        for (GLsizei i = 0; i < count; ++i) s_memo[slot].el[i] = element_locations[i];
        for (GLsizei i = 0; i < count; ++i)
            glUniform4fv(element_locations[i], 1, value + ((size_t)i * 4U));
        return 1;
    }
    s_memo[slot].state = 2;
    return 0;
}

static int resolve_tex_storage_format(GLenum internalformat, GLenum *format_out, GLenum *type_out) {
    GLenum format = 0;
    GLenum type = GL_UNSIGNED_BYTE;

    switch (internalformat) {
        case GL_RGBA:
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
            format = GL_RGBA;
            type = GL_UNSIGNED_BYTE;
            break;
        case GL_RGB:
        case GL_RGB8:
            format = GL_RGB;
            type = GL_UNSIGNED_BYTE;
            break;
        case GL_RGB565:
            format = GL_RGB;
            type = GL_UNSIGNED_SHORT_5_6_5;
            break;
        case GL_RGBA4:
            format = GL_RGBA;
            type = GL_UNSIGNED_SHORT_4_4_4_4;
            break;
        case GL_RGB5_A1:
            format = GL_RGBA;
            type = GL_UNSIGNED_SHORT_5_5_5_1;
            break;
        case GL_ALPHA:
            format = GL_ALPHA;
            type = GL_UNSIGNED_BYTE;
            break;
        case GL_LUMINANCE:
            format = GL_LUMINANCE;
            type = GL_UNSIGNED_BYTE;
            break;
        case GL_LUMINANCE_ALPHA:
            format = GL_LUMINANCE_ALPHA;
            type = GL_UNSIGNED_BYTE;
            break;
        default:
            return 0;
    }

    if (format_out) {
        *format_out = format;
    }
    if (type_out) {
        *type_out = type;
    }
    return 1;
}

/* Loading-screen-during-scene-loads state (set by patch.c SceneOpen hook). */
int g_scene_loading = 0;
static int g_loadscreen_ready = 0;

/* Per-frame draw stats for dip profiling; reset each gl_swap (see DIP-RENDER). */
unsigned int g_frame_draw_calls = 0;
unsigned long g_frame_draw_verts = 0;

/* LOADING SCREEN DURING SCENE LOADS (2026-06-29): the engine loads a scene's
 * textures synchronously on the GL-context thread, so drive an animated loading
 * screen from the upload path instead of letting the display freeze. */
void mcsm_scene_load_tick(void) {
    if (!g_scene_loading || !(g_loadscreen_ready || loading_screen_is_ready())) {
        return;
    }
    static uint64_t last_us = 0;
    uint64_t now = sceKernelGetSystemTimeWide();
    if (last_us && (now - last_us) < 110000ULL) { /* ~9 fps overlay */
        return;
    }
    last_us = now;
    loading_screen_render();
}

#ifndef USE_PVR_PSP2
void gl_preload() {
    // vitaGL's startShaderCompiler() (called during vglInit) looks for the
    // shader compiler module at ur0:data/external/libshacccg.suprx but the
    // file lives at ur0:/data/libshacccg.suprx (no external/ folder).  If
    // vitaGL's internal call fails, it sets compiler_initialized=false and
    // glCompileShader silently returns failure for EVERY shader (even trivial
    // fallbacks).  Copy the file to where vitaGL expects it before vglInit
    // so the driver's own init path works and sets the flag correctly.
    if (file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        // Create the external dir and copy the compiler module
        sceIoMkdir("ur0:/data/external", 0777);
        int rc = file_copy("ur0:/data/libshacccg.suprx",
                           "ur0:/data/external/libshacccg.suprx");
        l_info("copy libshacccg.suprx -> ur0:/data/external/ -> %d (%s)",
               rc, rc == 0 ? "OK" : "FAIL");
    } else if (!file_exists("ur0:/data/libshacccg.suprx")
               && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

/* ---- Render-scale (opt-in via ux0:data/mcsm/fb_override.txt = "WxH") --------
 * Render the game into a low-res FBO and bilinear-upscale it to the native
 * 960x544 display on present. Fewer fragment-shaded pixels => higher fps on this
 * fragment-bound 3D engine, while the picture stays native/fullscreen.
 * SAFE BY DEFAULT: with no override file the game's render res == native, so this
 * stays inactive and rendering goes straight to the display (the working path).
 * TUNABLE with no rebuild: edit fb_override.txt (e.g. 640x363, 576x326, 480x272). */
#define RS_NATIVE_W 960
#define RS_NATIVE_H 544
static GLboolean g_rs_active = GL_FALSE;
static GLuint g_rs_fbo = 0, g_rs_color = 0, g_rs_depth = 0;
static int g_rs_w = 0, g_rs_h = 0;


static void rs_init(int w, int h) {
    glGenTextures(1, &g_rs_color);
    glBindTexture(GL_TEXTURE_2D, g_rs_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &g_rs_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_rs_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

    glGenFramebuffers(1, &g_rs_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rs_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rs_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rs_depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_rs_depth);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st == GL_FRAMEBUFFER_COMPLETE) {
        g_rs_active = GL_TRUE; g_rs_w = w; g_rs_h = h;
        l_info("render-scale ACTIVE: game renders %dx%d -> upscaled to %dx%d native", w, h, RS_NATIVE_W, RS_NATIVE_H);
        /* leave g_rs_fbo bound: the game's first frame renders straight into it */
    } else {
        l_warn("render-scale FBO incomplete (status=0x%X) -> rendering native instead", (unsigned)st);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

/* Redirect the game's "bind the screen" (FB 0) to the low-res FBO while render-
 * scale is active. gl_swap binds the REAL FB 0 directly (it calls vitaGL, not
 * this wrapper) for the upscale blit + present. */
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    if (g_rs_active && framebuffer == 0) framebuffer = g_rs_fbo;
    glBindFramebuffer(target, framebuffer);
}

/* Passthrough: the game renders at the FBO's fixed resolution (= its override res).
 * (Per-frame dynamic-res scaling was removed — the engine renders to intermediate
 * FBOs, so it never worked; a lower fixed fb_override is the real GPU lever.) */
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
}

/* Present-side frame-lock period (us) = 1/fps_cap. Read ONCE in gl_init (safe
 * context), used by gl_swap. 0 = no lock. (Reading the file in gl_swap's hot
 * render path crashed boot — fopen there races the engine/loader I/O.) */
static int g_present_period_us = 0;

/* Last present-to-present interval (us), written every frame by gl_swap. Read
 * (racy but benign — a heuristic on an aligned 32-bit word) by the clock
 * governor in patch.c so it never downclocks a scene whose render/present is
 * already missing the frame target (sim-work alone is blind to the ~900-draw
 * submission cost). 0 = not yet measured. */
volatile uint32_t g_mcsm_present_dt_us = 0;

void gl_init() {
    /* The game renders at its (override) resolution into a matching FBO; the FBO is
     * then upscaled to the native 960x544 display. DPI is scaled with the res
     * (java.c) so UI lays out at native proportions. The display surface is native. */
    const int render_w = mcsm_get_framebuffer_width();
    const int render_h = mcsm_get_framebuffer_height();
    l_info("gl_init render=%dx%d display=%dx%d", render_w, render_h, RS_NATIVE_W, RS_NATIVE_H);
    /* The vitaGL display surface is ALWAYS native; render-scale (if any) upscales
     * the game's low-res FBO into it on present. */
    /* MCSM (2026-06-30 / 2026-07-03): vglInitExtended(...,ram_threshold,...)
     * makes vitaGL allocate (free_user - ram_threshold) as its RAM texture
     * pool and reserve `ram_threshold` for the engine's mmap pools. At 180MB
     * with a tight budget the RAM pool came out to 0 bytes (device log:
     * `VGLfree RAM=0KB`) -> when VRAM (down to ~18MB free in heavy scenes)
     * exhausts, textures have NO fallback -> black. Lowering the threshold
     * gives vitaGL a RAM pool but risks the engine OOMing (threshold=2MB
     * previously crashed boot). The safe balance is UNKNOWN without the real
     * numbers, so: (1) make it a DEVICE-FILE tunable (ux0:data/mcsm/
     * vram_reserve.txt = MB, default 180) so it can be dialed in WITHOUT a
     * rebuild, and (2) LOG the free memory + resulting pool sizes so we can
     * tune precisely. Clamp to a sane 32..208MB.
     *
     * NOTE 2026-07-03: default LOWERED 180->64. The RAM pool = free_user(~96MB)
     * - ram_reserve, so 180 meant a ZERO texture-RAM pool (no fallback) — which
     * forced very aggressive texture downsampling (blurry 3D). With reserve=64
     * we hand vitaGL a ~32MB RAM texture pool: when VRAM (CDRAM) fills, textures
     * fall back to RAM instead of corrupting CDRAM / GPUCRASH. That safety net is
     * what lets us relax the downsampler (see dsamp_min_dim). free_user stays ~96MB
     * through scenes so leaving the engine ~64MB of newlib headroom is safe. */
    int ram_reserve_mb = 48;   /* pool = free_user(~96MB) - reserve; LOWER reserve = BIGGER RAM texture fallback pool. 48 -> ~48MB pool so heavy-scene VRAM OOM lands in RAM (not the black 1x1 placeholder) while leaving the engine ~48MB newlib mmap headroom. Tunable via vram_reserve.txt. */
    {
        FILE *rf = fopen("ux0:data/mcsm/vram_reserve.txt", "r");
        if (rf) {
            int v = 0;
            if (fscanf(rf, "%d", &v) == 1 && v >= 32 && v <= 208) {
                ram_reserve_mb = v;
            }
            fclose(rf);
        }
    }
    {
        SceKernelFreeMemorySizeInfo info;
        info.size = sizeof(info);
        if (sceKernelGetFreeMemorySize(&info) == 0) {
            l_info("gl_init MEM (pre-vglInit): free_user=%uKB free_cdram=%uKB free_phycont=%uKB ram_reserve=%dMB",
                   (unsigned)(info.size_user / 1024), (unsigned)(info.size_cdram / 1024),
                   (unsigned)(info.size_phycont / 1024), ram_reserve_mb);
        }
    }
    /* Enlarge the GXM ring buffers beyond their tiny defaults (VDM 128KB, vertex
     * 2MB, fragment 512KB). A heavy 3D scene submits enough draws/geometry to
     * stall waiting on the default rings; enlarging is cheap (~+2.4MB) and removes
     * those CPU<->GPU ring waits = steadier fps in busy scenes. Must precede
     * vglInit. Default ON (very low risk — just larger buffers); opt-out via
     * no_gxm_tune.txt. */
    { FILE *nt = fopen("ux0:data/mcsm/no_gxm_tune.txt", "r");
      if (nt) { fclose(nt); l_info("gl_init: GXM ring-buffer tuning DISABLED (no_gxm_tune.txt)"); }
      else {
          vglSetVDMBufferSize(512 * 1024);          /* 128KB -> 512KB */
          vglSetVertexBufferSize(4 * 1024 * 1024);  /* 2MB   -> 4MB   */
          vglSetFragmentBufferSize(1024 * 1024);    /* 512KB -> 1MB   */
          l_info("gl_init: GXM rings enlarged (VDM=512K vtx=4M frag=1M) for heavy-scene throughput");
      } }
    /* Untapped vitaGL perf levers. vitaGL ALREADY defaults to: triple-buffering ON,
     * VRAM-first ON, and newlib mem as a final texture fallback (vglUseExtraMem) ON —
     * so those are not free wins. These two ARE untapped but carry risk, so they are
     * OPT-IN (default off, no rebuild to toggle):
     *   cached_mem.txt : vglUseCachedMem(TRUE) — vitaGL's internal pools use CACHED
     *     memory. CPU writes to GL pools (texture/vertex uploads) become much faster;
     *     vitaGL flushes caches before GPU reads. Risk: if the flush path misbehaves
     *     on this GXM build it corrupts rendering — hence opt-in. Must precede vglInit.
     *   gc_core3.txt : pin vitaGL's garbage-collector thread to core 3 (0x80000, freed
     *     by capUnlocker) so freeing deleted GPU resources doesn't steal render cycles.
     *     Risk: if core 3 isn't actually unlocked, thread create can fail -> opt-in. */
    /* DEFAULT-ON (2026-07-17): the workload is CPU-bound + upload-heavy, so cached GL
     * pools (faster CPU->GPU uploads) and moving vitaGL's GC off the engine's core are
     * both real wins; capUnlocker (which frees core 3) is a shipped prerequisite. Both
     * flip to opt-OUT via no_cached_mem.txt / no_gc_core3.txt if anything regresses. */
    { FILE *cm = mcsm_open_setting("no_cached_mem.txt", "r");
      if (cm) { fclose(cm); l_info("gl_init: vglUseCachedMem DISABLED (no_cached_mem.txt)"); }
      else { vglUseCachedMem(GL_TRUE); l_info("gl_init: vglUseCachedMem(TRUE) default-on — faster CPU uploads"); } }
    { FILE *gc = mcsm_open_setting("no_gc_core3.txt", "r");
      if (gc) { fclose(gc); l_info("gl_init: GC-on-core3 DISABLED (no_gc_core3.txt)"); }
      else { vglSetupGarbageCollector(160, 0x00080000); l_info("gl_init: vitaGL GC thread -> core3 default-on (frees engine core)"); } }
    vglInitExtended(0, RS_NATIVE_W, RS_NATIVE_H, ram_reserve_mb * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    /* vsync ON (helps a little) + a steady 30fps pacing cap in the game loop
     * (see hook_gameengine_loop / mcsm_pace_frame) is the real judder fix. Plain
     * vsync alone beats 17ms<->34ms (60<->30) because the GPU renders just under
     * 60fps and keeps missing vblank by a hair; the loop-level pace clamp forces a
     * consistent ~33ms delta so animation advances evenly = smooth motion. */
    /* Presenter: vsync ON by default gives a PRECISE vblank-locked rate (the
     * present-lock below pins it to an exact 30/60 with no beating). vsync OFF
     * (novsync.txt) stops the CPU blocking on vblank = highest throughput, but
     * timing then rides the sleep clock -> can judder. Tunable so the user can
     * pick fastest-throughput vs smoothest. */
    {
        FILE *nv = fopen("ux0:data/mcsm/novsync.txt", "r");
        if (nv) { fclose(nv); vglWaitVblankStart(GL_FALSE); l_info("presenter: vsync OFF (novsync.txt) — max throughput"); }
        else { vglWaitVblankStart(GL_TRUE); l_info("presenter: vsync ON (precise vblank lock)"); }
    }

    /* THE FIX (2026-06-22): shark_init returns 0 (libshacccg loads fine!), but
     * vitaGL's own startShaderCompiler uses vglMalloc — which fails — so it never
     * sets its is_shark_online flag and glCompileShader refuses to compile ANY
     * shader (even trivial, "(no info log)"). Initialize shark ourselves with the
     * SYSTEM allocator (malloc/free) — which works — and then force vitaGL's
     * global is_shark_online flag true so glCompileShader uses our loaded
     * compiler instead of re-running its failing vglMalloc init. */
    extern GLboolean is_shark_online;          /* vitaGL global (gxm.c) */
    shark_set_allocators(malloc, free);
    int sk = shark_init("ur0:data/external/libshacccg.suprx");
    if (sk < 0) sk = shark_init("ur0:data/libshacccg.suprx");
    l_info("gl_init: shark_init = 0x%08X (%d)", (unsigned)sk, sk);
    if (sk >= 0) {
        is_shark_online = GL_TRUE;
        /* ANTI-STUTTER 2026-06-30: the runtime shader compiler (ShaccCg) used a
         * heavy default opt level, so every new Telltale shader took 400-900ms to
         * compile mid-gameplay = the "mega stutter" (confirmed: each glLinkProgram
         * was immediately followed by a 400-900ms sim freeze). SHARK_OPT_SLOW is
         * O0 = MINIMAL optimization = FASTEST compile (the shaders run a hair
         * slower but compile in a fraction of the time). +fastmath/precision/int
         * speed it further. Must be set before any shader compiles. */
        vglSetupRuntimeShaderCompiler(SHARK_OPT_SLOW, GL_TRUE, GL_TRUE, GL_TRUE);
        l_info("gl_init: shader compiler -> SHARK_OPT_SLOW (fast compile, anti-stutter)");
        l_info("gl_init: forced is_shark_online=1 (runtime shader compiler up via malloc)");
    } else {
        l_warn("gl_init: shark_init FAILED (%d) — shaders will not compile", sk);
    }

    /* Only spin up the low-res FBO if the game's render res is BELOW native
     * (fb_override.txt set it). Otherwise leave the direct-to-display path alone. */
    if (render_w < RS_NATIVE_W || render_h < RS_NATIVE_H) {
        rs_init(render_w, render_h);
    }

    /* Loading-screen overlay: OFF BY DEFAULT (2026-07-03, user request — the
     * game's own transitions look cleaner without our extra overlay after the
     * Android logo). Opt IN by creating ux0:data/mcsm/loadscreen.txt. The old
     * noloadscreen.txt still force-disables (harmless now that off is default). */
    {
        FILE *on = fopen("ux0:data/mcsm/loadscreen.txt", "r");
        FILE *off = fopen("ux0:data/mcsm/noloadscreen.txt", "r");
        if (on && !off) {
            fclose(on);
            loading_screen_init(RS_NATIVE_W, RS_NATIVE_H);
            g_loadscreen_ready = loading_screen_is_ready();
            l_info("LS: loading-screen overlay ENABLED by loadscreen.txt ready=%d", g_loadscreen_ready);
        } else {
            if (on) fclose(on);
            if (off) fclose(off);
            l_info("LS: loading-screen overlay off by default");
        }
    }

    /* Present-side frame lock period from fps_cap.txt (read once, here, where
     * file I/O is safe — NOT in gl_swap). */
    {
        FILE *cf = mcsm_open_setting("fps_cap.txt", "r");
        if (cf) {
            int fps = 0;
            if (fscanf(cf, "%d", &fps) == 1 && fps > 0 && fps <= 120) {
                /* VBLANK-QUANTIZED PRESENT LOCK (2026-07-17): a plain 1000000/fps
                 * period lands the sleep-release right ON a vblank boundary; adaptive
                 * vsync then misses it by a hair and snaps to the NEXT vblank -> idle
                 * 30fps frames were beating to 50ms (20fps). Quantize to the nearest
                 * whole vblank count and UNDERSHOOT by ~2.5ms so the frame is always
                 * ready before the target vblank and vsync catches it cleanly. This
                 * makes fps_cap=30 (or 33) both resolve to a rock-steady 30fps. */
                const int vb = 16667;                 /* one 60Hz vblank (us) */
                int per = 1000000 / fps;
                int k = (per + vb / 2) / vb;           /* nearest whole vblanks */
                if (k < 1) k = 1;
                g_present_period_us = k * vb - 2500;
            }
            fclose(cf);
        }
        /* DE-STACK PACING opt-in (2026-07-17, ux0:data/mcsm/no_present_lock.txt):
         * with vsync ON there are THREE period gates at the same rate but
         * independent phase — sim-pace (mcsm_pace_frame), this present-lock, and
         * hw vsync — which can beat against each other and jitter frame time. The
         * sim-pace is the primary judder fix; dropping the present-lock leaves
         * vsync as the sole present clock. Kept OFF by default because the
         * present-lock also caps sub-16ms frames from presenting at 60 (its
         * original anti-60<->30 purpose); this can only be judged on-device, so
         * it's a toggle the user can A/B for the smoothest result. */
        { FILE *npl = mcsm_open_setting("no_present_lock.txt", "r");
          if (npl) { fclose(npl); g_present_period_us = 0;
                     l_info("present frame-lock DISABLED (no_present_lock.txt) — vsync+sim-pace only"); } }
        l_info("present frame-lock period = %d us (vblank-quantized)", g_present_period_us);
    }
}

void gl_swap() {
    static unsigned int s_swap_counter = 0;
    /* SAVE-RENAME KEYBOARD pump (2026-07-18): the Vita IME renders itself during
     * vglSwapBuffers; harvest the entered name when the user finishes + feed it
     * back to the engine as key events. Runs every present, near-free when idle. */
    { extern int mcsm_ime_poll(char **out); extern void mcsm_ime_deliver(const char *);
      char *ime_text = NULL;
      if (mcsm_ime_poll(&ime_text) && ime_text) mcsm_ime_deliver(ime_text); }
    /* Keep the Vita awake while the game runs. The idle/suspend timer has
     * second-level granularity, so ticking it ~1x/sec (every 30th present) fully
     * prevents auto-suspend/screen-blank while avoiding a wasted per-frame syscall
     * on the hottest path — a small but free battery win. */
    if ((s_swap_counter % 30u) == 0u) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }
    if (g_rs_active) {
        /* Bilinear-upscale the low-res render into the native display, THEN present.
         * draw FB is left at 0 so vglSwapBuffers presents the upscaled image.
         * Y is FLIPPED (dstY0/dstY1 swapped): vitaGL flips for the display when
         * presenting FB 0 directly, but an FBO->FB0 blit does not, so without this
         * the upscaled image comes out upside-down. */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rs_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, g_rs_w, g_rs_h, 0, RS_NATIVE_H, RS_NATIVE_W, 0,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    /* PRESENT-SIDE FRAME LOCK (anti-stutter 2026-06-30): the render thread
     * presents independently of the sim, so on a heavy 3D scene frames land at
     * 18-25ms and vsync snaps each to either 16.7ms or 33ms -> the DISPLAY beats
     * between 60 and 30fps = persistent judder, even on a static camera (the sim
     * pace alone can't fix this — it doesn't gate the present). Enforce a steady
     * minimum present interval so the display rate is consistent (no beating).
     * Period = 1/fps_cap.txt (e.g. 30 -> 33.3ms). fps_cap 0/absent = off. */
    if (g_present_period_us > 0) {
        static uint64_t s_last_present_us = 0;
        uint64_t pnow = sceKernelGetSystemTimeWide();
        if (s_last_present_us) {
            uint64_t el = pnow - s_last_present_us;
            if (el < (uint64_t)g_present_period_us) {
                sceKernelDelayThread((SceUInt)((uint64_t)g_present_period_us - el));
            }
        }
        s_last_present_us = sceKernelGetSystemTimeWide();
    }
    launch_state_mark_gl_phase(1);   /* in vglSwapBuffers (present) */
    vglSwapBuffers(GL_FALSE);
    launch_state_mark_gl_phase(0);   /* present returned */
    if (g_rs_active) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_rs_fbo); /* next frame renders into the FBO again */
    }
    s_swap_counter++;

    /* DIP PROFILER: log only severe render stalls with the draw-call /
     * vertex load that frame, so a sustained stutter shows whether it's
     * draw/GPU-bound (high draws) or stalling elsewhere (low draws). Cheap:
     * fires only on dips, and the logger is buffered. */
    {
        static uint64_t s_last_us = 0;
        uint64_t now_us = sceKernelGetSystemTimeWide();
        if (s_last_us) {
            uint64_t dt_us = now_us - s_last_us;
            /* Publish the true present cadence for the clock governor (every frame,
             * all builds — NOT gated by logging). */
            g_mcsm_present_dt_us = (dt_us > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)dt_us;
            uint32_t dt_ms = (uint32_t)(dt_us / 1000ULL);
            /* DIAG: log gameplay DIPS (>40ms = <25fps), throttled 1-in-16 to avoid
             * self-slowing, WITH draws/verts + VRAM so we can read CPU-vs-GPU-vs-VRAM. */
            static unsigned s_dipc = 0;
            if (dt_ms > 40U && (s_dipc++ & 0xFU) == 0U) {
                l_info("DIP-RENDER frame=%u dt=%ums draws=%u verts=%lu VRAM=%uKB RAM=%uKB",
                       s_swap_counter, dt_ms, g_frame_draw_calls, g_frame_draw_verts,
                       (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024u));
            }
        }
        s_last_us = now_us;
        g_frame_draw_calls = 0;
        g_frame_draw_verts = 0;
    }
    // Record a real frame-present heartbeat so telemetry/watchdog can tell
    // "actually rendering" apart from "input loop ticking over a black screen".
    launch_state_mark_present();
    if (s_swap_counter <= 8 || (s_swap_counter & 0x3ffU) == 0U) {
        /* Log vitaGL's per-pool FREE memory so we can see EXACTLY which GPU pool
         * exhausts when the 3D diorama loads (textures are tiny ~20MB, so the
         * CDRAM/phycont stall is from vitaGL's vertex/uniform/param pools or
         * geometry). VRAM=CDRAM, RAM=USER_RW, SLOW=PHYCONT. */
        l_info("gl_swap count=%u VGLfree: VRAM=%uKB RAM=%uKB PHYCONT=%uKB",
               s_swap_counter,
               (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u),
               (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024u),
               (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024u));
    }
}
#endif /* !USE_PVR_PSP2 */

void glBindVertexArrayOES_soloader(GLuint array) {
#ifdef USE_PVR_PSP2
    glBindVertexArrayOES(array);
#else
    glBindVertexArray(array);
#endif
}

void glDeleteVertexArraysOES_soloader(GLsizei n, const GLuint *arrays) {
#ifdef USE_PVR_PSP2
    glDeleteVertexArraysOES(n, arrays);
#else
    glDeleteVertexArrays(n, arrays);
#endif
}

void glGenVertexArraysOES_soloader(GLsizei n, GLuint *arrays) {
#ifdef USE_PVR_PSP2
    glGenVertexArraysOES(n, arrays);
#else
    glGenVertexArrays(n, arrays);
#endif
}

void glDrawElementsInstancedEXT_soloader(GLenum mode, GLsizei count, GLenum type,
                                         const void *indices, GLsizei instancecount) {
#ifdef USE_PVR_PSP2
    glDrawElementsInstancedEXT(mode, count, type, indices, instancecount);
#else
    glDrawElementsInstanced(mode, count, type, indices, instancecount);
#endif
}

void glVertexAttribDivisorEXT_soloader(GLuint index, GLuint divisor) {
#ifdef USE_PVR_PSP2
    glVertexAttribDivisorEXT(index, divisor);
#else
    glVertexAttribDivisor(index, divisor);
#endif
}

static GLenum normalize_texture_internalformat(GLenum internalformat) {
    switch (internalformat) {
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
        case GL_BGRA_EXT:
            return GL_RGBA;
        case GL_RGB8:
            return GL_RGB;
        default:
            return internalformat;
    }
}

static int texture_upload_should_log(unsigned int count, GLenum err) {
#if MCSM_FAST_FINAL_RUNTIME
    (void)count;
    return err != GL_NO_ERROR;
#else
    return count <= 64U || err != GL_NO_ERROR || (count & 0x3ffU) == 0U;
#endif
}

static GLenum drain_gl_errors_limited(void) {
    GLenum first = GL_NO_ERROR;
    for (unsigned int i = 0; i < 8U; ++i) {
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            break;
        }
        if (first == GL_NO_ERROR) {
            first = err;
        }
    }
    return first;
}

static int gl_draw_diag_should_log(unsigned int count, GLenum pre_err, GLenum query_err, GLenum err) {
#if MCSM_FAST_FINAL_RUNTIME
    (void)count;
    return pre_err != GL_NO_ERROR ||
           query_err != GL_NO_ERROR ||
           err != GL_NO_ERROR;
#else
    return count <= 96U ||
           pre_err != GL_NO_ERROR ||
           query_err != GL_NO_ERROR ||
           err != GL_NO_ERROR ||
           (count & 0x3ffU) == 0U;
#endif
}

static GLint gl_get_int_for_diag(GLenum pname, GLenum *query_err) {
    GLint value = 0;
    glGetIntegerv(pname, &value);
    GLenum err = drain_gl_errors_limited();
    if (query_err && *query_err == GL_NO_ERROR) {
        *query_err = err;
    }
    return value;
}

#define GL_DIAG_TEX_UNIT_CAP 16

static GLenum g_diag_active_texture = GL_TEXTURE0;
static GLuint g_diag_bound_texture_2d[GL_DIAG_TEX_UNIT_CAP];

/* POT-awareness for the wrap fix. The blanket REPEAT->CLAMP clamp (see
 * clamp_repeat_wrap) keeps NPOT textures complete on GXM, but it also kills
 * legitimate TILING on power-of-two world textures -> stretched/garbage gameplay
 * surfaces. Record each texture's POT status at upload time so we can let POT
 * textures keep GL_REPEAT while still clamping NPOT (and unknown, to be safe).
 * 0 = unknown, 1 = POT, 2 = NPOT. Indexed by GL texture id. */
#define GL_TEX_POT_CAP 16384
static uint8_t g_tex_pot[GL_TEX_POT_CAP];

static int gl_is_pow2(GLsizei v) {
    return v > 0 && (v & (v - 1)) == 0;
}

static void gl_tex_mark_pot(GLuint id, GLsizei w, GLsizei h) {
    if (id == 0 || id >= GL_TEX_POT_CAP) {
        return;
    }
    g_tex_pot[id] = (gl_is_pow2(w) && gl_is_pow2(h)) ? 1u : 2u;
}

static int gl_tex_is_known_pot(GLuint id) {
    return id != 0 && id < GL_TEX_POT_CAP && g_tex_pot[id] == 1u;
}
/* Textures the engine asked to tile (REPEAT), recorded in clamp_repeat_wrap.
 * If a texture later proves POT, force_complete_filter restores REPEAT so POT
 * tiling surfaces don't stay CLAMP'd (= stretched) when WRAP was set BEFORE the
 * upload (POT unknown at that moment). */
static uint8_t g_tex_want_repeat[GL_TEX_POT_CAP];

static int gl_texture_unit_index(GLenum texture) {
    if (texture < GL_TEXTURE0) {
        return -1;
    }
    unsigned int idx = (unsigned int)(texture - GL_TEXTURE0);
    if (idx >= GL_DIAG_TEX_UNIT_CAP) {
        return -1;
    }
    return (int)idx;
}

static int gl_sampler_diag_should_log(unsigned int count, GLenum pre_err, GLenum err) {
#if MCSM_FAST_FINAL_RUNTIME
    (void)count;
    return pre_err != GL_NO_ERROR ||
           err != GL_NO_ERROR;
#else
    return count <= 64U ||
           pre_err != GL_NO_ERROR ||
           err != GL_NO_ERROR ||
           (count & 0x1fffU) == 0U;
#endif
}

void glActiveTexture_soloader(GLenum texture) {
    static unsigned int s_count = 0;
    s_count++;

#if MCSM_FAST_FINAL_RUNTIME
    glActiveTexture(texture);
    if (gl_texture_unit_index(texture) >= 0) {
        g_diag_active_texture = texture;
    }
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    glActiveTexture(texture);
    GLenum err = glGetError();

    if (err == GL_NO_ERROR && gl_texture_unit_index(texture) >= 0) {
        g_diag_active_texture = texture;
    }

    if (gl_sampler_diag_should_log(s_count, pre_err, err)) {
        l_info("glActiveTexture #%u unit=0x%X idx=%d pre=0x%X err=0x%X",
               s_count,
               (unsigned)texture,
               gl_texture_unit_index(texture),
               (unsigned)pre_err,
               (unsigned)err);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * Texture LRU eviction cap (2026-06-21).
 * The Telltale engine re-uploads textures repeatedly with fresh GL names and
 * NEVER calls glDeleteTextures, so GPU texture memory grows without bound ->
 * CDRAM exhausts -> glCompressedTexImage2D OOMs (err=0x505) -> the render
 * thread can't finish the frame -> RenderThread::FinishFrame (called from
 * ScenePreload) waits forever -> the game thread deadlocks -> black screen.
 * No streaming/caching path on Vita avoids this (proven across many builds:
 * OOM count only grows). So we bound it: track every texture's GPU bytes and,
 * before an upload that would exceed a budget, glDeleteTextures the least-
 * recently-BOUND textures (the abandoned duplicate/orphan copies the engine
 * never reuses). Textures currently bound to any unit are never evicted, so
 * the live working set is preserved. All map mutation is confined to the one
 * GL thread (the upload thread) to stay lock-free and safe.
 * ------------------------------------------------------------------------- */
#define TEXLRU_SLOTS  8192u
#ifdef USE_PVR_PSP2
#define TEXLRU_BUDGET (40u * 1024u * 1024u)  /* byte backstop */
#else
/* vitaGL 2026-06-29: the 40MB byte budget was STILL evicting on vitaGL even though
 * the texture-OBJECT cap (TEXLRU_MAXTEX) was disabled — gameplay needs >40MB of
 * textures, so the LRU glDeleteTextures'd in-use ones every few draws. Observed:
 * 2901 compressed uploads for ~362 distinct textures (~8x re-upload thrash) ->
 * (a) deleted-but-still-used textures sampled as GARBAGE ("colorful blocks in
 * random places") and (b) constant PVRTC re-decode/re-upload throttled the 3D
 * scene to ~10fps while small resident UI textures stayed smooth. vitaGL has no
 * texture-object limit and VRAM has headroom (85MB free), so disable the budget
 * (huge value -> never evicts). OOM is still handled by the 1x1 placeholder. */
#define TEXLRU_BUDGET (1024u * 1024u * 1024u)
#endif
/* The PVR driver dies DETERMINISTICALLY at ~the 242nd live texture object
 * (err=0x505 regardless of free memory — 52MB CDRAM free; raising ULT 256->2048
 * didn't move it). It's a hard texture-OBJECT count limit. So cap the number of
 * live texture objects well under it by glDeleteTextures-ing the least-recently-
 * bound ones (freeing driver slots). 160 leaves wide margin and is far above the
 * per-frame working set. */
#ifdef USE_PVR_PSP2
#define TEXLRU_MAXTEX 180u  /* PVR_PSP2 has a ~220-260 texture-OBJECT ceiling; cap live objects under it. */
#else
#define TEXLRU_MAXTEX 1000000u  /* vitaGL has NO texture-object limit — eviction is unnecessary AND harmful (glDeleteTextures-ing textures the engine still uses crashes during scene loads). Effectively disabled. */
#endif
typedef struct { GLuint id; GLuint bytes; uint32_t use; } texlru_ent;
static texlru_ent s_texlru[TEXLRU_SLOTS];
static uint32_t   s_texlru_total = 0;
static uint32_t   s_texlru_count = 0;
static uint32_t   s_texlru_clock = 0;
static uint32_t   s_texlru_evicted = 0;
/* The engine's intended upload target, captured from the glBindTexture ARGUMENT.
 * We cannot trust GL_TEXTURE_BINDING_2D: under pressure PVR's bind fails and the
 * query returns 0. The bind argument is the real id about to be uploaded to.
 * NOTE: no thread guard — GL is single-context (serialized across threads via
 * eglMakeCurrent), so map access never truly overlaps. The old on_gl_thread
 * guard rejected the upload thread and made the whole cap a silent no-op. */
static GLuint     s_texlru_bound = 0;

static texlru_ent *texlru_lookup(GLuint id, int insert) {
    uint32_t h = (id * 2654435761u) & (TEXLRU_SLOTS - 1u);
    for (uint32_t i = 0; i < TEXLRU_SLOTS; i++) {
        uint32_t s = (h + i) & (TEXLRU_SLOTS - 1u);
        if (s_texlru[s].bytes && s_texlru[s].id == id) return &s_texlru[s];
        if (s_texlru[s].bytes == 0) {
            if (!insert) return NULL;
            s_texlru[s].id = id; s_texlru[s].use = 0; return &s_texlru[s];
        }
    }
    return NULL;
}
static int texlru_is_live(GLuint id) {
    if (!id) return 1;
    for (int u = 0; u < GL_DIAG_TEX_UNIT_CAP; u++)
        if (g_diag_bound_texture_2d[u] == id) return 1;
    return 0;
}
static void texlru_touch(GLuint id) {
    /* vitaGL (TEXLRU_MAXTEX==1000000) never evicts, so the per-bind LRU hash lookup
     * is pure dead weight — skip it. s_texlru_bound is set separately in glBindTexture,
     * so this doesn't affect the bound-id tracking. Constant-folds out on PVR. */
    if (!id || TEXLRU_MAXTEX >= 1000000u) return;
    texlru_ent *e = texlru_lookup(id, 0);
    if (e) e->use = ++s_texlru_clock;
}
static void texlru_make_room(GLuint need, GLuint keep_id) {
    int did_evict = 0;
    while (s_texlru_count >= TEXLRU_MAXTEX || s_texlru_total + need > TEXLRU_BUDGET) {
        texlru_ent *victim = NULL;
        for (uint32_t i = 0; i < TEXLRU_SLOTS; i++) {
            texlru_ent *e = &s_texlru[i];
            if (!e->bytes || e->id == keep_id || texlru_is_live(e->id)) continue;
            if (!victim || e->use < victim->use) victim = e;
        }
        if (!victim) break;                 /* only live textures remain */
        GLuint vid = victim->id;
        glDeleteTextures(1, &vid);
        (void)glGetError();
        s_texlru_total -= victim->bytes;
        s_texlru_count--;
        did_evict = 1;
        if (++s_texlru_evicted <= 12u || (s_texlru_evicted & 0xFFu) == 0u)
            l_info("TEXLRU: evicted #%u tex=%u (live count=%u total=%uKB)",
                   s_texlru_evicted, vid, s_texlru_count, s_texlru_total / 1024u);
        victim->id = 0; victim->bytes = 0; victim->use = 0;
    }
    /* PVR DEFERS the texture-object free until the GPU drains — glFlush wasn't
     * enough (residual err=0x505 unchanged). glFinish blocks until the GPU is
     * idle, which actually reclaims the deleted object slots before the next
     * uploads. Slow, but only fires while we're over the cap during a load burst
     * (a one-time cost), and correctness beats speed here. */
    if (did_evict) glFinish();
}
static void texlru_record(GLuint id, GLuint bytes) {
    /* vitaGL: no eviction -> the table is never read, so skip the per-upload hash
     * insert entirely (~2900/scene load). Constant-folds out on PVR (MAXTEX=180). */
    if (!id || TEXLRU_MAXTEX >= 1000000u) return;
    if (bytes == 0) bytes = 1;               /* keep slot nonzero = live */
    texlru_ent *e = texlru_lookup(id, 1);
    if (!e) return;                          /* table full: skip (rare) */
    if (e->bytes) s_texlru_total -= e->bytes; /* replacing a prior upload */
    else          s_texlru_count++;          /* brand-new texture object */
    e->bytes = bytes; e->use = ++s_texlru_clock;
    s_texlru_total += bytes;
}
/* Before an upload: free LRU non-live texture objects so we stay under the
 * driver's object-count limit (and byte backstop). */
static void texlru_before_upload(GLint bound_tex, GLsizei bytes) {
    texlru_make_room((GLuint)(bytes > 0 ? bytes : 1), (GLuint)(bound_tex > 0 ? bound_tex : 0));
}
static void texlru_after_upload(GLint bound_tex, GLsizei bytes, GLenum err) {
    if (bound_tex <= 0 || err != GL_NO_ERROR) return;
    texlru_record((GLuint)bound_tex, (GLuint)(bytes > 0 ? bytes : 1));
}

void glBindTexture_soloader(GLenum target, GLuint texture) {
    static unsigned int s_count = 0;
    s_count++;

    const int active_idx = gl_texture_unit_index(g_diag_active_texture);
#if MCSM_FAST_FINAL_RUNTIME
    glBindTexture(target, texture);
    if (target == GL_TEXTURE_2D) {
        s_texlru_bound = texture;
        texlru_touch(texture);
        if (active_idx >= 0) {
            g_diag_bound_texture_2d[active_idx] = texture;
        }
    }
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    glBindTexture(target, texture);
    GLenum err = glGetError();

    if (target == GL_TEXTURE_2D) {
        /* Capture the intended target unconditionally (the GL bind may report a
         * stale OOM error but the engine still means to upload to `texture`). */
        s_texlru_bound = texture;
        texlru_touch(texture);
        if (err == GL_NO_ERROR && active_idx >= 0)
            g_diag_bound_texture_2d[active_idx] = texture;
    }

    if (gl_sampler_diag_should_log(s_count, pre_err, err)) {
        l_info("glBindTexture #%u target=0x%X texture=%u active=0x%X idx=%d pre=0x%X err=0x%X",
               s_count,
               (unsigned)target,
               texture,
               (unsigned)g_diag_active_texture,
               active_idx,
               (unsigned)pre_err,
               (unsigned)err);
    }
#endif
}

void glUseProgram_soloader(GLuint program) {
    static unsigned int s_count = 0;
    s_count++;

#if MCSM_FAST_FINAL_RUNTIME
    glUseProgram(program);
    g_uniform_current_program = program;
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    glUseProgram(program);
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) {
        g_uniform_current_program = program;
    }

    /* Don't log on pre_err: a benign persistent GL_INVALID_ENUM lingers every
     * frame (an unsupported cap somewhere), which made this fire ~4777x/run. The
     * first 64 + any error THIS call produces is plenty. */
    if (s_count <= 64U || err != GL_NO_ERROR) {
        l_info("glUseProgram #%u program=%u pre=0x%X err=0x%X",
               s_count,
               program,
               (unsigned)pre_err,
               (unsigned)err);
    }
#endif
}

void glVertexAttribPointer_soloader(GLuint index, GLint size, GLenum type,
                                    GLboolean normalized, GLsizei stride,
                                    const void *pointer) {
    static unsigned int s_count = 0;
    s_count++;

#if MCSM_FAST_FINAL_RUNTIME
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;
    GLint program = gl_get_int_for_diag(GL_CURRENT_PROGRAM, &query_err);
    GLint array_buffer = gl_get_int_for_diag(GL_ARRAY_BUFFER_BINDING, &query_err);

    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    GLenum err = glGetError();

    if (gl_draw_diag_should_log(s_count, pre_err, query_err, err)) {
        l_info("glVertexAttribPointer #%u idx=%u size=%d type=0x%X norm=%u stride=%d ptr=%p program=%d array_buf=%d pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               index,
               size,
               (unsigned)type,
               (unsigned)normalized,
               stride,
               pointer,
               program,
               array_buffer,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }
#endif
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    static unsigned int s_count = 0;
    s_count++;
    g_frame_draw_calls++; g_frame_draw_verts += (unsigned long)count;

#if MCSM_FAST_FINAL_RUNTIME
    launch_state_mark_draw((unsigned)mode, (int)count, 0u, (int)g_uniform_current_program);
    glDrawArrays(mode, first, count);
    /* per-draw mark_gl_phase(0) dropped (2026-07-17): the present already refreshes
     * the watchdog phase/timestamp each frame; skip 2 syscalls/draw. */
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;
    GLint program = gl_get_int_for_diag(GL_CURRENT_PROGRAM, &query_err);
    GLint array_buffer = gl_get_int_for_diag(GL_ARRAY_BUFFER_BINDING, &query_err);
    GLint framebuffer = gl_get_int_for_diag(GL_FRAMEBUFFER_BINDING, &query_err);

    launch_state_mark_draw((unsigned)mode, (int)count, 0u, program);
    glDrawArrays(mode, first, count);
    launch_state_mark_gl_phase(0);
    GLenum err = glGetError();

    if (gl_draw_diag_should_log(s_count, pre_err, query_err, err)) {
        l_info("glDrawArrays #%u mode=0x%X first=%d count=%d program=%d array_buf=%d fb=%d pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               (unsigned)mode,
               first,
               count,
               program,
               array_buffer,
               framebuffer,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }
#endif
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    static unsigned int s_count = 0;
    s_count++;
    g_frame_draw_calls++; g_frame_draw_verts += (unsigned long)count;

#if MCSM_FAST_FINAL_RUNTIME
    launch_state_mark_draw((unsigned)mode, (int)count, (unsigned)type, (int)g_uniform_current_program);
    glDrawElements(mode, count, type, indices);
    /* per-draw mark_gl_phase(0) dropped (2026-07-17): present refreshes the watchdog
     * phase/timestamp each frame; skip 2 syscalls/draw. */
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;
    GLint program = gl_get_int_for_diag(GL_CURRENT_PROGRAM, &query_err);
    GLint array_buffer = gl_get_int_for_diag(GL_ARRAY_BUFFER_BINDING, &query_err);
    GLint element_buffer = gl_get_int_for_diag(GL_ELEMENT_ARRAY_BUFFER_BINDING, &query_err);
    GLint framebuffer = gl_get_int_for_diag(GL_FRAMEBUFFER_BINDING, &query_err);

    launch_state_mark_draw((unsigned)mode, (int)count, (unsigned)type, program);
    glDrawElements(mode, count, type, indices);
    launch_state_mark_gl_phase(0);
    GLenum err = glGetError();

    if (gl_draw_diag_should_log(s_count, pre_err, query_err, err)) {
        l_info("glDrawElements #%u mode=0x%X count=%d type=0x%X indices=%p program=%d array_buf=%d elem_buf=%d fb=%d pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               (unsigned)mode,
               count,
               (unsigned)type,
               indices,
               program,
               array_buffer,
               element_buffer,
               framebuffer,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }
#endif
}

/* Redundant-state dedup (deep-dive 2026-07-20): vitaGL's glEnable/glDisable are NOT
 * deferred — each call immediately issues GXM state and has NO redundancy guard
 * except GL_BLEND. glEnable(GL_DEPTH_TEST) runs change_depth_func (2x sceGxmSet*
 * DepthFunc + depth-write); glEnable(GL_CULL_FACE) runs sceGxmSetCullMode. A
 * Telltale engine that re-asserts depth/cull per batch pays that for every no-op
 * across ~900 draws/frame. Shadow-track ONLY these two caps (pure GXM state, no
 * FFP/texture/shader side effects) and skip no-op transitions; every other cap
 * forwards unchanged. Single-threaded, like the rest of GL state and the uniform
 * split memo. The dependent setters (glDepthFunc/glDepthMask/glCullFace/glFrontFace)
 * self-apply while enabled, so a skipped no-op enable/disable is a true no-op. */
static signed char g_cap_depth_test = -1;  /* -1 unknown, 0 disabled, 1 enabled */
static signed char g_cap_cull_face  = -1;
/* Safety valve (untestable on-device this pass): if the dedup ever desyncs the
 * shadow from GXM depth/cull state (visible as missing/inverted geometry), drop a
 * file ux0:data/mcsm/no_state_dedup.txt to forward every call unconditionally —
 * no rebuild needed. Read once, cached. Default ON. */
static int state_dedup_enabled(void) {
    static int v = -1;
    if (v < 0) { FILE *f = mcsm_open_setting("no_state_dedup.txt", "r");
                 if (f) { fclose(f); v = 0; l_info("state-dedup DISABLED (no_state_dedup.txt)"); } else v = 1; }
    return v;
}
void glEnable_soloader(GLenum cap) {
    if (state_dedup_enabled()) {
        if (cap == 0x0B71 /*GL_DEPTH_TEST*/) { if (g_cap_depth_test == 1) return; g_cap_depth_test = 1; }
        else if (cap == 0x0B44 /*GL_CULL_FACE*/) { if (g_cap_cull_face == 1) return; g_cap_cull_face = 1; }
    }
    glEnable(cap);
}
void glDisable_soloader(GLenum cap) {
    if (state_dedup_enabled()) {
        if (cap == 0x0B71 /*GL_DEPTH_TEST*/) { if (g_cap_depth_test == 0) return; g_cap_depth_test = 0; }
        else if (cap == 0x0B44 /*GL_CULL_FACE*/) { if (g_cap_cull_face == 0) return; g_cap_cull_face = 0; }
    }
    glDisable(cap);
}

/* Phase markers (5=finish 6=flush) so a render-thread wedge in a blocking GPU-sync
 * call shows up in the watchdog snapshot (glphase). glClear is non-blocking (mapped
 * straight to vitaGL), so it carries no marker. */
void glFinish_soloader(void) {
    launch_state_mark_gl_phase(5);
    glFinish();
    launch_state_mark_gl_phase(0);
}

void glFlush_soloader(void) {
    launch_state_mark_gl_phase(6);
    glFlush();
    launch_state_mark_gl_phase(0);
}

/* PERF (2026-07-17): mirror the engine's UNPACK_ALIGNMENT instead of querying
 * GL on every upload. The old push/pop did glGetIntegerv + up to ~17 glGetError
 * round-trips PER glTexImage2D/glTexSubImage2D (~2900 uploads/scene load = a big
 * slice of the load-hitch GL chatter). All engine uploads route through the
 * soloader wrappers and set alignment via glPixelStorei, so a tracked mirror is
 * exact. glPixelStorei with a valid alignment never raises GL_ERROR, so no drain
 * is needed. Default 4 = GL spec default (matches the old fallback). */
static GLint g_unpack_alignment = 4;

void glPixelStorei_soloader(GLenum pname, GLint param) {
    if (pname == 0x0CF5 /*GL_UNPACK_ALIGNMENT*/) {
        g_unpack_alignment = param;
    }
    glPixelStorei(pname, param);
}

static GLint push_unpack_alignment_one(void) {
    GLint old_align = g_unpack_alignment;
    if (old_align != 1) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    return old_align;
}

static void pop_unpack_alignment(GLint old_align) {
    if (old_align > 0 && old_align != 1) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, old_align);
    }
}

static uint8_t *convert_bgra_to_rgba(const uint8_t *src, GLsizei width, GLsizei height) {
    if (!src || width <= 0 || height <= 0) {
        return NULL;
    }

    const size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > (SIZE_MAX / 4U)) {
        return NULL;
    }

    uint8_t *dst = malloc(pixel_count * 4U);
    if (!dst) {
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t o = i * 4U;
        dst[o + 0] = src[o + 2];
        dst[o + 1] = src[o + 1];
        dst[o + 2] = src[o + 0];
        dst[o + 3] = src[o + 3];
    }

    return dst;
}

static uint8_t *convert_rgba4444_to_rgba8888(const uint16_t *src, GLsizei width, GLsizei height) {
    if (!src || width <= 0 || height <= 0) {
        return NULL;
    }

    const size_t w = (size_t)width;
    const size_t h = (size_t)height;
    if (w > (SIZE_MAX / h)) {
        return NULL;
    }

    const size_t pixel_count = w * h;
    if (pixel_count > (SIZE_MAX / 4U)) {
        return NULL;
    }

    uint8_t *dst = malloc(pixel_count * 4U);
    if (!dst) {
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t v = src[i];
        const uint8_t r = (uint8_t)((v >> 12) & 0x0FU);
        const uint8_t g = (uint8_t)((v >> 8) & 0x0FU);
        const uint8_t b = (uint8_t)((v >> 4) & 0x0FU);
        const uint8_t a = (uint8_t)(v & 0x0FU);
        const size_t o = i * 4U;
        dst[o + 0] = (uint8_t)((r << 4) | r);
        dst[o + 1] = (uint8_t)((g << 4) | g);
        dst[o + 2] = (uint8_t)((b << 4) | b);
        dst[o + 3] = (uint8_t)((a << 4) | a);
    }

    return dst;
}

static int rgba8888_byte_count(GLsizei width, GLsizei height, size_t *out_bytes) {
    if (!out_bytes || width <= 0 || height <= 0) {
        return 0;
    }

    const size_t w = (size_t)width;
    const size_t h = (size_t)height;
    if (w > (SIZE_MAX / h)) {
        return 0;
    }

    const size_t pixel_count = w * h;
    if (pixel_count > (SIZE_MAX / 4U)) {
        return 0;
    }

    *out_bytes = pixel_count * 4U;
    return 1;
}

static uint8_t *alloc_zero_rgba8888(GLsizei width, GLsizei height) {
    size_t byte_count = 0;
    if (!rgba8888_byte_count(width, height, &byte_count)) {
        return NULL;
    }

    uint8_t *dst = malloc(byte_count);
    if (!dst) {
        return NULL;
    }

    memset(dst, 0, byte_count);
    return dst;
}

static uint8_t *convert_rgb565_to_rgba8888(const uint16_t *src, GLsizei width, GLsizei height) {
    if (!src || width <= 0 || height <= 0) {
        return NULL;
    }

    size_t byte_count = 0;
    if (!rgba8888_byte_count(width, height, &byte_count)) {
        return NULL;
    }

    const size_t pixel_count = byte_count / 4U;
    uint8_t *dst = malloc(byte_count);
    if (!dst) {
        return NULL;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        const uint16_t v = src[i];
        const uint8_t r = (uint8_t)((v >> 11) & 0x1FU);
        const uint8_t g = (uint8_t)((v >> 5) & 0x3FU);
        const uint8_t b = (uint8_t)(v & 0x1FU);
        const size_t o = i * 4U;
        dst[o + 0] = (uint8_t)((r << 3) | (r >> 2));
        dst[o + 1] = (uint8_t)((g << 2) | (g >> 4));
        dst[o + 2] = (uint8_t)((b << 3) | (b >> 2));
        dst[o + 3] = 0xFFU;
    }

    return dst;
}

/* The game uploads only level 0 and never calls glGenerateMipmap, yet it sets
 * mipmapping min-filters on textures. A texture with a mipmap min-filter but no
 * mip levels is INCOMPLETE and samples as solid black/flat colour — exactly the
 * "backgrounds are just colours" symptom. Downgrade mipmap min-filters to their
 * non-mipmap base so single-level textures stay complete and sample correctly. */
static GLint demipmap_min_filter(GLenum pname, GLint param) {
    if (pname != 0x2801 /*GL_TEXTURE_MIN_FILTER*/) return param;
    switch (param) {
        case 0x2700: /*NEAREST_MIPMAP_NEAREST*/
        case 0x2702: /*NEAREST_MIPMAP_LINEAR */ return 0x2600; /*GL_NEAREST*/
        case 0x2701: /*LINEAR_MIPMAP_NEAREST */
        case 0x2703: /*LINEAR_MIPMAP_LINEAR  */ return 0x2601; /*GL_LINEAR*/
        default: return param;
    }
}

/* DIAGNOSTIC (2026-06-21): log the texture filter/wrap the game sets so we can
 * see why uploaded textures still render flat/wrong — e.g. a mipmap min-filter
 * on a now-mip-less texture (incomplete -> flat), or GL_REPEAT wrap on an NPOT
 * texture (incomplete on SGX). pname 0x2800=MAG 0x2801=MIN 0x2802=WRAP_S
 * 0x2803=WRAP_T; param 0x2600=NEAREST 0x2601=LINEAR 0x2700-0x2703=mip modes
 * 0x2901=REPEAT 0x812F=CLAMP_TO_EDGE. */
static void log_tex_param(GLenum target, GLenum pname, GLint param, GLint applied) {
    static unsigned int s_tp = 0;
    if (gl_verbose_diag_enabled() && (s_tp++ < 96U || (s_tp & 0x3FFU) == 0U)) {
        l_info("glTexParameter target=0x%X pname=0x%X param=0x%X->0x%X",
               (unsigned)target, (unsigned)pname, (unsigned)param, (unsigned)applied);
    }
}

/* GAME-WIDE FIX (2026-06-21): PowerVR SGX makes a NON-power-of-two texture with
 * GL_REPEAT wrap INCOMPLETE -> it samples as solid black. The game sets WRAP_S/T
 * = GL_REPEAT on many textures (UI atlases, character maps) which are NPOT, so
 * they render flat/untextured everywhere. We can't cheaply know POT/NPOT here,
 * so clamp EVERY GL_REPEAT to GL_CLAMP_TO_EDGE — that keeps all textures
 * complete (menu/UI/character art don't tile; at worst an edge texel repeats
 * instead of black, which is the right trade to actually SHOW textures). */
static GLuint gl_currently_bound_texture_2d(void) {
    int unit = gl_texture_unit_index(g_diag_active_texture);
    if (unit < 0) {
        return 0;
    }
    return g_diag_bound_texture_2d[unit];
}

/* POT-AWARE 2026-06-29: clamp REPEAT->CLAMP only for NPOT (and unknown)
 * textures, which GXM needs. Power-of-two textures tile fine on GXM, so let
 * them keep GL_REPEAT — otherwise every tiling world surface in gameplay gets
 * stretched/garbled. */
static GLint clamp_repeat_wrap(GLenum pname, GLint param) {
    if (pname == 0x2802 /*WRAP_S*/ || pname == 0x2803 /*WRAP_T*/) {
        GLuint wid = gl_currently_bound_texture_2d();
        if (param == 0x2901 /*GL_REPEAT*/) {
            if (wid && wid < GL_TEX_POT_CAP) g_tex_want_repeat[wid] = 1u;
            if (gl_tex_is_known_pot(wid)) {
                return param; /* POT: tiling is safe, keep REPEAT */
            }
            return 0x812F; /*GL_CLAMP_TO_EDGE*/
        }
        /* Engine explicitly set a NON-REPEAT wrap (e.g. CLAMP_TO_EDGE): clear the
         * "wants repeat" flag so force_complete_filter's elongation-fix does NOT
         * later re-force REPEAT and override the engine's explicit CLAMP. */
        if (wid && wid < GL_TEX_POT_CAP) g_tex_want_repeat[wid] = 0u;
    }
    return param;
}

void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param) {
    GLint applied = clamp_repeat_wrap(pname, demipmap_min_filter(pname, param));
    log_tex_param(target, pname, param, applied);
    glTexParameteri(target, pname, applied);
}

void glTexParameterf_soloader(GLenum target, GLenum pname, GLfloat param) {
    GLint applied = clamp_repeat_wrap(pname, demipmap_min_filter(pname, (GLint)param));
    log_tex_param(target, pname, (GLint)param, applied);
    glTexParameterf(target, pname, (GLfloat)applied);
}

void glTexParameteriv_soloader(GLenum target, GLenum pname, const GLint *params) {
    if (pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/ && params) {
        const GLint param = demipmap_min_filter(pname, params[0]);
        glTexParameteri(target, pname, param);
        return;
    }
    if (params) glTexParameteri(target, pname, params[0]);
}

void glTexParameterfv_soloader(GLenum target, GLenum pname, const GLfloat *params) {
    if (pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/ && params) {
        const GLfloat param = (GLfloat)demipmap_min_filter(pname, (GLint)params[0]);
        glTexParameterf(target, pname, param);
        return;
    }
    /* All GLES2 texture parameters are scalar (no vector params like BORDER_COLOR
     * exist on Vita), so route the fallback through the scalar setter. Avoids the
     * implicitly-declared glTexParameterfv (no vitaGL prototype -> unchecked ABI). */
    if (params) glTexParameterf(target, pname, params[0]);
}

void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param) {
    /* glTexParameterx is a no-op stub in pvr_gles_stubs.c. Route through
     * glTexParameteri (a real PVR GLES2 function) with the demipmapped
     * value so texture parameters are actually applied to the driver. */
    glTexParameteri(target, pname, demipmap_min_filter(pname, (GLint)param));
}

void glTexParameterxv_soloader(GLenum target, GLenum pname, const GLfixed *params) {
    if (pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/ && params) {
        const GLint param = demipmap_min_filter(pname, (GLint)params[0]);
        glTexParameteri(target, pname, param);
        return;
    }
    /* For non-min-filter params, convert fixed-point to int and use glTexParameteri */
    if (params) {
        glTexParameteri(target, pname, (GLint)params[0]);
    }
}

/* Telltale uploads single-level textures and never sets a min filter, so they
 * keep the GL default (NEAREST_MIPMAP_LINEAR) which makes a mipmap-less texture
 * INCOMPLETE -> GLES2 samples it as a flat color. Force a non-mipmap filter
 * right after upload so every texture is complete. Only touch GL_TEXTURE_2D and
 * swallow our own error so the engine's glGetError() sees a clean state. */
/* Telltale uploads only level 0 textures and the GL default min filter is
 * GL_NEAREST_MIPMAP_LINEAR, which makes single-level textures INCOMPLETE on
 * PowerVR — they sample as flat colors instead of the actual image. Fix only
 * the min filter after upload. On PVR we glFlush first so queued draw commands
 * with the old texture state finish before we change the filter. */
/* WHITE-LINES-BETWEEN-BLOCKS FIX (2026-07-20): the 3D world (compressed PVRTC/ETC1
 * textures) is Minecraft-style tile art that must sample GL_NEAREST — forcing
 * GL_LINEAR bilinearly interpolates across atlas/tile edges and shows bright/white
 * seams (a BASE-level bleed, so mipmaps-off can't fix it). Opt-in nearest_filter.txt
 * flips world textures to NEAREST (leaves UI/2D uncompressed textures on LINEAR so
 * fonts stay smooth). Default = unchanged. */
static int world_nearest_enabled(void) {
    static int v = -1;
    if (v < 0) { v = 0; FILE *f = mcsm_open_setting("nearest_filter.txt", "r"); if (f) { fclose(f); v = 1; } }
    return v;
}
static inline void force_complete_filter_ex(GLenum target, int allow_nearest) {
    if (target != 0x0DE1 /*GL_TEXTURE_2D*/) return;
    const int nearest = allow_nearest && world_nearest_enabled();
    glTexParameteri(target, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, nearest ? 0x2600 /*NEAREST*/ : 0x2601 /*LINEAR*/);
    if (nearest) glTexParameteri(target, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2600 /*NEAREST*/);
    /* ELONGATION FIX (2026-07-16): if the engine asked for WRAP=REPEAT before the
     * upload (POT unknown then -> clamp_repeat_wrap forced CLAMP = stretched
     * tiling), restore REPEAT now that the upload has proved the texture POT. */
    GLuint bt = (GLuint)s_texlru_bound;
    if (bt && bt < GL_TEX_POT_CAP && g_tex_want_repeat[bt] && g_tex_pot[bt] == 1u) {
        glTexParameteri(target, 0x2802 /*WRAP_S*/, 0x2901 /*GL_REPEAT*/);
        glTexParameteri(target, 0x2803 /*WRAP_T*/, 0x2901 /*GL_REPEAT*/);
    }
    (void)drain_gl_errors_limited();
}
/* Uncompressed (UI/2D) textures always keep LINEAR; only compressed world art may go NEAREST. */
static inline void force_complete_filter(GLenum target) { force_complete_filter_ex(target, 0); }

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void *data) {
    static unsigned int s_count = 0;
    s_count++;

    /* (Reverted the 512 cap 2026-06-21: its single-texture tracking broke on
     * interleaved uploads -> dropped a background texture's promoted base level
     * -> that layer went black. And texture data is tiny anyway (~10MB total:
     * mostly 256x256). The real OOM is render-target/CPU-heap memory, not
     * texture size.) Keep the harmless mip-level>0 drop only — MIN_FILTER is
     * pinned LINEAR so level 0 alone stays complete. */
    if (level > 0) {
        return;
    }

    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;   /* kept for the diagnostic logs below */
    /* Use the already-tracked bound id instead of querying GL_TEXTURE_BINDING_2D
     * (a glGetIntegerv + up to 8 glGetError drains) on EVERY compressed upload
     * (~2900/scene load) — matches glTexImage2D_soloader's path. 2026-07-17. */
    GLint bound_texture = (GLint)s_texlru_bound;
    gl_tex_mark_pot((GLuint)bound_texture, width, height);
    mcsm_scene_load_tick(); /* keep the loading screen alive during scene loads */
    uint8_t *zero_compressed = NULL;
    const void *upload_data = data;
    int zero_upload = 0;

    if (!data && imageSize > 0) {
        zero_compressed = malloc((size_t)imageSize);
        if (zero_compressed) {
            memset(zero_compressed, 0, (size_t)imageSize);
            upload_data = zero_compressed;
            zero_upload = 1;
        } else {
            l_warn("glCompressedTexImage2D: failed to allocate %d zero bytes for fmt=0x%X size=%dx%d.",
                   imageSize,
                   (unsigned)internalformat,
                   width,
                   height);
        }
    }

    texlru_before_upload((GLint)s_texlru_bound, imageSize);
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, upload_data);
    GLenum err = glGetError();
    texlru_after_upload((GLint)s_texlru_bound, imageSize, err);
    /* OOM FALLBACK (2026-06-22): when the compressed upload won't fit (PVR_PSP2
     * texture-object/pool ceiling — eviction gets close but a burst residual
     * remains), the texture is left empty/failed, and the engine's load-complete
     * wait (ScenePreload/FinishFrame) hangs forever on it. Instead define a 1x1
     * RGBA placeholder so the texture is VALID: it samples as a flat colour, but
     * the loader STOPS WAITING and the menu renders instead of stalling. */
    if (err == 0x0505 /* GL_OUT_OF_MEMORY */ && level == 0 && target == 0x0DE1) {
        static const uint8_t ph_px[4] = { 0, 0, 0, 255 };
        (void)drain_gl_errors_limited();
        glTexImage2D(target, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ph_px);
        GLenum ph_err = glGetError();
        static unsigned int s_ph = 0;
        if (s_ph++ < 24U)
            l_info("glCompressedTexImage2D: OOM -> 1x1 placeholder (tex=%d ph_err=0x%X)", bound_texture, (unsigned)ph_err);
        err = ph_err;
    }
    force_complete_filter_ex(target, 1);   /* compressed = world art -> may use NEAREST (nearest_filter.txt) */

    if (texture_upload_should_log(s_count, err) ||
        pre_err != GL_NO_ERROR ||
        query_err != GL_NO_ERROR ||
        (zero_upload && gl_verbose_diag_enabled())) {
        l_info("glCompressedTexImage2D #%u target=0x%X tex=%d level=%d fmt=0x%X size=%dx%d border=%d bytes=%d data=%p upload=%p zero=%d pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               (unsigned)target,
               bound_texture,
               level,
               (unsigned)internalformat,
               width,
               height,
               border,
               imageSize,
               data,
               upload_data,
               zero_upload,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }

    free(zero_compressed);
}

void glCompressedTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                                        GLint yoffset, GLsizei width, GLsizei height,
                                        GLenum format, GLsizei imageSize,
                                        const void *data) {
    /* Mip levels >0 are dropped (see glCompressedTexImage2D_soloader); their
     * sub-image fills target a non-existent level, so skip them. */
    if (level > 0) {
        return;
    }
    if (xoffset == 0 && yoffset == 0) {
        glCompressedTexImage2D_soloader(target, level, format, width, height, 0, imageSize, data);
    }
}

/* Box-downsample an RGBA8888 image by 2x. Returns a malloc'd buffer (caller
 * frees) or NULL on failure. Used to shrink the menu's large 1024² RGBA art:
 * those are the dominant GPU consumer (~32MB of the ~47MB texture load) and the
 * engine samples with normalized UVs, so a half-res copy renders identically. */
static uint8_t *downsample_rgba8888_2x(const uint8_t *src, int w, int h) {
    int dw = w >> 1, dh = h >> 1;
    if (dw < 1 || dh < 1) return NULL;
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * (size_t)dh * 4u);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        const uint8_t *r0 = src + (size_t)(y * 2) * w * 4;
        const uint8_t *r1 = r0 + (size_t)w * 4;
        uint8_t *d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            const uint8_t *a = r0 + (size_t)(x * 2) * 4;
            const uint8_t *b = a + 4;
            const uint8_t *c = r1 + (size_t)(x * 2) * 4;
            const uint8_t *e = c + 4;
            d[0] = (uint8_t)(((int)a[0] + b[0] + c[0] + e[0]) >> 2);
            d[1] = (uint8_t)(((int)a[1] + b[1] + c[1] + e[1]) >> 2);
            d[2] = (uint8_t)(((int)a[2] + b[2] + c[2] + e[2]) >> 2);
            d[3] = (uint8_t)(((int)a[3] + b[3] + c[3] + e[3]) >> 2);
            d += 4;
        }
    }
    return dst;
}

/* Texture IDs whose storage we allocated at half-res, so glTexSubImage2D knows to
 * downsample its data and halve its region to match. Direct-indexed by texture id
 * (same convention/cap as g_tex_pot) so the mark can be CLEARED — a hash set could
 * only ever add, and a stale mark (id later re-specified at a non-downsampled size
 * or deleted+recycled) would make glTexSubImage2D halve sub-data into full-res
 * storage = corruption. Engine texture ids fit well under GL_TEX_POT_CAP (same as
 * the POT arrays). Single GL context -> no lock needed. */
static uint8_t g_tex_dsamp[GL_TEX_POT_CAP];
static void dsamp_mark(GLuint id) {
    if (id && id < GL_TEX_POT_CAP) g_tex_dsamp[id] = 1u;
}
static void dsamp_unmark(GLuint id) {
    if (id && id < GL_TEX_POT_CAP) g_tex_dsamp[id] = 0u;
}
static int dsamp_is(GLuint id) {
    return id != 0 && id < GL_TEX_POT_CAP && g_tex_dsamp[id] == 1u;
}

/* ---- Depth-stencil render-target shim (2026-06-23) ------------------------
 * In-game the engine creates GL_DEPTH_STENCIL (0x84F9) *textures* for its 3D
 * scene FBO depth attachment. vitaGL only supports depth as RENDERBUFFERS, so
 * those texture allocations fail (GL_INVALID_VALUE 0x501) and the scene FBO ends
 * up with NO depth buffer -> depth testing is off -> the ground/dirt mesh
 * overdraws the whole scene (looks like "dirt everywhere", no real 3D). Shim:
 * when a depth-stencil texture is allocated, create a matching GL_DEPTH24_STENCIL8
 * renderbuffer and remember texid->rbo; when that texture is later attached to an
 * FBO depth/stencil slot, attach the renderbuffer instead. Depth can no longer be
 * *sampled* (soft particles / DOF lose their depth source) but the depth TEST
 * works, which is what makes the 3D actually render. */
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_RENDERBUFFER_BINDING
#define GL_RENDERBUFFER_BINDING 0x8CA7
#endif

#define MCSM_DEPTHTEX_MAX 32
static struct { GLuint tex; GLuint rbo; } g_depthtex_map[MCSM_DEPTHTEX_MAX];

static GLuint depthtex_lookup(GLuint tex) {
    if (!tex) return 0;
    for (int i = 0; i < MCSM_DEPTHTEX_MAX; ++i) {
        if (g_depthtex_map[i].tex == tex) return g_depthtex_map[i].rbo;
    }
    return 0;
}

static GLuint depthtex_make(GLuint tex, GLsizei w, GLsizei h) {
    int slot = -1;
    for (int i = 0; i < MCSM_DEPTHTEX_MAX; ++i) {
        if (g_depthtex_map[i].tex == tex) { slot = i; break; }          /* reuse */
        if (slot < 0 && g_depthtex_map[i].tex == 0) slot = i;           /* first free */
    }
    if (slot < 0) return 0;
    GLuint rbo = g_depthtex_map[slot].rbo;
    GLint prev_rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    if (!rbo) glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prev_rb);
    g_depthtex_map[slot].tex = tex;
    g_depthtex_map[slot].rbo = rbo;
    return rbo;
}

/* Redirect depth/stencil-texture FBO attachments to the shadow renderbuffer. */
void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment,
                                     GLenum textarget, GLuint texture, GLint level) {
    GLuint rbo = depthtex_lookup(texture);
    if (rbo && (attachment == GL_DEPTH_STENCIL_ATTACHMENT ||
                attachment == GL_DEPTH_ATTACHMENT ||
                attachment == GL_STENCIL_ATTACHMENT)) {
        glFramebufferRenderbuffer(target, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        static unsigned int s = 0;
        if (s++ < 16U) {
            l_info("glFramebufferTexture2D: depth tex=%u att=0x%X -> renderbuffer %u",
                   (unsigned)texture, (unsigned)attachment, (unsigned)rbo);
        }
        return;
    }
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

/* Opt-in (ux0:data/mcsm/mipmaps.txt): build mip chains for POT RGBA textures.
 * The engine uploads only level 0, so without this distant surfaces sample the
 * full-res texel = shimmer + wasted GPU texture-cache bandwidth. Trilinear mips
 * are sharper at distance AND cheaper to sample. Opt-in because it adds ~33%
 * VRAM per texture (caught by the RAM fallback pool) and a little upload time. */
static int g_mipmap_gen = -1;
static int mipmap_gen_enabled(void) {
    if (g_mipmap_gen < 0) {
        FILE *f = mcsm_open_setting("mipmaps.txt", "r");
        g_mipmap_gen = f ? 1 : 0;
        if (f) fclose(f);
    }
    return g_mipmap_gen;
}
/* Only mipmap textures >= this dim. Small textures (UI, tiny props) barely
 * benefit from mips but still cost a glGenerateMipmap on every area load, which
 * adds to the load hitch. Default 512 = only big surface textures (walls/floors/
 * ground — the ones seen at distance where mips actually help) get the chain.
 * Tunable: mipmap_min.txt (1..4096). */
static int g_mipmap_min = -1;
static int mipmap_min_dim(void) {
    if (g_mipmap_min < 0) {
        g_mipmap_min = 1024;   /* VRAM was EXHAUSTED (free_cdram=0) — only mip the biggest textures (mips add 33% VRAM). */
        FILE *f = fopen("ux0:data/mcsm/mipmap_min.txt", "r");
        if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 4096) g_mipmap_min = v; fclose(f); }
    }
    return g_mipmap_min;
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels) {
    static unsigned int s_count = 0;
    s_count++;

    /* Depth-stencil textures aren't supported by vitaGL -> back them with a
     * renderbuffer so the scene FBO actually gets a depth buffer (see shim note). */
    if ((GLenum)internalformat == GL_DEPTH_STENCIL || format == GL_DEPTH_STENCIL) {
        GLuint rbo = depthtex_make((GLuint)s_texlru_bound, width, height);
        static unsigned int s = 0;
        if (s++ < 16U) {
            l_info("glTexImage2D: depth-stencil tex=%u %dx%d -> renderbuffer %u (vitaGL has no depth textures)",
                   (unsigned)s_texlru_bound, width, height, (unsigned)rbo);
        }
        return;
    }

    /* Drop mip levels >0 ONLY for textures we downsampled — their level 0 is now
     * 512² so a later 512² level 1 would be an invalid (over-large) mip. For
     * normal textures, KEEP their mips: dropping them here left the engine's
     * later glTexSubImage2D(level=N) targeting a non-existent level -> 795
     * GL_INVALID_OPERATION (0x502) errors that leaked driver texture-op slots
     * and ultimately caused the OOM. (MIN_FILTER is pinned LINEAR regardless.) */
    if (level > 0 && dsamp_is((GLuint)s_texlru_bound)) {
        return;
    }

    if (level == 0) {
        gl_tex_mark_pot((GLuint)s_texlru_bound, width, height);
    }
    mcsm_scene_load_tick(); /* keep the loading screen alive during scene loads */

    const GLenum orig_internalformat = (GLenum)internalformat;
    const GLenum orig_format = format;
    GLenum upload_internalformat = normalize_texture_internalformat(orig_internalformat);
    GLenum upload_format = format;
    GLenum upload_type = type;
    const void *upload_pixels = pixels;
    uint8_t *converted = NULL;
    int zero_storage = 0;

    /* PVR rejects some 16-bit Android texture storage/upload combinations.
     * Promote them to RGBA8888 before they reach the driver. */
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
        upload_internalformat = GL_RGBA;
        upload_format = GL_RGBA;
        if (pixels) {
            converted = convert_bgra_to_rgba((const uint8_t *)pixels, width, height);
            if (converted) {
                upload_pixels = converted;
            } else {
                l_warn("glTexImage2D BGRA conversion failed size=%dx%d; uploading as GL_RGBA with original pointer.", width, height);
            }
        }
    }
#ifdef USE_PVR_PSP2
    /* PVR-only: 16-bit 565/4444 must be promoted to RGBA8888. vitaGL/GXM accept
     * U5U6U5 and U4U4U4U4 natively, so for vitaGL we skip this entirely — saves
     * the per-texture CPU convert + halves memory/bandwidth, and avoids any
     * channel-order corruption on font/UI textures. */
    else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        upload_internalformat = GL_RGBA;
        upload_format = GL_RGBA;
        if (pixels) {
            upload_type = GL_UNSIGNED_BYTE;
            converted = convert_rgb565_to_rgba8888((const uint16_t *)pixels, width, height);
            if (converted) {
                upload_pixels = converted;
            } else {
                upload_internalformat = normalize_texture_internalformat(orig_internalformat);
                upload_format = orig_format;
                upload_type = type;
                l_warn("glTexImage2D RGB565 conversion failed size=%dx%d; uploading original 16-bit data.", width, height);
            }
        } else if (width > 0 && height > 0) {
            converted = alloc_zero_rgba8888(width, height);
            if (converted) {
                upload_type = GL_UNSIGNED_BYTE;
                upload_pixels = converted;
                zero_storage = 1;
            } else {
                upload_internalformat = normalize_texture_internalformat(orig_internalformat);
                upload_format = orig_format;
                upload_type = type;
                l_warn("glTexImage2D RGB565 zero-storage allocation failed size=%dx%d; trying driver storage path.", width, height);
            }
        }
    } else if (format == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4) {
        upload_internalformat = GL_RGBA;
        upload_format = GL_RGBA;
        if (pixels) {
            upload_type = GL_UNSIGNED_BYTE;
            converted = convert_rgba4444_to_rgba8888((const uint16_t *)pixels, width, height);
            if (converted) {
                upload_pixels = converted;
            } else {
                upload_type = type;
                l_warn("glTexImage2D RGBA4444 conversion failed size=%dx%d; uploading original 16-bit data.", width, height);
            }
        } else if (width > 0 && height > 0) {
            converted = alloc_zero_rgba8888(width, height);
            if (converted) {
                upload_type = GL_UNSIGNED_BYTE;
                upload_pixels = converted;
                zero_storage = 1;
            }
            if (!zero_storage) {
                l_warn("glTexImage2D RGBA4444 zero-storage allocation failed size=%dx%d; trying driver storage path.", width, height);
            }
        }
    }
#endif

    GLenum pre_err = drain_gl_errors_limited();
    /* Halve large RGBA8888 art (>=1024²) — the dominant GPU consumer. Normalized
     * UVs make the half-res copy render the same. These are allocated with
     * data=NULL then filled by glTexSubImage2D, so mark the texture id (dsamp) and
     * halve the allocation here; the sub-image path downsamples to match.
     * NOTE: lowering this to 512² to relieve CDRAM CORRUPTED rendering (black menu
     * + GPUCRASH) — 512² catches render-targets / mip-sampled 3D textures that
     * can't be safely halved. Keep 1024² (only large 2D art is halved). The CDRAM
     * exhaustion from the diorama must be solved another way (skip diorama / extra
     * memory), NOT by widening downsample.
     *
     * NOTE 2026-07-03: RAISED 1024->2048. Halving every 1024² texture is the
     * SOURCE of the blurry 3D look the user reported (wood grain / brick / mine
     * tracks smeared). VRAM keeps ~34MB headroom all session and we now hand
     * vitaGL a ~32MB RAM fallback pool (see ram_reserve_mb), so 1024² art can stay
     * full-res: it fits VRAM, and any overflow lands in RAM instead of corrupting
     * CDRAM (pool grown to ~48MB via ram_reserve_mb=48 to further cut black faces).
     * Only truly huge 2048²+ art (16MB each) is still halved. Tunable via
     * downsample_min.txt — set 1024 to restore old behavior if a scene regresses. */
    static int dsamp_min_dim = -1;   /* RAISED back to 2048 (2026-07-17): the deep-dive
        * proved heavy scenes are DRAW/geometry-bound (~900 draws/438k verts), NOT
        * VRAM- or texture-bandwidth-bound — the log shows 35MB+ free VRAM and the RAM
        * fallback pool essentially never used. So downsampling to 1024 was paying a
        * REAL sharpness cost (blurry 3D) for ZERO fps gain. 2048 keeps all <=1024 art
        * full-res and only halves truly huge 2048²+ textures. Now CACHED (read once,
        * not fopen'd on every one of ~2900 uploads per scene load). Tunable via
        * downsample_min.txt (512/1024=lower res, 4096=nothing halved). */
    if (dsamp_min_dim < 0) {
        dsamp_min_dim = 2048;
        FILE *df = fopen("ux0:data/mcsm/downsample_min.txt", "r");
        if (df) {
            int v = 0;
            if (fscanf(df, "%d", &v) == 1 && (v == 512 || v == 1024 || v == 2048 || v == 4096)) {
                dsamp_min_dim = v;
            }
            fclose(df);
        }
    }
    uint8_t *downsampled = NULL;
    /* RENDER-TARGET-SAFE downsample (2026-07-16): ONLY halve textures that arrive
     * WITH pixel data. Textures allocated data==NULL are render-targets / FBO
     * color-or-depth attachments (and engine-streamed surfaces filled later via
     * glTexSubImage2D); halving their storage while the engine still renders at
     * full size corrupts CDRAM (black menu / GPUCRASH). Requiring upload_pixels
     * makes the 512 threshold SAFE. */
    if (level == 0 && upload_pixels &&
        width >= dsamp_min_dim && height >= dsamp_min_dim &&
        upload_type == GL_UNSIGNED_BYTE && upload_format == GL_RGBA) {
        downsampled = downsample_rgba8888_2x((const uint8_t *)upload_pixels, width, height);
        if (downsampled) {
            upload_pixels = downsampled;
            dsamp_mark((GLuint)s_texlru_bound);
            static unsigned int s_ds = 0;
            if (s_ds++ < 16U) {
                l_info("glTexImage2D: halve %dx%d RGBA -> %dx%d tex=%u (+data)",
                       width, height, width >> 1, height >> 1,
                       (unsigned)s_texlru_bound);
            }
            width >>= 1;
            height >>= 1;
        }
    }
    if (level == 0 && !downsampled) {
        /* Full-res (re-)spec of this id: clear any stale half-res mark so a later
         * glTexSubImage2D does NOT halve sub-data into this full-res storage
         * (id reuse via glTexImage2D re-spec, or a recycled id after delete). */
        dsamp_unmark((GLuint)s_texlru_bound);
    }
    GLint tlru_bound = (level == 0) ? (GLint)s_texlru_bound : 0;
    GLsizei tlru_bytes = (GLsizei)((long)width * (long)height * 4L); /* RGBA8888 (post-downsample) */
    GLint old_unpack_align = push_unpack_alignment_one();
    texlru_before_upload(tlru_bound, tlru_bytes);
    /* WHITE-SURFACE FIX (2026-07-20): a GL_RGBA source with GL_HALF_FLOAT_OES type but
     * a plain GL_RGBA internalformat makes vitaGL store it as 8-bit U8 — the F16 bit
     * patterns then read as blown-out (~white). Force a 16F internalformat so vitaGL
     * takes its half-float storage path (HDR/bloom/effect maps render correctly). */
    if (upload_type == 0x8D61 /*GL_HALF_FLOAT_OES*/ || upload_type == 0x140B /*GL_HALF_FLOAT*/) {
        upload_internalformat = 0x881A /*GL_RGBA16F*/;
    }
    glTexImage2D(target,
                 level,
                 (GLint)upload_internalformat,
                 width,
                 height,
                 border,
                 upload_format,
                 upload_type,
                 upload_pixels);
    GLenum err = glGetError();
    texlru_after_upload(tlru_bound, tlru_bytes, err);
    pop_unpack_alignment(old_unpack_align);
    force_complete_filter(target);

    /* Build a mip chain for POT RGBA8888 textures (opt-in) and switch to
     * trilinear. Guards: level 0, GL_TEXTURE_2D, RGBA8888, both dims power-of-two,
     * clean upload. NPOT / compressed / failed uploads keep flat GL_LINEAR.
     *
     * PRELOAD-BLACK FIX (2026-07-17): require upload_pixels != NULL. Textures
     * allocated data==NULL and filled later via glTexSubImage2D (the preload/UI
     * streaming pattern) would otherwise have glGenerateMipmap run over the EMPTY
     * allocation -> mip levels 1..N are all black, and the subsequent level-0
     * glTexSubImage2D never refreshes them (sub-image path does not regenerate
     * mips). With trilinear MIN_FILTER pinned here, any minified/scaled draw of
     * that UI samples the black mips -> the preload screen renders black. Gating
     * on real pixel data means such dynamic textures stay single-level + GL_LINEAR
     * (complete, correct), while in-game surface art — which arrives WITH data in
     * one glTexImage2D call — still gets its mip chain. */
    if (level == 0 && upload_pixels && target == 0x0DE1 /*GL_TEXTURE_2D*/ && err == GL_NO_ERROR &&
        upload_format == GL_RGBA && upload_type == GL_UNSIGNED_BYTE &&
        width >= mipmap_min_dim() && height >= mipmap_min_dim() &&
        (width & (width - 1)) == 0 && (height & (height - 1)) == 0 &&
        mipmap_gen_enabled()) {
        glGenerateMipmap(target);
        if (glGetError() == GL_NO_ERROR) {
            glTexParameteri(target, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2703 /*GL_LINEAR_MIPMAP_LINEAR*/);
            static unsigned int s_mip = 0;
            if (s_mip++ < 12U)
                l_info("glTexImage2D: mipmaps built %dx%d tex=%u -> trilinear", width, height, (unsigned)s_texlru_bound);
        }
        (void)drain_gl_errors_limited();
    }

    if (texture_upload_should_log(s_count, err) ||
        pre_err != GL_NO_ERROR ||
        ((orig_internalformat != upload_internalformat ||
          orig_format != upload_format ||
          type != upload_type ||
          zero_storage) && gl_verbose_diag_enabled())) {
        l_info("glTexImage2D #%u target=0x%X level=%d ifmt=0x%X->0x%X fmt=0x%X->0x%X type=0x%X->0x%X size=%dx%d border=%d data=%p conv=%d zero=%d pre=0x%X err=0x%X",
               s_count,
               (unsigned)target,
               level,
               (unsigned)orig_internalformat,
               (unsigned)upload_internalformat,
               (unsigned)orig_format,
               (unsigned)upload_format,
               (unsigned)type,
               (unsigned)upload_type,
               width,
               height,
               border,
               pixels,
               converted != NULL && !zero_storage,
               zero_storage,
               (unsigned)pre_err,
               (unsigned)err);
    }

    free(converted);
    free(downsampled);
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLsizei width, GLsizei height,
                              GLenum format, GLenum type, const void *pixels) {
    static unsigned int s_count = 0;
    s_count++;

    /* For downsampled textures we dropped level>0 in glTexImage2D, so drop their
     * level>0 sub-image fills too (the level doesn't exist). Normal textures keep
     * all levels — do NOT blanket-drop, that caused 795 0x502 errors. */
    if (level > 0 && dsamp_is((GLuint)s_texlru_bound)) {
        return;
    }

    const GLenum orig_format = format;
    GLenum upload_format = format;
    GLenum upload_type = type;
    const void *upload_pixels = pixels;
    uint8_t *converted = NULL;

    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
        upload_format = GL_RGBA;
        converted = convert_bgra_to_rgba((const uint8_t *)pixels, width, height);
        if (converted) {
            upload_pixels = converted;
        } else if (pixels) {
            l_warn("glTexSubImage2D BGRA conversion failed size=%dx%d; uploading as GL_RGBA with original pointer.", width, height);
        }
    }
#ifdef USE_PVR_PSP2
    /* PVR-only 16-bit promotion; vitaGL uploads 565/4444 natively (see glTexImage2D). */
    else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        upload_format = GL_RGBA;
        upload_type = GL_UNSIGNED_BYTE;
        converted = convert_rgb565_to_rgba8888((const uint16_t *)pixels, width, height);
        if (converted) {
            upload_pixels = converted;
        } else if (pixels) {
            upload_format = orig_format;
            upload_type = type;
            l_warn("glTexSubImage2D RGB565 conversion failed size=%dx%d; uploading original 16-bit data.", width, height);
        }
    } else if (format == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4) {
        upload_format = GL_RGBA;
        upload_type = GL_UNSIGNED_BYTE;
        converted = convert_rgba4444_to_rgba8888((const uint16_t *)pixels, width, height);
        if (converted) {
            upload_pixels = converted;
        } else if (pixels) {
            upload_type = type;
            l_warn("glTexSubImage2D RGBA4444 conversion failed size=%dx%d; uploading original 16-bit data.", width, height);
        }
    }
#endif

    /* If glTexImage2D halved this texture's storage, downsample the sub-image
     * data and halve its region so it matches the 512² allocation. */
    uint8_t *sub_ds = NULL;
    if (level == 0 && upload_type == GL_UNSIGNED_BYTE && upload_format == GL_RGBA &&
        upload_pixels && width >= 2 && height >= 2 && dsamp_is((GLuint)s_texlru_bound)) {
        sub_ds = downsample_rgba8888_2x((const uint8_t *)upload_pixels, width, height);
        if (sub_ds) {
            upload_pixels = sub_ds;
            xoffset >>= 1;
            yoffset >>= 1;
            width  >>= 1;
            height >>= 1;
        }
    }

    GLenum pre_err = drain_gl_errors_limited();
    GLint old_unpack_align = push_unpack_alignment_one();
    glTexSubImage2D(target,
                    level,
                    xoffset,
                    yoffset,
                    width,
                    height,
                    upload_format,
                    upload_type,
                    upload_pixels);
    GLenum err = glGetError();
    pop_unpack_alignment(old_unpack_align);
    /* force_complete_filter dropped on the sub-image path (2026-07-17): the parent
     * glTexImage2D already pinned MIN_FILTER + wrap state; a sub-image data upload
     * doesn't change texture completeness. Saves a glTexParameteri + error drain per
     * glTexSubImage2D (streaming textures update many sub-tiles per scene load). */
    free(sub_ds);

    if (texture_upload_should_log(s_count, err) ||
        pre_err != GL_NO_ERROR ||
        ((orig_format != upload_format || type != upload_type) && gl_verbose_diag_enabled())) {
        l_info("glTexSubImage2D #%u target=0x%X level=%d xy=%d,%d fmt=0x%X->0x%X type=0x%X->0x%X size=%dx%d data=%p conv=%d pre=0x%X err=0x%X",
               s_count,
               (unsigned)target,
               level,
               xoffset,
               yoffset,
               (unsigned)orig_format,
               (unsigned)upload_format,
               (unsigned)type,
               (unsigned)upload_type,
               width,
               height,
               pixels,
               converted != NULL,
               (unsigned)pre_err,
               (unsigned)err);
    }

    free(converted);
}

static int uniform_scalar_should_skip(const char *name, GLint location) {
    if (location >= 0) {
        return 0;
    }
    static unsigned s_logged = 0;
    if (s_logged++ < 16U) {
        l_info("%s skipped invalid location=%d", name, location);
    }
    return 1;
}

static int uniform_vector_should_skip(const char *name, GLint location, GLsizei count, const void *value) {
    if (location >= 0 && count > 0 && value) {
        return 0;
    }
    static unsigned s_logged = 0;
    if (s_logged++ < 32U) {
        l_info("%s skipped location=%d count=%d value=%p", name, location, count, value);
    }
    return 1;
}

void glUniform1f_soloader(GLint location, GLfloat v0) {
    if (uniform_scalar_should_skip("glUniform1f", location)) return;
    glUniform1f(location, v0);
}

void glUniform1fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniform1fv", location, count, value)) return;
    glUniform1fv(location, count, value);
}

void glUniform1i_soloader(GLint location, GLint v0) {
    static unsigned int s_count = 0;
    s_count++;
    if (uniform_scalar_should_skip("glUniform1i", location)) return;

#if MCSM_FAST_FINAL_RUNTIME
    glUniform1i(location, v0);
    (void)s_count;
#else
    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;
    GLint program = gl_get_int_for_diag(GL_CURRENT_PROGRAM, &query_err);

    glUniform1i(location, v0);
    GLenum err = glGetError();

    const int sampler_idx = (v0 >= 0 && v0 < GL_DIAG_TEX_UNIT_CAP) ? v0 : -1;
    const GLuint bound_tex = sampler_idx >= 0 ? g_diag_bound_texture_2d[sampler_idx] : 0;
    if (gl_sampler_diag_should_log(s_count, pre_err, err) || query_err != GL_NO_ERROR || sampler_idx < 0) {
        l_info("glUniform1i #%u program=%d loc=%d value=%d bound_tex2d=%u pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               program,
               location,
               v0,
               bound_tex,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }
#endif
}

void glUniform1iv_soloader(GLint location, GLsizei count, const GLint *value) {
    static unsigned int s_count = 0;
    s_count++;
    if (uniform_vector_should_skip("glUniform1iv", location, count, value)) return;

#if MCSM_FAST_FINAL_RUNTIME
    glUniform1iv(location, count, value);
    (void)s_count;
#else
    const GLint first_value = (value && count > 0) ? value[0] : -1;
    GLenum pre_err = drain_gl_errors_limited();
    GLenum query_err = GL_NO_ERROR;
    GLint program = gl_get_int_for_diag(GL_CURRENT_PROGRAM, &query_err);

    glUniform1iv(location, count, value);
    GLenum err = glGetError();

    const int sampler_idx = (first_value >= 0 && first_value < GL_DIAG_TEX_UNIT_CAP) ? first_value : -1;
    const GLuint bound_tex = sampler_idx >= 0 ? g_diag_bound_texture_2d[sampler_idx] : 0;
    if (gl_sampler_diag_should_log(s_count, pre_err, err) || query_err != GL_NO_ERROR || sampler_idx < 0) {
        l_info("glUniform1iv #%u program=%d loc=%d count=%d first=%d bound_tex2d=%u pre=0x%X qerr=0x%X err=0x%X",
               s_count,
               program,
               location,
               count,
               first_value,
               bound_tex,
               (unsigned)pre_err,
               (unsigned)query_err,
               (unsigned)err);
    }
#endif
}

void glUniform2f_soloader(GLint location, GLfloat v0, GLfloat v1) {
    if (uniform_scalar_should_skip("glUniform2f", location)) return;
    glUniform2f(location, v0, v1);
}

void glUniform2fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniform2fv", location, count, value)) return;
    glUniform2fv(location, count, value);
}

void glUniform2i_soloader(GLint location, GLint v0, GLint v1) {
    if (uniform_scalar_should_skip("glUniform2i", location)) return;
    glUniform2i(location, v0, v1);
}

void glUniform2iv_soloader(GLint location, GLsizei count, const GLint *value) {
    if (uniform_vector_should_skip("glUniform2iv", location, count, value)) return;
    glUniform2iv(location, count, value);
}

void glUniform3f_soloader(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    if (uniform_scalar_should_skip("glUniform3f", location)) return;
    glUniform3f(location, v0, v1, v2);
}

void glUniform3fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniform3fv", location, count, value)) return;
    glUniform3fv(location, count, value);
}

void glUniform3i_soloader(GLint location, GLint v0, GLint v1, GLint v2) {
    if (uniform_scalar_should_skip("glUniform3i", location)) return;
    glUniform3i(location, v0, v1, v2);
}

void glUniform3iv_soloader(GLint location, GLsizei count, const GLint *value) {
    if (uniform_vector_should_skip("glUniform3iv", location, count, value)) return;
    glUniform3iv(location, count, value);
}

void glUniform4f_soloader(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    if (uniform_scalar_should_skip("glUniform4f", location)) return;
    glUniform4f(location, v0, v1, v2, v3);
}

/* MEGA-LOG: the skinned-character bone palette (U7_0) is uploaded as a big
 * glUniform4fv (count ~84 vec4 = 28 row-major mat4x3 bones). This is the single
 * most important animation signal: it tells us whether the skeleton is actually
 * POSED+ANIMATING or frozen at bind pose (identity). For each big upload, scan
 * all bones for max deviation from identity (diagonal at floats 0,5,10 of each
 * 12-float bone) and whether the palette changes between frames.
 *   maxdev~0 + nonId~0           -> BIND POSE (animation NOT applied) = frozen
 *   maxdev>0.1 + changes climbing -> skeleton IS animating
 *   changes==0                   -> palette static (not re-posed) */
static int mcsm_anim_pose_diag_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        SceUID fd = sceIoOpen("ux0:data/mcsm/animdiag.txt", SCE_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            s_enabled = 1;
            l_info("ANIM-POSE diagnostics enabled by animdiag.txt");
        } else {
            s_enabled = 0;
        }
    }
    return s_enabled;
}

static void mcsm_log_anim_pose(GLint location, GLsizei count, const GLfloat *value) {
    if (!mcsm_anim_pose_diag_enabled()) {
        return;
    }
    if (count < 60 || !value) {
        return;
    }
    static uint32_t s_last_hash = 0;
    static unsigned int s_uploads = 0, s_changes = 0;
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)value;
    size_t n = (size_t)count * 4u * sizeof(float);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    int changed = (h != s_last_hash);
    s_uploads++;
    if (changed) { s_changes++; s_last_hash = h; }

    const GLsizei tf = count * 4;
    float maxdev = 0.0f; int maxbone = -1; int nonid = 0;
    for (GLsizei f = 0; f < tf; ++f) {
        int within = f % 12;
        float expect = (within == 0 || within == 5 || within == 10) ? 1.0f : 0.0f;
        float d = value[f] - expect; if (d < 0.0f) d = -d;
        if (d > 0.01f) ++nonid;
        if (d > maxdev) { maxdev = d; maxbone = f / 12; }
    }
    if (s_uploads <= 12U || (s_uploads % 60U) == 0U || (changed && s_changes <= 32U)) {
        l_info("ANIM-POSE prog=%u loc=%d count=%d maxdev=%.4f @bone%d nonId=%d/%d uploads=%u changes=%u first=%g,%g,%g,%g",
               g_uniform_current_program, location, count, maxdev, maxbone, nonid, (int)tf,
               s_uploads, s_changes, value[0], value[1], value[2], value[3]);
    }
}

void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniform4fv", location, count, value)) return;
    mcsm_log_anim_pose(location, count, value);

#if MCSM_FAST_FINAL_RUNTIME
    int split = gl_uniform4fv_split_telltale(location, count, value);
    if (!split) {
        glUniform4fv(location, count, value);
    }
#else
    GLenum pre_err = drain_gl_errors_limited();
    int split = gl_uniform4fv_split_telltale(location, count, value);
    if (!split) {
        glUniform4fv(location, count, value);
    }
    GLenum err = glGetError();

    static unsigned s_logged = 0;
    if (err != GL_NO_ERROR || pre_err != GL_NO_ERROR || (count > 1 && !split && s_logged < 16U)) {
        l_info("glUniform4fv program=%u loc=%d count=%d split=%d pre=0x%X err=0x%X",
               g_uniform_current_program,
               location,
               count,
               split,
               (unsigned)pre_err,
               (unsigned)err);
        s_logged++;
    }
#endif
}

void glUniform4i_soloader(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
    if (uniform_scalar_should_skip("glUniform4i", location)) return;
    glUniform4i(location, v0, v1, v2, v3);
}

void glUniform4iv_soloader(GLint location, GLsizei count, const GLint *value) {
    if (uniform_vector_should_skip("glUniform4iv", location, count, value)) return;
    glUniform4iv(location, count, value);
}

void glUniformMatrix2fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniformMatrix2fv", location, count, value)) return;
    glUniformMatrix2fv(location, count, transpose, value);
}

void glUniformMatrix3fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniformMatrix3fv", location, count, value)) return;
    glUniformMatrix3fv(location, count, transpose, value);
}

void glUniformMatrix4fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniformMatrix4fv", location, count, value)) return;
    glUniformMatrix4fv(location, count, transpose, value);
}

void glTexStorage2D_soloader(GLenum target, GLsizei levels, GLenum internalformat,
                             GLsizei width, GLsizei height) {
    if (levels <= 0 || width <= 0 || height <= 0) {
        return;
    }

    if (internalformat == GL_ETC1_RGB8_OES) {
        static int warned = 0;
        if (!warned) {
            l_warn("glTexStorage2D: ETC1 immutable storage is unsupported; relying on runtime fallback.");
            warned = 1;
        }
        return;
    }

    GLenum format = 0;
    GLenum type = GL_UNSIGNED_BYTE;
    if (!resolve_tex_storage_format(internalformat, &format, &type)) {
        static int warned = 0;
        if (!warned) {
            l_warn("glTexStorage2D: unsupported internalformat 0x%X.", internalformat);
            warned = 1;
        }
        return;
    }

    GLsizei w = width;
    GLsizei h = height;
    for (GLsizei level = 0; level < levels; ++level) {
        glTexImage2D_soloader(target, level, format, w, h, 0, format, type, NULL);
        if (w > 1) {
            w >>= 1;
        }
        if (h > 1) {
            h >>= 1;
        }
    }
    /* The engine later uploads to individual levels via glTexSubImage2D.
     * Multi-level textures created this way get the GL default min filter
     * (GL_NEAREST_MIPMAP_LINEAR) but only some levels will actually have
     * data. Without a non-mipmap filter the texture is INCOMPLETE and
     * samples as a flat colour on Vita PVR. Force GL_LINEAR here so the
     * texture renders correctly regardless of which levels get data. */
    force_complete_filter(target);
}

void glInvalidateFramebuffer_soloader(GLenum target, GLsizei numAttachments,
                                      const GLenum *attachments) {
#ifdef USE_PVR_PSP2
    /* PowerVR tile-based GPU MUST discard framebuffer attachments before
     * rendering; stale tile data corrupts frames with random colors/existing
     * framebuffer content. This is the #1 cause of the flashing. */
    glDiscardFramebufferEXT(target, numAttachments, attachments);
#else
    (void)target;
    (void)numAttachments;
    (void)attachments;
#endif
}

GLsync glFenceSync_soloader(GLenum condition, GLbitfield flags) {
    (void)condition;
    (void)flags;
    return (GLsync)&g_sync_sentinel;
}

void glDeleteSync_soloader(GLsync sync) {
    (void)sync;
}

GLenum glClientWaitSync_soloader(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    (void)sync;
    (void)flags;
    (void)timeout;
    return GL_ALREADY_SIGNALED;
}

void glWaitSync_soloader(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    (void)sync;
    (void)flags;
    (void)timeout;
}

// Log what the GPU driver (vitaGL/SceGxm) actually reports, so diagnostics show
// the real hardware identity and not just the PowerVR string we hand to the game.
static void log_real_gl_identity(void) {
    if (g_gl_real_identity_logged) {
        return;
    }
    g_gl_real_identity_logged = 1;

    const GLubyte *real_vendor = glGetString(GL_VENDOR);
    const GLubyte *real_renderer = glGetString(GL_RENDERER);
    const GLubyte *real_version = glGetString(GL_VERSION);
    l_info("GL identity REAL (hardware): vendor=\"%s\" renderer=\"%s\" version=\"%s\"",
           real_vendor ? (const char *)real_vendor : "(null)",
           real_renderer ? (const char *)real_renderer : "(null)",
           real_version ? (const char *)real_version : "(null)");
}

const GLubyte *glGetString_soloader(GLenum name) {
    switch (name) {
        case GL_VENDOR:
            if (!g_gl_identity_logged) {
                l_info("GL identity spoof (to game): vendor=\"%s\" renderer=\"%s\"", k_gl_vendor, k_gl_renderer);
                g_gl_identity_logged = 1;
            }
            log_real_gl_identity();
            return (const GLubyte *)k_gl_vendor;
        case GL_RENDERER:
            if (!g_gl_identity_logged) {
                l_info("GL identity spoof (to game): vendor=\"%s\" renderer=\"%s\"", k_gl_vendor, k_gl_renderer);
                g_gl_identity_logged = 1;
            }
            log_real_gl_identity();
            return (const GLubyte *)k_gl_renderer;
        case GL_VERSION:
            return (const GLubyte *)k_gl_version;
#ifdef GL_SHADING_LANGUAGE_VERSION
        case GL_SHADING_LANGUAGE_VERSION:
            return (const GLubyte *)k_glsl_version;
#endif
        case GL_EXTENSIONS: {
            const GLubyte *extensions = glGetString(name);
            if (extensions && !g_gl_extensions_logged) {
                g_gl_extensions_logged = 1;
                log_relevant_extension_support(get_augmented_extension_string((const char *)extensions));
            }
            return (const GLubyte *)get_augmented_extension_string((const char *)extensions);
        }
        default:
            return glGetString(name);
    }
}

// Replace every occurrence of `find` with `repl` in-place. `repl` MUST be no
// longer than `find` (we only ever shrink), so this is always safe in the
// original buffer. Returns the number of replacements made.
static int glsl_replace_shorter(char *s, const char *find, const char *repl) {
    const size_t flen = strlen(find);
    const size_t rlen = strlen(repl);
    int n = 0;
    char *p = s;
    while ((p = strstr(p, find)) != NULL) {
        memcpy(p, repl, rlen);
        memmove(p + rlen, p + flen, strlen(p + flen) + 1);
        p += rlen;
        n++;
    }
    return n;
}

static int glsl_replace_alloc(char **src_io, size_t *len_io,
                              const char *find, const char *repl) {
    const size_t flen = strlen(find);
    const size_t rlen = strlen(repl);
    const char *src = *src_io;
    const char *scan = src;
    char *dst;
    char *out;
    int n = 0;

    if (!src || flen == 0) {
        return 0;
    }

    while ((scan = strstr(scan, find)) != NULL) {
        n++;
        scan += flen;
    }

    if (n == 0) {
        return 0;
    }

    const size_t old_len = *len_io;
    const size_t new_len = (rlen >= flen)
        ? old_len + ((size_t)n * (rlen - flen))
        : old_len - ((size_t)n * (flen - rlen));
    dst = malloc(new_len + 1);
    if (!dst) {
        l_warn("glShaderSource: allocation failed while rewriting '%s'", find);
        return 0;
    }

    scan = src;
    out = dst;
    while (1) {
        const char *hit = strstr(scan, find);
        size_t prefix_len;
        if (!hit) {
            const size_t tail_len = strlen(scan);
            memcpy(out, scan, tail_len);
            out += tail_len;
            break;
        }
        prefix_len = (size_t)(hit - scan);
        memcpy(out, scan, prefix_len);
        out += prefix_len;
        memcpy(out, repl, rlen);
        out += rlen;
        scan = hit + flen;
    }
    *out = '\0';

    free(*src_io);
    *src_io = dst;
    /* ROOT FIX (2026-07-20b): use the ACTUAL bytes written, not the arithmetic
     * new_len. If `src` carried an embedded NUL (an upstream over-long _length),
     * the strstr/strlen copy above stopped at it while new_len assumed the full
     * span — leaving uninitialized malloc tail bytes in [out .. dst+new_len] that
     * VARY every launch and, when hashed, made the progcache key non-deterministic
     * (fragment shaders only, since only they hit this rewriter). Reporting the
     * real length keeps the buffer garbage-free at the source. */
    *len_io = (size_t)(out - dst);
    return n;
}

static int glsl_prepend_alloc(char **src_io, size_t *len_io, const char *prefix) {
    if (!src_io || !*src_io || !len_io || !prefix) {
        return 0;
    }

    const size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) {
        return 0;
    }

    const size_t old_len = *len_io;
    char *dst = malloc(prefix_len + old_len + 1);
    if (!dst) {
        l_warn("glShaderSource: allocation failed while prepending shader fixup");
        return 0;
    }

    memcpy(dst, prefix, prefix_len);
    memcpy(dst + prefix_len, *src_io, old_len + 1);
    free(*src_io);
    *src_io = dst;
    *len_io = prefix_len + old_len;
    return 1;
}

static int promote_shader_precision_for_vita(char **src_io, size_t *len_io) {
    int changed = 0;

    // Telltale's PowerVR shaders lean on lowp varyings/default fragment math.
    // On Vita/PVR this compiles cleanly but can quantize or clamp color-heavy
    // paths into flashing flat colors. Keep the rewrite to the common aliases
    // seen in dumped shaders so vertex/fragment varying precision stays paired.
    changed += glsl_replace_alloc(src_io, len_io,
                                  "#define ulow uniform lowp",
                                  "#define ulow uniform mediump");
    changed += glsl_replace_alloc(src_io, len_io,
                                  "#define vlow varying lowp",
                                  "#define vlow varying mediump");
    changed += glsl_replace_alloc(src_io, len_io,
                                  "precision lowp float;",
                                  "precision mediump float;");
    return changed;
}

// vitaGL's ShaccCg translator cannot compile a few PowerVR-specific GLSL
// features the game's shaders use. A shader that fails to compile yields an
// invalid GL program, and the engine then crashes (heap corruption) while
// enumerating that program's uniforms in GFXPlatform::CreateProgram.
//
// Rewrite those constructs into something legal so the shader compiles and the
// program links. Rendering for that material is wrong (framebuffer-fetch blend
// / shadow comparison are neutralized), but boot no longer crashes — this is a
// probe to see whether the title screen renders without the real PowerVR
// driver. All substitutions are <= the original length so they are done in
// place on `src`. Returns non-zero if anything was changed.
/* The neutralizer replaces gl_LastFragData references with vec4(1.0) and
 * comments out unsupported extension pragmas. The shaders use #define macros:
 *   #define ttFragIn0 gl_LastFragData[0]
 * After replacement this becomes:
 *   #define ttFragIn0 vec4(1.0)
 * All ttFragIn0 references in the shader body then resolve to opaque white.
 * vec4(x) fills all 4 components with x in GLSL ES 1.00 per the spec,
 * so mix(vec4(1.0), surf, alpha) = surf*alpha + white*(1-alpha) which
 * equals surf for opaque geometry (α≈1). This is the standard approach used
 * by the soloader-boilerplate reference port for unsupported extensions. */
static int neutralize_unsupported_glsl(char **src_io, size_t *len_io) {
    int changed = 0;
    char *src = src_io ? *src_io : NULL;

    if (!src || !len_io) {
        return 0;
    }

    /* Replace gl_LastFragData with vec4(1.0) = white.
     * In GLSL ES, vec4(x) fills ALL 4 components with x per the spec.
     * mix(white, surf, alpha) = white*(1-α) + surf*α, which equals
     * surf for opaque geometry (α≈1). Replacements use glsl_replace_shorter
     * because vec4(1.0) is shorter than gl_LastFragData[0]. */
    if (strstr(src, "gl_LastFragData")) {
        /* WHITE-SURFACE FIX (2026-07-20): the framebuffer-read (gl_LastFragData) stub
         * is white by default. That's the identity for MODULATE blends (mix(white,surf)
         * = surf at a=1) but for ADDITIVE light accumulation (out = fbRead + light) it
         * makes every pixel white. Opt-in fbfetch_zero.txt swaps the stub to vec4(0.0),
         * the identity for additive — pick whichever makes the white surfaces correct.
         * Both are 9 chars < gl_LastFragData[0] (18), so glsl_replace_shorter is valid. */
        static int s_fbz = -1;
        if (s_fbz < 0) { s_fbz = 0; FILE *fz = mcsm_open_setting("fbfetch_zero.txt", "r"); if (fz) { fclose(fz); s_fbz = 1; } }
        const char *repl = s_fbz ? "vec4(0.0)" : "vec4(1.0)";
        changed += glsl_replace_shorter(src, "gl_LastFragData[0]", repl);
        changed += glsl_replace_shorter(src, "gl_LastFragData[1]", repl);
        changed += glsl_replace_shorter(src, "gl_LastFragData[2]", repl);
        changed += glsl_replace_shorter(src, "gl_LastFragData[3]", repl);
        changed += glsl_replace_shorter(src, "gl_LastFragData",    repl);
    }

    /* PROGCACHE KEY-DRIFT FIX (2026-07-17): the engine emits a vestigial
     * `#define ttFragIn0 <X>` whose value varies run-to-run — observed values:
     * vec4(0.0), vec4(1.0), vec4(1.0,1.0,1.0,1.0), gl_FragColor, gl_LastFragData[0].
     * Verified across 455 dumped shaders: ttFragIn0 is a DEAD macro — #defined but
     * NEVER referenced in any shader body — so its value has ZERO effect on the
     * compiled GXP. But it was the DOMINANT progcache key-drift source: ~87% of
     * shaders carry it, so the SAME shader hashed to a different FNV key nearly
     * every run and its <key>.bin was never reused = permanent mid-frame ShaccCg
     * recompile = the "insane fps fluctuation" + "same-spot stutter" + "shaders
     * not being read". Folding every variant to one canonical value makes the key
     * reproducible across runs. Provably safe: dead macro -> byte-identical GXP, so
     * a cache entry compiled under any variant is correct for all. (The
     * gl_LastFragData[0] variant was already folded to vec4(1.0) just above.) */
    changed += glsl_replace_alloc(src_io, len_io, "#define ttFragIn0 vec4(1.0,1.0,1.0,1.0)", "#define ttFragIn0 vec4(1.0)");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "#define ttFragIn0 vec4(0.0)", "#define ttFragIn0 vec4(1.0)");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "#define ttFragIn0 gl_FragColor", "#define ttFragIn0 vec4(1.0)");
    src = *src_io;

    /* Disable fragment-depth writes without leaving unsupported built-ins for
     * ShaccCg. Assignments become writes to an ordinary throwaway float. */
    if (strstr(src, "gl_FragDepth")) {
        changed += glsl_prepend_alloc(src_io, len_io, "float mcsm_unused_frag_depth;\n");
        src = *src_io;
        changed += glsl_replace_alloc(src_io, len_io, "gl_FragDepthEXT", "mcsm_unused_frag_depth");
        src = *src_io;
        changed += glsl_replace_alloc(src_io, len_io, "gl_FragDepth", "mcsm_unused_frag_depth");
        src = *src_io;
    }

    changed += glsl_replace_alloc(src_io, len_io, "sampler2DShadow", "sampler2D");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "samplerCubeShadow", "samplerCube");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "shadow2DProjEXT", "texture2DProj");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "shadow2DProj", "texture2DProj");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "shadow2DEXT", "texture2D");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "shadow2D", "texture2D");
    src = *src_io;

    changed += glsl_replace_alloc(src_io, len_io, "texture2DProjLodEXT", "texture2DProj");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "texture2DProjLod", "texture2DProj");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "texture2DLodEXT", "texture2D");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "texture2DLod", "texture2D");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "textureCubeLodEXT", "textureCube");
    src = *src_io;
    changed += glsl_replace_alloc(src_io, len_io, "textureCubeLod", "textureCube");
    src = *src_io;

    /* Comment out unsupported extension pragmas */
    static const char *exts[] = {
        "#extension GL_EXT_shader_framebuffer_fetch",
        "#extension GL_ARM_shader_framebuffer_fetch",
        "#extension GL_EXT_shadow_samplers",
        "#extension GL_EXT_frag_depth",
        "#extension GL_EXT_shader_texture_lod",
        "#extension GL_ARB_shader_texture_lod",
    };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); ++e) {
        char *ext = strstr(src, exts[e]);
        while (ext) {
            ext[0] = '/';
            ext[1] = '/';
            changed++;
            ext = strstr(ext + 2, exts[e]);
        }
    }

    return changed;
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        total_length += get_shader_source_part_length(string[i], _length, i);
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        const size_t part_len = get_shader_source_part_length(string[i], _length, i);
        memcpy(str + l, string[i], part_len);
        l += part_len;
    }
    str[total_length] = '\0';

    int precision_promotions = promote_shader_precision_for_vita(&str, &total_length);
    if (precision_promotions) {
        static unsigned s_precision_logged = 0;
        if (s_precision_logged++ < 8U) {
            l_info("glShaderSource(%u): promoted %d lowp shader precision qualifiers to mediump.",
                   shader, precision_promotions);
        }
    }

#ifndef USE_PVR_PSP2
    if (neutralize_unsupported_glsl(&str, &total_length)) {
        static unsigned s_neutralized_logged = 0;
        if (s_neutralized_logged++ < 8U) {
            l_info("glShaderSource(%u): neutralized unsupported GLSL features "
                   "(framebuffer-fetch/shadow/depth/lod) so the shader can compile.", shader);
        }
    }
#endif

    track_shader_source(shader, str, total_length);
    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
    static uint32_t s_compile_counter = 0;

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        GLint status = GL_TRUE;
        GLenum err_before = GL_NO_ERROR;
        GLenum err_after = GL_NO_ERROR;
        shader_diag_entry *entry = get_shader_diag_entry(shader, 0);
        s_compile_counter++;

        err_before = glGetError();
        if (s_compile_counter <= 8U) {
            log_shader_compile_runtime_state(shader, "before", err_before);
        }
        if (entry && entry->owned_source) {
            const GLchar *src = entry->owned_source;
            glShaderSource(shader, 1, &src, NULL);
        }
        glCompileShader(shader);
        err_after = glGetError();
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (s_compile_counter <= 8U || status != GL_TRUE || err_after != GL_NO_ERROR) {
            log_shader_compile_runtime_state(shader, "after", err_after);
        }
        log_shader_compile_failure(shader);

        // Some game shaders use features vitaGL/ShaccCg can't translate
        // (GL_EXT_shader_framebuffer_fetch / gl_LastFragData, shadow samplers).
        // A failed shader yields an invalid GL program; the engine then crashes
        // (heap corruption) enumerating uniforms in GFXPlatform::CreateProgram.
        // Substitute a minimal valid shader so the program still links (renders
        // wrong for that material, but no crash) and boot can continue.
#ifdef USE_PVR_PSP2
        /* PVR_PSP2 compiles shaders immediately, so status!=GL_TRUE is a real
         * failure -> substitute a minimal valid shader to avoid the engine
         * crashing on an invalid program. On vitaGL with VGL_MODE_POSTPONED the
         * actual GLSL->GXP compile happens in glLinkProgram, so glCompileShader
         * legitimately reports status=0 here — substituting a fallback would
         * REPLACE the real shader with a black one (and even the fallback reads
         * status=0). So this block must NOT run for the vitaGL build. */
        if (status != GL_TRUE) {
            GLint stype = 0;
            glGetShaderiv(shader, GL_SHADER_TYPE, &stype);
            const char *fallback = (stype == GL_VERTEX_SHADER)
                ? "void main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}"
                : "precision mediump float;void main(){gl_FragColor=vec4(0.0,0.0,0.0,1.0);}";
            glShaderSource(shader, 1, &fallback, NULL);
            glCompileShader(shader);
            GLint fbstatus = GL_TRUE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &fbstatus);
            l_warn("glCompileShader(%u): original failed; substituted minimal %s fallback (status=%d).",
                   shader, (stype == GL_VERTEX_SHADER) ? "vertex" : "fragment", (int)fbstatus);
        }
#else
        (void)status;  /* vitaGL POSTPONED: real compile deferred to glLinkProgram */
#endif
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

/* ============================================================================
 * LOADER-SIDE SHADER PROGRAM CACHE (2026-06-30)
 * The Telltale engine compiles 63 UNIQUE GLSL programs via ShaccCg, in batches
 * at scene transitions (~99s of frozen sim-thread time per playthrough). They
 * never repeat, so the only way to kill the recompiles is to persist the
 * COMPILED GXM binary across runs. vitaGL's own HAVE_SHADER_CACHE does this but
 * is crash-prone (titleid overflow, OOM, stale-format deserialize). Instead we
 * cache at the loader level via glGetProgramBinary/glProgramBinary (both present
 * and functional in the stable lib), keyed by a hash of the (already vita-
 * patched) shader sources, with OUR OWN version magic so a stale or mismatched
 * file is IGNORED, never misread. Fully fail-safe: if no valid cache file
 * exists the program is untouched and compiles normally; we only call
 * glProgramBinary on a fully validated file.
 *
 * First run: compiles all 63 (freezes, behind the load screen) and writes the
 * binaries. Every run after: loads them instantly -> the scene-transition
 * freezes drop to near-zero. Bump PROGCACHE_MAGIC on any lib/compiler change. */
#ifndef USE_PVR_PSP2
extern void glGetProgramBinary(GLuint, GLsizei, GLsizei *, GLenum *, void *);
extern void glProgramBinary(GLuint, GLenum, const void *, GLsizei);
extern void glGetAttachedShaders(GLuint, GLsizei, GLsizei *, GLuint *);
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif

#define PROGCACHE_DIR    "ux0:data/mcsm_progcache"
#define PROGCACHE_MAGIC  0x4d435034u   /* 'MCP4' — bumped 2026-07-20b: MCP3 clamped the engine's
                                        * over-long _length but device missdumps proved FRAGMENT
                                        * shaders still carried a NUL+varying-garbage tail (from
                                        * glsl_replace_alloc's strlen-copy / arithmetic-length
                                        * mismatch). track_shader_source now clamps the hashed source
                                        * to the first embedded NUL — the final, path-independent fix.
                                        * Keys change again vs MCP3, so those caches must invalidate. */
#define PROGCACHE_MAXBIN (1024u * 1024u)
static int g_progcache_dir_ready = 0;
static unsigned g_progcache_hits = 0, g_progcache_misses = 0;
/* SAVE DIAGNOSTIC counters (2026-07-18): the on-device cache came up EMPTY after
 * play. These pin WHY progcache_save wrote nothing, written via sceIo (reliable,
 * unlike newlib fprintf) to ux0:data/mcsm/progcache_diag.txt. */
static unsigned g_pc_links = 0, g_pc_key0 = 0, g_pc_notlinked = 0, g_pc_nobin = 0;
static unsigned g_pc_saveok = 0, g_pc_openfail = 0, g_pc_loadfail = 0;
static int g_pc_lastopen = 1;

/* FNV-1a over the attached shaders' tracked (patched) sources. 0 = unkeyable. */
static uint64_t progcache_key_for_program(GLuint program) {
    GLuint sh[8]; GLsizei n = 0;
    glGetAttachedShaders(program, 8, &n, sh);
    if (n <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    for (GLsizei i = 0; i < n; i++) {
        shader_diag_entry *e = get_shader_diag_entry(sh[i], 0);
        if (!e || !e->owned_source || e->owned_source_len == 0) return 0;
        const unsigned char *p = (const unsigned char *)e->owned_source;
        for (size_t k = 0; k < e->owned_source_len; k++) { h ^= p[k]; h *= 1099511628211ull; }
        h ^= 0x7c; h *= 1099511628211ull;   /* separator between shaders */
    }
    return h ? h : 1;
}

static void progcache_path(uint64_t key, char *out, int outsz) {
    snprintf(out, outsz, "%s/%08X%08X.bin", PROGCACHE_DIR,
             (unsigned)(key >> 32), (unsigned)(key & 0xffffffffu));
}

/* Returns 1 if the program is now linked from a valid cache file (skip compile),
 * 0 if there was no usable file (program untouched -> compile normally). */
static int progcache_try_load(GLuint program, uint64_t key) {
    if (!key) return 0;
    char path[160]; progcache_path(key, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    uint32_t hdr[4];   /* magic, version, binaryFormat, length */
    if (sceIoRead(fd, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        hdr[0] != PROGCACHE_MAGIC || hdr[3] == 0 || hdr[3] > PROGCACHE_MAXBIN) {
        sceIoClose(fd); return 0;   /* stale/garbage -> recompile + overwrite */
    }
    void *buf = malloc(hdr[3]);
    if (!buf) { sceIoClose(fd); return 0; }
    int rd = sceIoRead(fd, buf, hdr[3]);
    sceIoClose(fd);
    if (rd != (int)hdr[3]) { free(buf); return 0; }
    /* File fully validated: commit to glProgramBinary. */
    glProgramBinary(program, (GLenum)hdr[2], buf, (GLsizei)hdr[3]);
    free(buf);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        /* BULLETPROOF (2026-07-17): vitaGL's unserialize sets both shader progs
         * unconditionally, so glProgramBinary should ALWAYS leave the program
         * PROG_LINKED — this branch is purely defensive. Do NOT delete the file and
         * do NOT claim a hit: return 0 so the caller does a normal glLinkProgram
         * recompile that overwrites this key with a fresh binary. The old code
         * deleted the file AND returned 1 (hit) -> the engine ran a BROKEN program
         * that session AND the key recompiled next session = the repeat-stutter
         * churn. Only the magic/length header check above is allowed to delete. */
        g_pc_loadfail++;   /* file existed + loaded but glProgramBinary failed to restore = the load bug */
        l_warn("progcache: binary link!=TRUE (defensive) -> recompile, key kept");
        return 0;
    }
    return 1;
}

/* Persist the freshly-compiled binary for next run. Best-effort, never fatal. */
static void progcache_save(GLuint program, uint64_t key) {
    if (!key) return;
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) { g_pc_notlinked++; return; }
    GLint binlen = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binlen);
    if (binlen <= 0 || (unsigned)binlen > PROGCACHE_MAXBIN) { g_pc_nobin++; return; }
    void *bin = malloc((size_t)binlen);
    if (!bin) return;
    GLsizei len = 0; GLenum fmt = 0;
    glGetProgramBinary(program, binlen, &len, &fmt, bin);
    if (len <= 0 || (unsigned)len > (unsigned)binlen) { free(bin); g_pc_nobin++; return; }
    if (!g_progcache_dir_ready) { sceIoMkdir(PROGCACHE_DIR, 0777); g_progcache_dir_ready = 1; }
    char path[160]; progcache_path(key, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        /* SELF-HEAL (2026-07-18): the progcache dir may not exist (fresh install
         * with no seed, or user deleted it mid-session). Re-create + retry once. */
        sceIoMkdir(PROGCACHE_DIR, 0777);
        fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    }
    g_pc_lastopen = (int)fd;
    if (fd >= 0) {
        uint32_t hdr[4] = { PROGCACHE_MAGIC, 1u, (uint32_t)fmt, (uint32_t)len };
        sceIoWrite(fd, hdr, sizeof(hdr));
        sceIoWrite(fd, bin, len);
        sceIoClose(fd);
        g_pc_saveok++;
    } else {
        g_pc_openfail++;
    }
    free(bin);
}
#endif /* !USE_PVR_PSP2 */

#ifndef USE_PVR_PSP2
/* DRIFT DIAGNOSTIC (2026-07-18): on a progcache MISS, dump each attached shader's
 * tracked (patched) source. Two rejoins of the same scene each produce their MISS
 * sources under their (drifting) key names -> diffing near-identical pairs reveals
 * EXACTLY what varies in the source across runs = why the key drifts and the
 * shader recompiles (stutters) on rejoin. sceIo write (reliable). Capped. */
static void dump_miss_sources(GLuint program, uint64_t key) {
    static unsigned s_dumped = 0;
    if (s_dumped >= 80u) return;
    GLuint sh[8]; GLsizei n = 0;
    glGetAttachedShaders(program, 8, &n, sh);
    for (GLsizei i = 0; i < n; i++) {
        shader_diag_entry *e = get_shader_diag_entry(sh[i], 0);
        if (!e || !e->owned_source || !e->owned_source_len) continue;
        if (s_dumped == 0) sceIoMkdir("ux0:data/mcsm/missdump", 0777);
        char path[128];
        snprintf(path, sizeof(path), "ux0:data/mcsm/missdump/%08X%08X_%d.glsl",
                 (unsigned)(key >> 32), (unsigned)(key & 0xffffffffu), (int)i);
        SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) { sceIoWrite(fd, e->owned_source, (SceSize)e->owned_source_len); sceIoClose(fd); s_dumped++; }
    }
}
#endif

void glLinkProgram_soloader(GLuint program) {
#ifndef USE_PVR_PSP2
    /* On vitaGL with VGL_MODE_POSTPONED the real GLSL->GXP compile (via shark)
     * happens HERE, at link. A complex shader could hang/stall shark; mark the
     * phase so a freeze shows glphase=2 in the watchdog snapshot. */
    /* Always-on progcache HEALTH file (2026-07-18) written via sceIo (newlib
     * fprintf proved unreliable here). Overwritten every 16th link with the full
     * save-path breakdown so the user can see EXACTLY what the cache is doing:
     *   hits/misses = read side;  saveok = files actually written this run.
     *   key0 = programs with no tracked source (uncacheable);
     *   notlinked/nobin = program had no binary to save;
     *   openfail + lastopen = sceIoOpen failure (dir/permission).
     * THE TEST: play a scene, then replay it — misses should stop growing and
     * saveok should be > 0. If saveok stays 0, the fields above say why. */
    g_pc_links++;
    { static unsigned s_statc = 0;
      if ((s_statc++ & 15u) == 0u) {
          char b[320];
          int n = snprintf(b, sizeof(b),
              "hits=%u misses=%u saveok=%u loadfail=%u\nlinks=%u key0=%u notlinked=%u nobin=%u openfail=%u lastopen=0x%08X\nloadfail>0 = saved binary won't restore (real bug); loadfail=0 = misses are just new/uncached content\n",
              g_progcache_hits, g_progcache_misses, g_pc_saveok, g_pc_loadfail, g_pc_links,
              g_pc_key0, g_pc_notlinked, g_pc_nobin, g_pc_openfail, (unsigned)g_pc_lastopen);
          SceUID df = sceIoOpen("ux0:data/mcsm/progcache_diag.txt",
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
          if (df >= 0) { if (n > 0) sceIoWrite(df, b, (SceSize)n); sceIoClose(df); } } }
    uint64_t pkey = progcache_key_for_program(program);
    if (pkey && progcache_try_load(program, pkey)) {
        /* Linked from disk — NO ShaccCg compile this run (the freeze killer). */
        g_progcache_hits++;
        if (g_progcache_hits <= 80U)
            l_info("progcache HIT prog=%u key=%08X%08X (hits=%u miss=%u)", program,
                   (unsigned)(pkey >> 32), (unsigned)(pkey & 0xffffffffu),
                   g_progcache_hits, g_progcache_misses);
        log_program_link_failure(program);
        program_cache_refresh(program);
        return;
    }
    launch_state_mark_gl_phase(2);
    uint64_t _clt0 = sceKernelGetSystemTimeWide();
    glLinkProgram(program);
    launch_state_mark_gl_phase(0);
    /* ASYNC-DESIGN DIAG (2026-07-20): record the compiling thread + duration on each
     * cache MISS (the blocking ShaccCg compile). Tells us whether compiles run on the
     * render/present thread (=> async needed) or a loader thread, how long each takes,
     * and how they cluster (scene-load burst vs scattered gameplay). First 200/session,
     * ux0:data/mcsm/shader_compile_diag.txt. Pure diagnostic. */
    { extern int sceKernelGetThreadId(void);
      static unsigned _cc = 0;
      unsigned _dtms = (unsigned)((sceKernelGetSystemTimeWide() - _clt0) / 1000ull);
      if (++_cc <= 200u) {
          char b[112]; int n = snprintf(b, sizeof(b), "#%u tid=0x%08X dt=%ums scene=%d present=%u\n",
              _cc, (unsigned)sceKernelGetThreadId(), _dtms,
              launch_state_scene_active(), (unsigned)launch_state_get_present_count());
          int flags = (_cc == 1u) ? (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC)
                                  : (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND);
          SceUID f = sceIoOpen("ux0:data/mcsm/shader_compile_diag.txt", flags, 0777);
          if (f >= 0) { if (n > 0) sceIoWrite(f, b, (SceSize)n); sceIoClose(f); } } }
    if (pkey) {
        g_progcache_misses++;
        dump_miss_sources(program, pkey);
        progcache_save(program, pkey);
        if (g_progcache_misses <= 80U)
            l_info("progcache MISS prog=%u key=%08X%08X compiled+saved (hits=%u miss=%u)", program,
                   (unsigned)(pkey >> 32), (unsigned)(pkey & 0xffffffffu),
                   g_progcache_hits, g_progcache_misses);
    } else {
        g_pc_key0++;
        static unsigned s_uncached = 0;
        s_uncached++;
        if (s_uncached <= 8U || gl_verbose_diag_enabled()) {
            l_info("progcache UNCACHED prog=%u missing tracked shader source (uncached=%u hits=%u miss=%u)",
                   program, s_uncached, g_progcache_hits, g_progcache_misses);
        }
    }
#else
    glLinkProgram(program);
#endif
    log_program_link_failure(program);
    program_cache_refresh(program);
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    (void)length;
    glShaderSource(shader, 1, &string, NULL);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif


/*
 * ====================================================================
 * PVR_PSP2 path: reimplement gl_init / gl_preload / gl_swap
 * ====================================================================
 */
#ifdef USE_PVR_PSP2
#include "utils/pvr_init.h"

void gl_init(void) {
    /*
     * PVR EGL context is set up once during pvr_init_gl,
     * so gl_init is a no-op here.  If the framebuffer size
     * changes between preload and init the caller should
     * call pvr_init_gl directly with the new dimensions.
     */
}

void gl_preload(void) {
    /* no-op: PVR modules are loaded during soloader_init_all */
}

void gl_swap(void) {
    (void)pvr_swap_buffers();
}

/* Passthrough stubs needed by dynlib.c; in PVR mode the game renders directly
 * to the display FBO 0 so these are identity operations. */
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    glBindFramebuffer(target, framebuffer);
}
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
}
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
}
#endif /* USE_PVR_PSP2 */
