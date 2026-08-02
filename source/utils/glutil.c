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
#include "utils/config.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/launch_state.h"
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
#include <psp2/display.h>            /* sceDisplayWaitVblankStartMulti — exact frame pacing */

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

static const char k_gl_vendor[] = "Imagination Technologies";

/* GPU IDENTITY -> ENGINE QUALITY TIER (2026-07-29).
 * libGameEngine.so carries a GPU database and keys its quality decisions off the
 * GL_RENDERER string. The names it knows are, from weakest to strongest:
 *     PowerVR SGX 540 / 541 / 542 / 543 / 543MP / 544 / 544MP
 *     Mali-400, Adreno (TM) 2 / 30 / 305 / 320 / 330, Tegra 3, NVIDIA X1
 * We were reporting "PowerVR SGX 543MP" -- an iPad-3-class part, high in that
 * list -- so the game recognised a capable device and configured itself to match.
 * That is a real reason it boots at maximum render quality on hardware weaker
 * than anything in the table.
 *
 * `gpu_tier` in graphics.txt now picks which identity we present:
 *     0 -> "PowerVR SGX 540"   weakest PowerVR the engine knows
 *     1 -> "PowerVR SGX 541"
 *     2 -> "PowerVR SGX 543"
 *     3 / -1 -> "PowerVR SGX 543MP"  (unchanged default, the Vita's real part)
 * Deliberately staying inside the PowerVR family: the engine has vendor-specific
 * paths, and claiming to be an Adreno or Mali could send it down a branch written
 * for hardware we are not. Same family, lower rung -- the engine's own low-spec
 * configuration, which is what a budget PowerVR phone would get.
 *
 * NOTE: this is the SECOND thing tried for tier selection. The Lua-side
 * PlatformGetGPUQuality hook was the obvious candidate and is a confirmed dead
 * end -- device logs show MCSM never calls it (nor RenderSetQualityLevel /
 * RenderSetCurQualityLevel / RenderGetQualityLevels). The renderer string is
 * consulted; those functions are not. */
static const char *mcsm_gl_renderer(void) {
    /* gpu_name wins when set. The engine's GPU table is a RANK table, not a
     * quality gradient -- it maps each device string to a number (SGX 544MP=16,
     * 543MP=15, 544=13, 543=12, 542=8, 541=7, 540=6, Mali-400=5, Adreno 305=4,
     * Tegra 3=2, GC1000=1) and then reduces that rank to a binary LOW/HIGH
     * RenderQualityType. gpu_tier only reaches four PowerVR entries; naming the
     * device directly reaches every rank, including ranks below anything PowerVR
     * offers, which is the only way to test where the LOW/HIGH threshold sits. */
    const char *n = mcsm_cfg()->gpu_name;
    if (n && n[0]) return n;
    switch (mcsm_cfg()->gpu_tier) {
        case 0:  return "PowerVR SGX 540";
        case 1:  return "PowerVR SGX 541";
        case 2:  return "PowerVR SGX 543";
        default: return "PowerVR SGX 543MP";
    }
}
#define k_gl_renderer (mcsm_gl_renderer())
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
#ifdef DEBUG_SOLOADER
    uint32_t flags;
    char stage[8];
    char sha1[48];
    char source_path[256];
    char preview[224];
#endif
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

/* ☠ DEFINED OUTSIDE THE GUARD ABOVE, ON PURPOSE. This used to sit INSIDE the
 * `#ifndef MCSM_FAST_FINAL_RUNTIME` block, so building the documented diagnostic
 * configuration -- `-DMCSM_FAST_FINAL_RUNTIME=0`, which is exactly what the SIMSPLIT
 * log line tells you to do -- skipped the block, left the macro undefined, and made
 * all seven `MCSM_DIAG_HELPER static ...` declarations fail to parse. The one build
 * this marker exists to serve was the one build it broke.
 *
 * Two separate reasons a helper carries it, which the old comment collapsed into one:
 *   - some are used only by the diagnostic (#else) branches, compiled out at =1;
 *   - some (texlru_is_live, the 565/4444/zero-fill converters) are reachable only on
 *     the PVR backend, i.e. gated on USE_PVR_PSP2, not on this flag at all. */
#define MCSM_DIAG_HELPER __attribute__((unused))

/* A static diagnostic counter is still an observable write even when every log
 * consuming it compiles out. Keep real counts in logging/profiling builds, but
 * make them compile-time constants in the shipping fast path. */
#if defined(DEBUG_SOLOADER) || !MCSM_FAST_FINAL_RUNTIME
#define MCSM_DIAG_COUNT(name) static unsigned int name = 0; name++
#else
#define MCSM_DIAG_COUNT(name) enum { name = 0 }
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

#ifdef DEBUG_SOLOADER
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
#endif

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

#ifdef DEBUG_SOLOADER
static int shader_file_diag_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        initialized = 1;
        enabled = 0; /* shader-source diagnostics removed — off by default */
    }
    return enabled;
}
#endif

#ifdef DEBUG_SOLOADER
static int gl_verbose_diag_enabled(void) {
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        initialized = 1;
        enabled = 0; /* verbose GL/texture diagnostics removed — off by default */
    }
    return enabled;
}
#else
#define gl_verbose_diag_enabled() 0
#endif

static void track_shader_source(GLuint shader, const char *source, size_t length) {
    shader_diag_entry *entry;
#ifdef DEBUG_SOLOADER
    char *sha1 = NULL;
#endif

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

#ifdef DEBUG_SOLOADER
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
#endif
}

#ifdef DEBUG_SOLOADER
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
#endif

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
#ifdef DEBUG_SOLOADER
    log_shader_diag_context(shader, (GLenum)shader_type);
#endif

    if (log_buf != stack_log) {
        free(log_buf);
    }
}

#ifdef DEBUG_SOLOADER
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
#endif

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

/* Drop every memoised uniform-array split for `program`.
 *
 * ☠ THE MEMO MUST DIE WITH THE LAYOUT IT DESCRIBES. g_uniform_split_memo caches
 * per-element uniform LOCATIONS keyed on (program, location, count), and a relink can
 * assign completely different locations to the same names. program_cache_refresh() is
 * the single point where a program's uniform layout is (re)established -- it is called
 * from both arms of glLinkProgram_soloader, including the progcache-hit path -- so any
 * cache derived from that layout has to be invalidated here or it silently outlives it.
 * Without this the memo keeps answering with the PREVIOUS link's locations after a
 * relink (or after a deleted program's GLuint is recycled by a new one), writing
 * uniforms to the wrong slots: wrong-looking shading, no error, nothing logged. */
static void uniform_split_memo_forget(GLuint program);

static void program_cache_refresh(GLuint program) {
    GLint link_status = GL_FALSE;
    GLint active_uniforms = 0;
    GLint max_name_len = 0;
    char name_buf[PROGRAM_UNIFORM_NAME_CAP];
    char norm_name[PROGRAM_UNIFORM_NAME_CAP];
    GLsizei out_len = 0;
    GLint size = 0;
    GLenum type = 0;

    /* Invalidate BEFORE repopulating: the memo describes the OLD layout. */
    uniform_split_memo_forget(program);

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
#ifdef DEBUG_SOLOADER
    static unsigned s_logged = 0;
    if (s_logged++ < 64U || program == 19U) {
        l_info("glLinkProgram(%u): cached %d active uniforms (reported=%d maxName=%d)",
               program,
               cache->uniform_count,
               active_uniforms,
               max_name_len);
    }
#endif
}

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name) {
    if (!name) {
#ifdef DEBUG_SOLOADER
        static unsigned s_null_logged = 0;
        if (s_null_logged++ < 8U) {
            l_warn("glGetUniformLocation(%u, NULL) skipped", program);
        }
#endif
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

typedef struct UniformSplitMemo {
    GLuint prog;
    GLint loc;
    GLsizei cnt;
    signed char state; /* 0 empty, 1 split, 2 no-split */
    unsigned used;
    GLint el[64];
} UniformSplitMemo;

static UniformSplitMemo g_uniform_split_memo[256];
static unsigned g_uniform_split_clock = 0;
static unsigned g_uniform_split_index[1024];

/* See the note on program_cache_refresh(). Linear over 256 slots, but this runs only
 * on a link -- a handful of times per boot -- never on the per-draw path. The direct-
 * mapped index is cleared alongside, or a stale slot number could still be reached
 * through it after the slot is reused for a different program. */
static void uniform_split_memo_forget(GLuint program) {
    for (unsigned i = 0; i < sizeof(g_uniform_split_memo) / sizeof(g_uniform_split_memo[0]); i++) {
        if (g_uniform_split_memo[i].state && g_uniform_split_memo[i].prog == program) {
            g_uniform_split_memo[i].state = 0;
            g_uniform_split_memo[i].prog = 0;
            g_uniform_split_memo[i].loc = -1;
            g_uniform_split_memo[i].cnt = 0;
        }
    }
    for (unsigned h = 0; h < sizeof(g_uniform_split_index) / sizeof(g_uniform_split_index[0]); h++) {
        const unsigned slot1 = g_uniform_split_index[h];
        if (slot1 && !g_uniform_split_memo[slot1 - 1].state) {
            g_uniform_split_index[h] = 0;
        }
    }
}

/* Keep the large name/location scratch arrays off the ~860-call/frame cached-hit
 * path. GCC otherwise gives every hit a ~380-byte stack frame and a wide register
 * save merely because the first-encounter resolver lives in the same function. */
__attribute__((noinline))
static int gl_uniform4fv_split_resolve(GLuint program, GLint location, GLsizei count,
                                       const GLfloat *value, unsigned hkey, int empty) {
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
        for (int i = 0; i < 256; ++i) {
            if (g_uniform_split_memo[i].used < oldest) {
                oldest = g_uniform_split_memo[i].used;
                slot = i;
            }
        }
    }
    g_uniform_split_memo[slot].prog = program;
    g_uniform_split_memo[slot].loc = location;
    g_uniform_split_memo[slot].cnt = count;
    g_uniform_split_memo[slot].used = ++g_uniform_split_clock;
    g_uniform_split_index[hkey] = (unsigned)(slot + 1);
    if (splittable) {
        g_uniform_split_memo[slot].state = 1;
        for (GLsizei i = 0; i < count; ++i) {
            g_uniform_split_memo[slot].el[i] = element_locations[i];
        }
        for (GLsizei i = 0; i < count; ++i)
            glUniform4fv(element_locations[i], 1, value + ((size_t)i * 4U));
        return 1;
    }
    g_uniform_split_memo[slot].state = 2;
    return 0;
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
     * per (program, base location, count). Capacity 256 covers the measured full-game
     * key set; a 1024-slot direct index makes steady-state lookup approximately O(1),
     * with a full-key-checked linear fallback so collisions cannot return bad state. */
    const unsigned hkey = (((unsigned)program * 2654435761u) ^
                           ((unsigned)location * 40503u) ^
                           ((unsigned)count * 2246822519u)) & 1023u;
    int hit = -1;
    int empty = -1;
    {
        const int cand = (int)g_uniform_split_index[hkey] - 1;
        if (cand >= 0 && g_uniform_split_memo[cand].state &&
            g_uniform_split_memo[cand].prog == program &&
            g_uniform_split_memo[cand].loc == location &&
            g_uniform_split_memo[cand].cnt == count) {
            hit = cand;
        }
    }
    if (hit < 0) {
        for (int i = 0; i < 256; ++i) {
            if (g_uniform_split_memo[i].state &&
                g_uniform_split_memo[i].prog == program &&
                g_uniform_split_memo[i].loc == location &&
                g_uniform_split_memo[i].cnt == count) {
                hit = i;
                break;
            }
            if (!g_uniform_split_memo[i].state && empty < 0) {
                empty = i;
            }
        }
    }
    if (hit >= 0) {
        g_uniform_split_index[hkey] = (unsigned)(hit + 1);
        g_uniform_split_memo[hit].used = ++g_uniform_split_clock;
        if (g_uniform_split_memo[hit].state == 2) {
            return 0;
        }
        for (GLsizei i = 0; i < count; ++i) {
            glUniform4fv(g_uniform_split_memo[hit].el[i], 1,
                         value + ((size_t)i * 4U));
        }
        return 1;
    }

    return gl_uniform4fv_split_resolve(program, location, count, value, hkey, empty);
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

/* Per-frame draw stats exist only in builds that can emit DIP-RENDER. */
#ifdef DEBUG_SOLOADER
unsigned int g_frame_draw_calls = 0;
unsigned long g_frame_draw_verts = 0;
#endif

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

/* ---- Render-scale (opt-in via ux0:data/mcsm/graphics.txt (resolution) = "WxH") --------
 * Render the game into a low-res FBO and bilinear-upscale it to the native
 * 960x544 display on present. Fewer fragment-shaded pixels => higher fps on this
 * fragment-bound 3D engine, while the picture stays native/fullscreen.
 * SAFE BY DEFAULT: with no override file the game's render res == native, so this
 * stays inactive and rendering goes straight to the display (the working path).
 * TUNABLE with no rebuild: edit graphics.txt (resolution) (e.g. 640x363, 576x326, 480x272). */
#define RS_NATIVE_W 960
#define RS_NATIVE_H 544
static GLboolean g_rs_active = GL_FALSE;
static GLuint g_rs_fbo = 0, g_rs_color = 0, g_rs_depth = 0;
static int g_rs_w = 0, g_rs_h = 0;
static GLint g_rs_blit_filter = GL_LINEAR;   /* graphics.txt `upscale` */
static int g_rs_blit_flip = 0;               /* immutable after config load */


static void rs_init(int w, int h) {
    glGenTextures(1, &g_rs_color);
    glBindTexture(GL_TEXTURE_2D, g_rs_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    /* graphics.txt `upscale`. Applied to the FBO texture AND the blit below so both
     * sampling points agree. NEAREST keeps glyph edges hard; LINEAR smears them. */
    const McsmCfg *cfg = mcsm_cfg();
    g_rs_blit_filter = cfg->upscale_nearest ? GL_NEAREST : GL_LINEAR;
    g_rs_blit_flip = cfg->blit_flip;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_rs_blit_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_rs_blit_filter);
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
static int g_present_period_us = 0;   /* whole-microsecond part of the present period */
/* ★ Whole vblanks per presented frame, or 0 if the cap is genuinely fractional.
 *
 * ☠ THIS IS *NOT* DERIVABLE FROM THE MICROSECOND REMAINDER, and assuming it was is
 * what broke 30fps. The old test asked `1000000 % fps`, i.e. "is the period a whole
 * number of MICROSECONDS" -- a completely different question. It gets the answer wrong
 * in BOTH directions:
 *     fps 30: 1e6%30=10 -> "fractional"  but 33333us IS exactly 2 vblanks
 *     fps 15: 1e6%15=10 -> "fractional"  but            4 vblanks
 *     fps 60: 1e6%60=40 -> "fractional"  but            1 vblank
 *     fps 40: 1e6%40=0  -> "WHOLE"       but 25000us is 1.5 vblanks -- not whole at all
 * So every shipped profile (all capped at 30) took the fractional microsecond-timeline
 * path and never used vblank counting, which is exactly the sleep-based guessing the
 * counter exists to replace. Device histogram with that bug live:
 *     vblanks 1:114 2:306 3:142 4:34   <- frames landing on one, two, three and four
 * The right question is whether the DISPLAY can express the rate: 60 % fps == 0. */
static int g_present_vb_whole = 0;
static int g_present_period_rem = 0;  /* numerator of the leftover fraction (rem/den us) */
static int g_present_period_den = 0;  /* denominator; 0 = no fractional part */

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
        FILE *rf = mcsm_open_setting("vram_reserve.txt", "r");
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
    { FILE *nt = mcsm_open_setting("no_gxm_tune.txt", "r");
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
     * Baked-in defaults: the workload is CPU-bound + upload-heavy, so cached GL pools
     * (faster CPU->GPU uploads; vitaGL flushes caches before GPU reads) and pinning
     * vitaGL's GC thread to core 3 (0x80000, freed by the shipped capUnlocker) so it
     * doesn't steal render cycles are both real wins. vglUseCachedMem must precede vglInit. */
    vglUseCachedMem(GL_TRUE);                    /* faster CPU->GPU uploads (baked-in default) */
    /* GC thread affinity: core 3 ONLY if capUnlocker actually freed it this boot,
     * else the 3 user cores. A hardcoded core-3 pin makes the GC unschedulable on
     * any Vita without capUnlocker -> GC never runs -> vglSwapBuffers wedges on the
     * first menu frame (the "works on my Vita, hangs on testers'" stall). */
    extern int mcsm_gc_core_mask(void);
    int gc_mask = mcsm_gc_core_mask();
    vglSetupGarbageCollector(160, gc_mask);
    l_info("gl_init: cached-mem + GC affinity=0x%05X (%s)", (unsigned)gc_mask,
           (gc_mask & 0x00080000) ? "core3 via capUnlocker" : "user cores (no capUnlocker)");
    /* The device-tested vitaGL rollback intentionally has vglSetupScratchMemory as
     * a `bx lr` stub. Later pool-enabled archives regressed orientation, UI alpha,
     * and textures, so do not claim or enable that path in the shipping build. The
     * configure step prints the linked archive hash for unambiguous provenance. */
    l_info("gl_init: vitaGL scratch pool DISABLED (known-good compatibility rollback)");

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
    if (!mcsm_cfg()->vsync) { vglWaitVblankStart(GL_FALSE); l_info("presenter: vsync OFF (graphics.txt) — max throughput"); }
    else                    { vglWaitVblankStart(GL_TRUE);  l_info("presenter: vsync ON (precise vblank lock)"); }

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
        /* 2026-06-30 (ANTI-STUTTER, superseded below): the runtime shader compiler
         * used a heavy default opt level, so every new Telltale shader took
         * 400-900ms to compile mid-gameplay = the "mega stutter" (each
         * glLinkProgram was immediately followed by a 400-900ms sim freeze).
         * SHARK_OPT_SLOW (O0) traded shader speed for compile speed to kill it.
         *
         * 2026-07-29: RAISED TO O2. Two things changed that invert that trade:
         *
         *   1. The compile cost is no longer paid repeatedly. The progcache
         *      (glGetProgramBinary -> ux0:data/mcsm_progcache) now actually hits
         *      -- 274 programs cached on the test console -- since the key-drift
         *      bug was fixed. A shader compiles ONCE per console, ever, and is
         *      reloaded from disk afterwards. The O0 penalty, by contrast, was
         *      being paid on every frame of every session forever.
         *
         *   2. The bottleneck moved. O0 was chosen while the console was
         *      CPU-bound (sim 49-71ms/frame with the GPU idle), so shader quality
         *      was irrelevant. At the current performance profile (576x326,
         *      uncapped, 47.5 fps avg) the GPU is now the limit -- so the code
         *      the GPU runs is suddenly what matters.
         *
         * O0 means no instruction scheduling and no register-allocation work,
         * which on the USSE costs occupancy as well as instructions.
         * SHARK_OPT_DEFAULT is O2 -- the compiler's own default, not the risky
         * -Ofast. fastmath/fastprecision/fastint stay on as before; those already
         * carry whatever precision risk exists here.
         *
         * COST: the first launch after this change recompiles the cache, so
         * shaders stutter once each as they are re-encountered, then never again.
         * PROGCACHE_MAGIC is bumped in lockstep -- without that the O0 binaries
         * already on disk would keep loading and this change would do nothing. */
        {
            static const char *opt_name[5] = { "O0/SLOW", "O1/SAFE", "O2/DEFAULT", "O3/FAST", "Ofast/UNSAFE" };
            int so = mcsm_cfg()->shader_opt;
            if (so < 0 || so > 4) so = 2;
            vglSetupRuntimeShaderCompiler((shark_opt)so, GL_TRUE, GL_TRUE, GL_TRUE);
            l_info("gl_init: shader compiler -> %s (shader_opt=%d; progcache keyed per opt level, "
                   "so opt 0 reuses the pre-existing cache verbatim)", opt_name[so], so);
        }
        l_info("gl_init: forced is_shark_online=1 (runtime shader compiler up via malloc)");
    } else {
        l_warn("gl_init: shark_init FAILED (%d) — shaders will not compile", sk);
    }

    /* Only spin up the low-res FBO if the game's render res is BELOW native
     * (graphics.txt (resolution) set it). Otherwise leave the direct-to-display path alone. */
    if (render_w < RS_NATIVE_W || render_h < RS_NATIVE_H) {
        rs_init(render_w, render_h);
    }

    /* Present-side frame cap from graphics.txt (read once, here, where file I/O
     * is safe — NOT in gl_swap). */
    {
        int fps = mcsm_cfg()->fps_cap;
        if (fps > 0 && fps <= 120) {
                /* VBLANK-QUANTIZED PRESENT LOCK (2026-07-17): a plain 1000000/fps
                 * period lands the sleep-release right ON a vblank boundary; adaptive
                 * vsync then misses it by a hair and snaps to the NEXT vblank -> idle
                 * 30fps frames were beating to 50ms (20fps). Quantize to the nearest
                 * whole vblank count and UNDERSHOOT by ~2.5ms so the frame is always
                 * ready before the target vblank and vsync catches it cleanly. This
                 * makes fps_cap=30 (or 33) both resolve to a rock-steady 30fps. */
                /* EXACT-TIMELINE PACING (2026-07-29). The quantize-to-whole-vblanks
                 * rule above cannot express a rate whose period is not a whole
                 * number of vblanks -- 24fps needs 60/24 = 2.5 -- so fps_cap=24
                 * rounded to k=2 and silently delivered 30. Rates that are not
                 * 60/N were therefore unreachable.
                 *
                 * Track the IDEAL presentation timeline instead of a fixed period:
                 * advance a target timestamp by the exact fractional period each
                 * frame and present at the first vblank at or after it. For 24fps
                 * the targets land at 2.5, 5.0, 7.5, 10.0 vblanks, so vsync catches
                 * them on vblanks 3, 5, 8, 10 -- a repeating 3:2:3:2 cadence
                 * averaging exactly 24.000fps with no drift, which is precisely how
                 * 24fps film is shown on 60Hz displays.
                 *
                 * Whole-vblank rates are unaffected: 30fps still lands on every 2nd
                 * vblank and 20fps on every 3rd, identical to before, and they gain
                 * drift-free long-term accuracy since the target advances by the
                 * exact period rather than an integer approximation.
                 *
                 * g_present_period_us keeps its old meaning (nominal period, used
                 * as the enable flag and for the catch-up clamp); the fractional
                 * remainder rides alongside it. */
                g_present_period_us  = 1000000 / fps;
                g_present_period_rem = 1000000 % fps;   /* exact period = whole + rem/fps */
                g_present_period_den = fps;
                g_present_vb_whole   = (fps > 0 && fps <= 60 && (60 % fps) == 0) ? (60 / fps) : 0;
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
          if (npl) { fclose(npl); g_present_period_us = 0; g_present_period_rem = 0; g_present_period_den = 0;
                     l_info("present frame-lock DISABLED (no_present_lock.txt) — vsync+sim-pace only"); } }
        if (g_present_vb_whole > 0)
            l_info("present frame-lock = %d vblanks/frame (%d fps, EXACT — paced by vblank count)",
                   g_present_vb_whole, 60 / g_present_vb_whole);
        else
            l_info("present frame-lock = %d+%d/%d us per frame (fractional cadence — not a whole "
                   "number of vblanks, so it rides a pulldown)", g_present_period_us,
                   g_present_period_rem, g_present_period_den ? g_present_period_den : 1);
    }

    /* TROPHIES — must be here, and only here. sceNpTrophy's one-time setup dialog is
     * a Vita common dialog: it only advances while frames are being presented, so it
     * has to run on the thread that owns the GL context and after vglInit. gl_init is
     * the last point that is both. It fails soft (no NoTrpDrm / no TROPHY.TRP just
     * logs and disables unlocks), so boot is never at risk.
     *
     * ☠ THE FRAMEBUFFER MUST BE SAVED AND RESTORED AROUND IT. rs_init() deliberately
     * leaves g_rs_fbo bound so the game's first frame renders straight into the
     * render-scale FBO (see the comment there). The trophy setup dialog presents
     * frames itself, and the system composites the dialog over the DISPLAY buffer, so
     * it needs FB0 bound -- and leaving FB0 bound afterwards would send the engine's
     * first frame to the display instead of the FBO, breaking the whole render-scale
     * path on the one boot where the dialog appears. */
    {
        GLint saved_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        extern int mcsm_trophies_init(void);
        const int trc = mcsm_trophies_init();
        if (trc < 0) l_info("gl_init: trophies unavailable (rc=%d) — game continues normally", trc);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
        if (g_rs_active && saved_fbo != (GLint)g_rs_fbo)
            l_warn("gl_init: render-scale FBO was not bound before trophy init (was %d)", saved_fbo);
    }
}

void gl_swap() {
#ifdef DEBUG_SOLOADER
    static unsigned int s_swap_counter = 0;
#endif
    static unsigned char s_power_tick_countdown = 0;
    /* SAVE-RENAME KEYBOARD pump (2026-07-18): the Vita IME renders itself during
     * vglSwapBuffers; harvest the entered name when the user finishes + feed it
     * back to the engine as key events. Runs every present, near-free when idle. */
    { extern int mcsm_ime_poll(char **out); extern void mcsm_ime_deliver(const char *);
      char *ime_text = NULL;
      if (mcsm_ime_poll(&ime_text) && ime_text) mcsm_ime_deliver(ime_text); }
    /* First-boot trophy setup is a common dialog too, so it advances only while frames
     * are presented -- exactly like the IME above. Driving it here instead of a blocking
     * loop inside mcsm_trophies_init() is what keeps the first boot from stalling. */
    { extern int mcsm_trophies_setup_poll(void); (void)mcsm_trophies_setup_poll(); }
    /* Keep the Vita awake while the game runs. The idle/suspend timer has
     * second-level granularity, so ticking it ~1x/sec (every 30th present) fully
     * prevents auto-suspend/screen-blank while avoiding a wasted per-frame syscall
     * on the hottest path — a small but free battery win. */
    if (s_power_tick_countdown == 0u) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        s_power_tick_countdown = 29u;
    } else {
        s_power_tick_countdown--;
    }
    if (g_rs_active) {
        /* Bilinear-upscale the low-res render into the native display, THEN present.
         * draw FB is left at 0 so vglSwapBuffers presents the upscaled image.
         *
         * Y ORIENTATION IS vitaGL-VERSION DEPENDENT, so it is a runtime toggle.
         * On the long-shipped lib (aa75c61) an FBO->FB0 blit did NOT flip while
         * presenting FB0 directly DID, so this blit had to swap dstY0/dstY1 to come
         * out upright. Upstream 96c41a1 changed the FBO-bind path in gxm.c from
         * `glScissor(region...)` to `glViewport(gl_viewport...)`, and vitaGL applies
         * the FBO Y-flip THROUGH the viewport transform (which is exactly what the
         * adjacent `#ifndef HAVE_UNFLIPPED_FBOS change_cull_mode()` compensates for)
         * -- so the content now arrives already flipped and our swap double-flips it,
         * presenting the whole game upside-down.
         *
         * Rather than hardcode a guess that costs a full build+deploy round trip to
         * correct, read it from graphics.txt: `blit_flip = 0` (no swap, correct for
         * the new lib) or `1` (swap, correct for the old one). Default follows
         * whichever lib this build links. */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rs_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (g_rs_blit_flip)
            glBlitFramebuffer(0, 0, g_rs_w, g_rs_h, 0, RS_NATIVE_H, RS_NATIVE_W, 0,
                              GL_COLOR_BUFFER_BIT, g_rs_blit_filter);
        else
            glBlitFramebuffer(0, 0, g_rs_w, g_rs_h, 0, 0, RS_NATIVE_W, RS_NATIVE_H,
                              GL_COLOR_BUFFER_BIT, g_rs_blit_filter);
    }
    /* PRESENT-SIDE FRAME LOCK (anti-stutter 2026-06-30): the render thread
     * presents independently of the sim, so on a heavy 3D scene frames land at
     * 18-25ms and vsync snaps each to either 16.7ms or 33ms -> the DISPLAY beats
     * between 60 and 30fps = persistent judder, even on a static camera (the sim
     * pace alone can't fix this — it doesn't gate the present). Enforce a steady
     * minimum present interval so the display rate is consistent (no beating).
     * Period = 1/graphics.txt fps_cap (e.g. 30 -> 33.3ms). A cap of 0 is off. */
    if (g_present_period_us > 0 && g_present_vb_whole > 0) {
        /* ★★★ EXACT PACING BY VBLANK COUNT. This is the whole fix.
         *
         * Every rate this port ships is a whole number of vblanks, so the frame's
         * present slot is a COUNT, not a deadline in microseconds. sceDisplayGetVcount()
         * is that count, straight from the display hardware, so scheduling against it
         * cannot drift and cannot land on the wrong side of a boundary the way a sleep
         * can.
         *
         * The subtlety that matters: vglSwapBuffers ALREADY waits one vblank (vsync is
         * on). So to present on vblank N we must wake at N-1 and let the swap consume
         * the last one. Waiting a fixed (target-1) here would be wrong whenever
         * rendering has already eaten vblanks -- it would ADD to an over-budget frame
         * and turn a 2-vblank cap into 3. Waiting `remaining-1`, computed from the live
         * counter, is right in every case: a frame that is already late waits zero.
         *
         * Missed frames resync instead of accumulating debt, so a slow patch can never
         * produce a catch-up burst afterwards. */
        static unsigned s_next_vc = 0;

        const unsigned vc = (unsigned)sceDisplayGetVcount();
        const int vb = g_present_vb_whole;
        if (!s_next_vc) s_next_vc = vc + (unsigned)vb;

        const int remaining = (int)(s_next_vc - vc);   /* signed: survives wraparound */

        /* NEVER AUTO-DOWNSHIFT THE USER'S CAP.
         *
         * The old hysteresis widened `vb` after only four late frames and held the
         * wider cadence for 90 frames. A 60 FPS request therefore became 30 after
         * four ordinary >16.7 ms frames, then became 20 after four >33.3 ms frames.
         * The same mechanism pulled a 30 FPS profile to 20 during a short busy patch
         * and kept it there after the scene recovered. That is why every profile
         * appeared hard-locked to 20 regardless of the selected cap.
         *
         * A cap is a maximum presentation cadence, not a dynamic quality governor.
         * A genuinely slow frame can still miss its slot and present on a later
         * vblank, but the following frame immediately targets the configured cadence
         * again. This preserves natural 30/60 recovery without catch-up bursts. */

        if (remaining > 1) {
            sceDisplayWaitVblankStartMulti((unsigned int)(remaining - 1));
        }
        /* Schedule the next slot. Late frames present at vc+1 (the swap's own wait), so
         * the next slot is measured from there rather than from a target we already
         * missed -- that is what stops debt accumulating. */
        s_next_vc = (remaining > 0 ? s_next_vc : vc + 1u) + (unsigned)g_present_vb_whole;
    } else if (g_present_period_us > 0) {
        /* FRACTIONAL cap (24, 40, ...): no single vblank count expresses it, so keep the
         * exact microsecond timeline with its remainder carry. Reachable only by hand
         * from graphics.txt -- every shipped profile is a whole-vblank rate. */
        static uint64_t s_target_us = 0;
        static int      s_rem_acc   = 0;
        uint64_t pnow = sceKernelGetSystemTimeWide();
        if (!s_target_us) s_target_us = pnow;
        s_target_us += (uint64_t)g_present_period_us;
        if (g_present_period_den) {
            s_rem_acc += g_present_period_rem;
            if (s_rem_acc >= g_present_period_den) { s_rem_acc -= g_present_period_den; s_target_us++; }
        }
        if (pnow > s_target_us + (uint64_t)g_present_period_us) { s_target_us = pnow; s_rem_acc = 0; }
        const uint64_t undershoot = 2500;
        uint64_t wake = (s_target_us > undershoot) ? s_target_us - undershoot : 0;
        if (pnow < wake) sceKernelDelayThread((SceUInt)(wake - pnow));
    }
    launch_state_mark_gl_phase(1);   /* in vglSwapBuffers (present) */
    /* ★★★ has_commondialog MUST be GL_TRUE WHILE THE IME IS UP, OR IT IS INVISIBLE.
     *
     * This was hardcoded GL_FALSE, and that alone defeated every save-rename attempt
     * ever made -- through the Lua hook, the JNI hook, the SELECT button and the vkbd
     * vtable bridge alike. A Vita common dialog does not draw itself: it is composited
     * by the presenter, and ONLY when the presenter is told one is up. So
     * sceImeDialogInit succeeded, the dialog genuinely existed, mcsm_ime_poll polled
     * it every frame -- and nothing was ever put on screen. "The keyboard doesn't show
     * up" was literally true, and no amount of hunting for a better TRIGGER could have
     * fixed it, because the trigger was never the broken part.
     *
     * The tell was in plain sight: dialog.c's own blocking helper presents with
     * GL_TRUE (which is why fatal-error dialogs DO appear), and the trophy setup
     * dialog only advanced because its loop passed 1 explicitly. This path never did.
     *
     * Cost is one global read per present. Kept as a live check rather than a latch
     * because the dialog can end on any frame. */
    { extern int mcsm_ime_needs_compositing(void);
      extern int mcsm_trophies_setup_active(void);
      const int dlg = mcsm_ime_needs_compositing() || mcsm_trophies_setup_active();
      vglSwapBuffers(dlg ? GL_TRUE : GL_FALSE); }
    launch_state_mark_gl_phase(0);   /* present returned */
    if (g_rs_active) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_rs_fbo); /* next frame renders into the FBO again */
    }
#ifdef DEBUG_SOLOADER
    s_swap_counter++;
#endif

    /* DIP PROFILER: log only severe render stalls with the draw-call /
     * vertex load that frame, so a sustained stutter shows whether it's
     * draw/GPU-bound (high draws) or stalling elsewhere (low draws). Cheap:
     * fires only on dips, and the logger is buffered. */
    const uint64_t present_now_us = sceKernelGetSystemTimeWide();
    {
        static uint64_t s_last_us = 0;
        const uint64_t now_us = present_now_us;
        if (s_last_us) {
            uint64_t dt_us = now_us - s_last_us;
            /* Publish the true present cadence for the clock governor (every frame,
             * all builds — NOT gated by logging). */
            g_mcsm_present_dt_us = (dt_us > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)dt_us;
#ifdef DEBUG_SOLOADER
            uint32_t dt_ms = (uint32_t)(dt_us / 1000ULL);
            /* DIAG: log gameplay DIPS (>40ms = <25fps), throttled 1-in-16 to avoid
             * self-slowing, WITH draws/verts + VRAM so we can read CPU-vs-GPU-vs-VRAM. */
            static unsigned s_dipc = 0;
            if (dt_ms > 40U && (s_dipc++ & 0xFU) == 0U) {
                l_info("DIP-RENDER frame=%u dt=%ums draws=%u verts=%lu VRAM=%uKB RAM=%uKB",
                       s_swap_counter, dt_ms, g_frame_draw_calls, g_frame_draw_verts,
                       (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024u));
            }

            /* ★ PACING HISTOGRAM -- what the frame rate FEELS like, not what it
             * averages to.
             *
             * An average is the wrong statistic for smoothness and actively misleads
             * here. fps_cap 24 on a 60Hz panel cannot be even: 60/24 = 2.5 vblanks,
             * so a perfectly-paced 24 is a 3:2 pulldown alternating 33ms and 50ms.
             * Any counter averaging over a second reports a truthful "24" while the
             * motion carries the 50ms frames -- which is why 24 can measure right and
             * feel like 20 or worse. A burst after a heavy frame does the same thing
             * in reverse: some 16ms frames and some 80ms ones still average to 24.
             *
             * So bucket the actual present intervals by the vblank they landed on and
             * dump the distribution periodically. p50/p95/max plus the bucket spread
             * say immediately whether a rate is EVEN (one or two adjacent buckets) or
             * merely correct-on-average (spread across many), and no other line in
             * this log can tell those apart. Costs one compare chain per frame. */
            {
                static uint32_t s_bucket[8];   /* 1..7 vblanks, [0] = over 7 */
                static uint32_t s_n, s_max_ms, s_sum_ms;
                const uint32_t vb = (dt_ms + 8U) / 17U;   /* nearest whole vblank */
                s_bucket[(vb >= 1U && vb <= 7U) ? vb : 0U]++;
                s_n++; s_sum_ms += dt_ms;
                if (dt_ms > s_max_ms) s_max_ms = dt_ms;
                if (s_n >= 600U) {           /* ~10-25s depending on rate */
                    /* p50/p95 straight from the buckets, in vblank units. */
                    uint32_t acc = 0, p50 = 0, p95 = 0;
                    for (uint32_t i = 1; i <= 7U; i++) {
                        acc += s_bucket[i];
                        if (!p50 && acc * 2U >= s_n)   p50 = i;
                        if (!p95 && acc * 20U >= s_n * 19U) { p95 = i; break; }
                    }
                    l_info("PACING n=%u avg=%ufps p50=%uvb(%ufps) p95=%uvb(%ufps) max=%ums | "
                           "vblanks 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 7+:%u",
                           s_n, s_sum_ms ? (1000U * s_n / s_sum_ms) : 0U,
                           p50, p50 ? 60U / p50 : 0U,
                           p95, p95 ? 60U / p95 : 0U, s_max_ms,
                           s_bucket[1], s_bucket[2], s_bucket[3], s_bucket[4],
                           s_bucket[5], s_bucket[6], s_bucket[7], s_bucket[0]);
                    for (uint32_t i = 0; i < 8U; i++) s_bucket[i] = 0;
                    s_n = 0; s_max_ms = 0; s_sum_ms = 0;
                }
            }
#endif /* DEBUG_SOLOADER */
        }
        s_last_us = now_us;
#ifdef DEBUG_SOLOADER
        g_frame_draw_calls = 0;
        g_frame_draw_verts = 0;
#endif
    }
    // Record a real frame-present heartbeat so telemetry/watchdog can tell
    // "actually rendering" apart from "input loop ticking over a black screen".
    launch_state_mark_present_at_us(present_now_us);
#ifdef DEBUG_SOLOADER
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
    /* LOW-VRAM EARLY WARNING (2026-07-25). The engine never calls glDeleteTextures
     * and vitaGL's texture eviction is deliberately disabled (enabling it deleted
     * textures the engine was still using -> garbage blocks + upload thrash), so
     * GPU texture memory only grows. Measured across a 79-minute session: 80MB ->
     * 8MB free, transiently 0, but with ZERO GL_OUT_OF_MEMORY errors -- i.e. a
     * standing risk, not a live failure, which is why eviction has NOT been
     * re-enabled on a guess. Warn once per threshold crossing so that if it ever
     * does start failing, the log says so plainly instead of surfacing as a
     * mystery black screen (the documented end state: an upload OOMs, the render
     * thread cannot finish the frame, and FinishFrame waits forever). Cheap: the
     * query only runs on the same throttled tick as the log above. */
    {
        static unsigned s_vram_warn_level = 0;   /* 0 none, 1 <16MB, 2 <4MB */
        if ((s_swap_counter & 0x3ffU) == 0U) {
            const unsigned free_kb = (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u);
            unsigned level = (free_kb < 4096u) ? 2u : (free_kb < 16384u) ? 1u : 0u;
            if (level > s_vram_warn_level) {
                s_vram_warn_level = level;
                l_warn("VRAM LOW: %uKB free (texture memory only grows on this port; "
                       "uploads may start failing). frame=%u", free_kb, s_swap_counter);
            } else if (level < s_vram_warn_level) {
                /* Re-arm on ANY improvement, not just a full recovery to level 0.
                 * The old test was `level == 0`, so a 2 -> 1 recovery left the latch
                 * stuck at 2 and a later re-entry into the <4MB band logged NOTHING:
                 * `level > s_vram_warn_level` was 2 > 2 = false. That silently
                 * discarded every warning after the first, which matters because this
                 * is the only signal for the failure mode that froze the r85 session
                 * (VRAM 206KB, RAM 142KB, renderer stalled). Latching down to the
                 * current level keeps one-warning-per-crossing while ensuring each
                 * fresh descent is reported. */
                s_vram_warn_level = level;
            }
        }
    }
#endif
}
#endif /* !USE_PVR_PSP2 */

/* The ONLY reason this wrapper exists: drop the loader's cached byte-size for each
 * name before GL is free to hand it to a different buffer. Without it a recycled name
 * whose new size coincidentally matches the old one takes the no-allocation
 * glBufferSubData path on its first upload and writes into storage that was never
 * allocated for it. Everything else is a straight pass-through. */
void glDeleteBuffers_soloader(GLsizei n, const GLuint *buffers) {
    extern void mcsm_vb_forget_gl_buffer(unsigned int name);
    if (buffers) {
        for (GLsizei i = 0; i < n; i++) mcsm_vb_forget_gl_buffer((unsigned int)buffers[i]);
    }
    glDeleteBuffers(n, buffers);
}

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

MCSM_DIAG_HELPER static int gl_draw_diag_should_log(unsigned int count, GLenum pre_err, GLenum query_err, GLenum err) {
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

MCSM_DIAG_HELPER static GLint gl_get_int_for_diag(GLenum pname, GLenum *query_err) {
    GLint value = 0;
    glGetIntegerv(pname, &value);
    GLenum err = drain_gl_errors_limited();
    if (query_err && *query_err == GL_NO_ERROR) {
        *query_err = err;
    }
    return value;
}

#define GL_DIAG_TEX_UNIT_CAP 16

#if !MCSM_FAST_FINAL_RUNTIME
static GLenum g_diag_active_texture = GL_TEXTURE0;
#endif
static int g_diag_active_texture_index = 0;
static GLuint g_diag_bound_texture_2d[GL_DIAG_TEX_UNIT_CAP];

/* ★ ALL THREE REDUNDANT-CALL DEDUPS ARE NOW GONE, EACH RETIRED BY MEASUREMENT.
 * The skip counters added on 2026-07-30 existed to answer "does this ever fire", and
 * across three device sessions the answer came back the same every time:
 *     glBindTexture   0 / 6,061,254   and   0 / 213,324
 *     glUseProgram    797 / 1,105,607 and 713 / 69,725      (~0.1%)
 *     depth/cull      cap_skips = 0    (2026-07-31, 7,400 frames to the menu)
 * The engine simply does not re-assert redundant GL state. The first two went on
 * 2026-07-30; the last went on 2026-07-31 together with the shared
 * no_state_dedup.txt kill-switch and the shadow-invalidation contract.
 * ☠ The lesson is the counters, not the dedups: an optimisation with no way to tell
 * whether it fires is how all three of these survived, and how a buffer fix ran zero
 * times on device while still reporting a clean build. Never ship the next one
 * without its measurement attached. */

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

MCSM_DIAG_HELPER static int gl_sampler_diag_should_log(unsigned int count, GLenum pre_err, GLenum err) {
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
    MCSM_DIAG_COUNT(s_count);
    const int texture_index = gl_texture_unit_index(texture);

#if MCSM_FAST_FINAL_RUNTIME
    glActiveTexture(texture);
    if (texture_index >= 0) {
        g_diag_active_texture_index = texture_index;
    }
#else
    GLenum pre_err = drain_gl_errors_limited();
    glActiveTexture(texture);
    GLenum err = glGetError();

    if (err == GL_NO_ERROR && texture_index >= 0) {
        g_diag_active_texture = texture;
        g_diag_active_texture_index = texture_index;
    }

    if (gl_sampler_diag_should_log(s_count, pre_err, err)) {
        l_info("glActiveTexture #%u unit=0x%X idx=%d pre=0x%X err=0x%X",
               s_count,
               (unsigned)texture,
               texture_index,
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
/* The currently bound id is functional state used by texture conversion, POT/wrap,
 * depth-texture emulation, and upload paths on both backends. */
static GLuint s_texlru_bound = 0;

/* vitaGL reclaim is deliberately disabled after three device-proven corruption
 * failures. Do not keep paying an 8192-entry LRU hash probe on every bind for a
 * collector that immediately returns. PVR still needs the object-count cap. */
#ifdef USE_PVR_PSP2
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
#define TEXLRU_MAXTEX 1000000u  /* vitaGL has no texture-OBJECT limit; the object cap stays off. */

/* vitaGL RECLAIM (2026-07-25) --------------------------------------------------
 * The engine never calls glDeleteTextures, so GPU texture memory only grows:
 * measured 80MB -> 8MB free across a 79-minute session (transiently 0). Nothing
 * failed in that run, but the end state is documented and ugly -- an upload OOMs,
 * the render thread cannot finish the frame, FinishFrame waits forever, black
 * screen.
 *
 * Eviction was tried twice before and BOTH attempts made things worse, so this
 * one is built to not repeat either:
 *   - Attempt 1 used a fixed 40MB byte budget. Gameplay legitimately needs more
 *     than that, so it evicted CONSTANTLY and inevitably deleted textures still
 *     in use -> garbage blocks + PVRTC re-upload thrash down to ~10fps.
 *     => Here there is NO budget. Nothing is evicted until free VRAM is actually
 *        low, so in a normal session this code never runs at all.
 *   - Both attempts relied on "not bound right now" as the safety check. That is
 *     too weak: a texture not bound this instant can still be needed next draw.
 *     => Here a victim must ALSO be untouched for TEXLRU_MIN_AGE binds, i.e. it
 *        belongs to a scene that is long gone. Currently-bound is still excluded.
 * Work per call is capped so a reclaim can never turn into a long stall. */
/* GRADUATED reclaim. The AGE gate is what makes this safe -- a texture nothing has
 * bound in a very long time belongs to a scene that is gone, and that is equally
 * true whether we notice it early or late. So we do NOT wait for near-exhaustion:
 * we start trimming while there is still plenty of headroom, and only get less
 * patient as free VRAM actually falls. Starting at 8MB free (the old plan) meant
 * running the tank down to nothing first and reclaiming in a panic; starting at
 * 40MB keeps VRAM in a healthy band all session.
 *
 * Tier            free VRAM        evict textures idle for      rationale
 * ---------------------------------------------------------------------------
 * (none)          > 40MB           nothing                      plenty spare
 * relaxed         24..40MB         240000 binds (~1 min+)       certainly dead
 * normal          12..24MB         80000  binds (~20s)          very likely dead
 * urgent          < 12MB           20000  binds (~4s)           take what we can
 *
 * Bind rate is roughly 5-12k/s in gameplay, so those ages are seconds-to-minutes
 * of being untouched, not frames. Anything still part of the live working set is
 * re-bound every frame and can never qualify. */
#define TEXLRU_VRAM_RELAXED  (40u * 1024u * 1024u)
#define TEXLRU_VRAM_NORMAL   (24u * 1024u * 1024u)
#define TEXLRU_VRAM_URGENT   (12u * 1024u * 1024u)
#define TEXLRU_AGE_RELAXED   240000u
#define TEXLRU_AGE_NORMAL    80000u
#define TEXLRU_AGE_URGENT    20000u
#define TEXLRU_EVICT_PER_RUN 32u                   /* bound the stall */
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
/* Bounded linear probe. The unbounded version was safe only while this table was
 * dead code on vitaGL; now that every glBindTexture touches it, an unbounded probe
 * is a trap. The engine re-uploads textures under FRESH GL names constantly, so the
 * table fills, and once it has no empty slot every MISS would walk all 8192 entries
 * -- at ~12k binds/s that is ~100M probes/s, which would cost far more than the
 * texture memory it was added to reclaim. 32 probes is generous for this hash at
 * any sane load factor; past that we report "not found", which is always safe:
 * a missed touch just leaves a stale age (the entry looks older, so at worst it
 * becomes evictable sooner), and a missed insert simply skips tracking one texture. */
#define TEXLRU_MAX_PROBE 32u
/* Slot states, encoded in (id, bytes):
 *     bytes != 0                -> LIVE, holding texture `id`
 *     bytes == 0 && id != 0     -> TOMBSTONE (evicted; the chain continues through it)
 *     bytes == 0 && id == 0     -> never used; a probe may stop here
 *
 * The tombstone is load-bearing, not bookkeeping. Eviction used to clear a victim
 * to a plain hole, which SEVERS the probe chain: any entry that had been pushed
 * past that slot by a collision became unreachable. Two ways that turned into
 * deleting a texture the engine was still drawing with:
 *   (a) texlru_touch() for the orphaned entry returned NULL, so its `use` stopped
 *       being refreshed. It then looked like the OLDEST entry in the table and the
 *       reclaim -- which deliberately picks the oldest -- chose it first, even
 *       though it was in the live working set.
 *   (b) the next texlru_record() for that id landed on the hole and inserted a
 *       SECOND entry for it. The stale duplicate kept ageing and was deleted while
 *       the fresh one was in use.
 * Either way glDeleteTextures hits a live texture, which is precisely the
 * "deleted-but-still-used textures sampled as GARBAGE" failure documented above --
 * the thing the age gate exists to prevent. texlru_is_live() cannot save it: that
 * only knows the handful of units bound at this instant.
 * Insert prefers the first tombstone seen so the table still gets reused. */
static texlru_ent *texlru_lookup(GLuint id, int insert) {
    uint32_t h = (id * 2654435761u) & (TEXLRU_SLOTS - 1u);
    texlru_ent *tomb = NULL;
    for (uint32_t i = 0; i < TEXLRU_MAX_PROBE; i++) {
        texlru_ent *e = &s_texlru[(h + i) & (TEXLRU_SLOTS - 1u)];
        if (e->bytes) {
            if (e->id == id) return e;          /* live hit */
            continue;                            /* occupied by someone else */
        }
        if (e->id != 0) {                        /* tombstone: remember, keep probing */
            if (!tomb) tomb = e;
            continue;
        }
        if (!insert) return NULL;                /* virgin slot: chain genuinely ends */
        if (tomb) { tomb->id = id; tomb->use = 0; return tomb; }
        e->id = id; e->use = 0; return e;
    }
    if (insert && tomb) { tomb->id = id; tomb->use = 0; return tomb; }
    return NULL;
}
MCSM_DIAG_HELPER static int texlru_is_live(GLuint id) {
    if (!id) return 1;
    for (int u = 0; u < GL_DIAG_TEX_UNIT_CAP; u++)
        if (g_diag_bound_texture_2d[u] == id) return 1;
    return 0;
}
static void texlru_touch(GLuint id) {
    /* The per-bind timestamp IS the safety mechanism for the vitaGL reclaim below
     * (a victim must be idle for TEXLRU_MIN_AGE binds), so it can no longer be
     * skipped there. One hash probe per bind.
     *
     * Except for the commonest case by far: consecutive draws re-binding the SAME
     * texture. Re-probing for an id we just stamped buys nothing -- it is already
     * the freshest entry in the table -- so short-circuit it. Ageing is unaffected:
     * a run of repeated binds now advances the clock once instead of N times, and
     * since every timestamp is measured against that same clock, relative ages (all
     * the reclaim compares) are unchanged. */
    static GLuint s_last_touched = 0;
    if (!id || id == s_last_touched) return;
    s_last_touched = id;
    texlru_ent *e = texlru_lookup(id, 0);
    if (e) e->use = ++s_texlru_clock;
}
#ifndef USE_PVR_PSP2
/* vitaGL: reclaim only under real VRAM pressure, and only textures that have been
 * idle long enough to be certain they belong to an unloaded scene. Returns without
 * touching anything in the overwhelmingly common case. */
static void texlru_reclaim_vitagl(GLuint keep_id) {
    /* DISABLED (2026-07-29) — THIRD failed attempt at reclaiming texture memory on
     * this engine. Do not re-enable without a fundamentally different idea.
     *
     * Device evidence: with the MOST conservative tier active (age >= 240000 binds,
     * 32MB free) it reclaimed 52 textures and visibly broke the chapter-selector
     * art. No OOM, no VRAM pressure -- it simply deleted textures the game was
     * still going to draw.
     *
     * The age heuristic cannot work here, and the reason is structural: the engine
     * NEVER calls glDeleteTextures, so it believes every texture it has ever
     * created is still live and never re-uploads one. Menu and selector art is
     * uploaded at boot and then not bound again for the whole time the player is in
     * gameplay -- so by any "recently used" measure it looks long dead, while the
     * engine still holds and expects it. From the engine's side NOTHING is ever
     * dead, so no idle-time threshold can separate "cold" from "finished". The two
     * earlier attempts (40MB byte budget, texture-object cap) died of the same
     * cause with different symptoms.
     *
     * Unbounded texture growth is the lesser evil and is what we ship: measured
     * across a 79-minute session VRAM fell 80MB -> 8MB with ZERO GL_OUT_OF_MEMORY
     * errors. A standing risk that has never once fired beats corrupting what the
     * player is looking at. The low-VRAM warning in gl_swap stays, so if it ever
     * does start failing the log will say so.
     *
     * Anything that replaces this needs a real liveness signal from the engine --
     * scene/resource-set unload boundaries, say -- not a timer. */
    (void)keep_id;
    return;

    /* Throttle: this sits on BOTH upload paths (~2900 uploads per scene load), and
     * below the relaxed threshold it would otherwise sweep the whole table on every
     * one of them -- landing squarely on the scene loads this port is already slow
     * at. VRAM pressure moves slowly, so sampling it periodically is equivalent and
     * costs ~1/64th as much. */
    static unsigned s_tick = 0;
    if ((s_tick++ & 0x3fU) != 0U) return;

    const uint32_t free_vram = (uint32_t)vglMemFree(VGL_MEM_VRAM);
    uint32_t min_age;
    if      (free_vram >= TEXLRU_VRAM_RELAXED) return;                  /* plenty spare */
    else if (free_vram >= TEXLRU_VRAM_NORMAL)  min_age = TEXLRU_AGE_RELAXED;
    else if (free_vram >= TEXLRU_VRAM_URGENT)  min_age = TEXLRU_AGE_NORMAL;
    else                                       min_age = TEXLRU_AGE_URGENT;

    /* Snapshot the bound units ONCE. texlru_is_live() re-reads this array for every
     * candidate, so calling it inside the sweep meant ~16 loads per tracked entry
     * per pass (~3000 entries = ~50k needless loads). */
    GLuint bound[GL_DIAG_TEX_UNIT_CAP];
    for (int u = 0; u < GL_DIAG_TEX_UNIT_CAP; u++) bound[u] = g_diag_bound_texture_2d[u];

    /* ONE pass collects the oldest eligible victims. The previous shape re-swept all
     * 8192 slots for each individual victim (up to 32 sweeps per call). */
    texlru_ent *victims[TEXLRU_EVICT_PER_RUN];
    unsigned nv = 0;
    for (uint32_t i = 0; i < TEXLRU_SLOTS; i++) {
        texlru_ent *e = &s_texlru[i];
        if (!e->bytes || e->id == keep_id) continue;
        if (s_texlru_clock - e->use < min_age) continue;                /* still recent */
        int live = 0;
        for (int u = 0; u < GL_DIAG_TEX_UNIT_CAP; u++) if (bound[u] == e->id) { live = 1; break; }
        if (live) continue;
        if (nv < TEXLRU_EVICT_PER_RUN) {
            victims[nv++] = e;
        } else {
            /* Replace the youngest of the chosen set, so we keep the oldest N. */
            unsigned worst = 0;
            for (unsigned k = 1; k < nv; k++) if (victims[k]->use > victims[worst]->use) worst = k;
            if (e->use < victims[worst]->use) victims[worst] = e;
        }
    }

    unsigned freed = 0;
    uint32_t freed_kb = 0;
    for (unsigned n = 0; n < nv; n++) {
        texlru_ent *victim = victims[n];
        GLuint vid = victim->id;
        glDeleteTextures(1, &vid);
        (void)glGetError();
        s_texlru_total -= victim->bytes;
        s_texlru_count--;
        freed_kb += victim->bytes / 1024u;
        /* TOMBSTONE: keep `id` so the probe chain through this slot survives.
         * Clearing it to 0 severs the chain and gets live textures deleted -- see
         * the state table above texlru_lookup(). */
        victim->bytes = 0; victim->use = 0;
        freed++;
    }
    if (freed) {
        s_texlru_evicted += freed;
        l_info("TEXLRU: reclaimed %u idle textures (%uKB) [age>=%u, free was %uKB]; "
               "live=%u total=%uKB free now=%uKB",
               freed, freed_kb, (unsigned)min_age, free_vram / 1024u,
               s_texlru_count, s_texlru_total / 1024u,
               (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u));
        glFinish();                    /* make the frees actually land before uploading */
    }
}
#endif

static void texlru_make_room(GLuint need, GLuint keep_id) {
#ifndef USE_PVR_PSP2
    (void)need;
    texlru_reclaim_vitagl(keep_id);
    return;
#else
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
#endif
}
static void texlru_record(GLuint id, GLuint bytes) {
    /* Recording is required on BOTH backends now: vitaGL's pressure-gated reclaim
     * needs the byte/age table to pick safe victims. ~2900 hash inserts per scene
     * load, which is noise next to the uploads themselves. */
    if (!id) return;
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
#else
static inline void texlru_touch(GLuint id) { (void)id; }
static inline void texlru_before_upload(GLint bound_tex, GLsizei bytes) {
    (void)bound_tex; (void)bytes;
}
static inline void texlru_after_upload(GLint bound_tex, GLsizei bytes, GLenum err) {
    (void)bound_tex; (void)bytes; (void)err;
}
#endif /* USE_PVR_PSP2 */

void glBindTexture_soloader(GLenum target, GLuint texture) {
    MCSM_DIAG_COUNT(s_count);

    const int active_idx = g_diag_active_texture_index;
#if MCSM_FAST_FINAL_RUNTIME
    /* ☠ REDUNDANT-BIND DEDUP REMOVED (2026-07-30) -- MEASURED DEAD, TWICE.
     * The idea was that a Telltale frame rebinds the same atlas across consecutive
     * batches, so the per-unit shadow g_diag_bound_texture_2d[] could skip the call.
     * Two device sessions say it never happens: 0 skips of 6,061,254 binds, then
     * GLDEDUP `tex=0/213324` and `prog=713/69725` (1%) for the program variant. The
     * engine simply does not issue redundant binds, so the compare could never pay
     * off while its counters cost 2-3 read-modify-writes per draw at ~890 draws a
     * frame. Removed rather than left "measure only": the question it existed to ask
     * has a definitive answer. g_diag_bound_texture_2d[] is still maintained below --
     * it predates this and other diagnostics read it. */
    glBindTexture(target, texture);
    if (target == GL_TEXTURE_2D) {
        s_texlru_bound = texture;
        texlru_touch(texture);
        if (active_idx >= 0) {
            g_diag_bound_texture_2d[active_idx] = texture;
        }
    }
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
    MCSM_DIAG_COUNT(s_count);

#if MCSM_FAST_FINAL_RUNTIME
    /* ☠ NO PROGRAM DEDUP HERE, AND THAT IS THE MEASURED ANSWER (2026-07-30).
     * vitaGL's glUseProgram is not a cheap assignment -- it re-resolves the
     * shader-patcher programs and rebinds the uniform buffers -- so skipping a
     * redundant one looked worthwhile at ~890 draws a frame. Two device sessions say
     * the engine does not issue them: 797 of 1,105,607 calls, then 713 of 69,725,
     * about 0.1%. The dedup and its counters are both gone; the counters alone cost a
     * read-modify-write per draw, and keeping a "measure only" counter forever is how
     * the earlier dead optimisations stayed invisible.
     * g_uniform_current_program is still maintained -- the uniform wrappers read it
     * as their split-memo key -- it is simply no longer used to skip anything. */
    glUseProgram(program);
    g_uniform_current_program = program;
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
    MCSM_DIAG_COUNT(s_count);

#if MCSM_FAST_FINAL_RUNTIME
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
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
    MCSM_DIAG_COUNT(s_count);
#ifdef DEBUG_SOLOADER
    g_frame_draw_calls++; g_frame_draw_verts += (unsigned long)count;
#endif

#if MCSM_FAST_FINAL_RUNTIME
#ifdef DEBUG_SOLOADER
    launch_state_mark_draw((unsigned)mode, (int)count, 0u, (int)g_uniform_current_program);
#endif
    glDrawArrays(mode, first, count);
    /* per-draw mark_gl_phase(0) dropped (2026-07-17): the present already refreshes
     * the watchdog phase/timestamp each frame; skip 2 syscalls/draw. */
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
    MCSM_DIAG_COUNT(s_count);
#ifdef DEBUG_SOLOADER
    g_frame_draw_calls++; g_frame_draw_verts += (unsigned long)count;
#endif

#if MCSM_FAST_FINAL_RUNTIME
#ifdef DEBUG_SOLOADER
    launch_state_mark_draw((unsigned)mode, (int)count, (unsigned)type, (int)g_uniform_current_program);
#endif
    glDrawElements(mode, count, type, indices);
    /* per-draw mark_gl_phase(0) dropped (2026-07-17): present refreshes the watchdog
     * phase/timestamp each frame; skip 2 syscalls/draw. */
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

/* ☠ THE DEPTH/CULL DEDUP IS GONE (2026-07-31), AND IT WAS MEASURED, NOT GUESSED.
 *
 * It shadowed GL_DEPTH_TEST / GL_CULL_FACE and skipped no-op enable/disable
 * transitions, on the theory that a material-sorted Telltale renderer re-asserts them
 * per batch across ~900 draws/frame. Device log 2026-07-31, a full session that
 * reached the menu and rendered 7 400+ frames with hundreds of thousands of draws:
 *
 *     GLDEDUP: cap_skips=0
 *
 * Zero. Not "low" -- the engine never once issued a redundant depth or cull enable,
 * so every call paid a cached-file check plus two compares to skip nothing. That is
 * the same verdict, from the same probe, that already deleted the glUseProgram (~1%)
 * and glBindTexture (0%) dedups on 2026-07-30; this was the last of the three and the
 * only one still carrying a correctness hazard, because a stale shadow would have
 * skipped a call that WAS needed and shown up as missing or inverted geometry.
 *
 * Removing a skip that never happens cannot change behaviour. It also retires the
 * no_state_dedup.txt escape hatch and the shadow-invalidation contract with it.
 * If this is ever reinstated, put the counter back at the same time -- an unmeasured
 * dedup is how all three of these survived this long. */
void glEnable_soloader(GLenum cap) {
    glEnable(cap);
}
void glDisable_soloader(GLenum cap) {
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

/* 2026-07-29: these no longer touch GL at all. vitaGL's glPixelStorei (textures.c)
 * implements exactly ONE pname -- GL_UNPACK_ROW_LENGTH -- and sends everything
 * else, GL_UNPACK_ALIGNMENT included, to `default: SET_GL_ERROR(GL_INVALID_ENUM)`.
 * So the old push/pop pair could never change unpack behaviour; all it did was
 * raise GL_INVALID_ENUM twice per upload.
 *
 * That was not harmless. glGetError returns and CLEARS the first error since the
 * last call, so the flag set by push() was the one the upload's own glGetError
 * read back -- device logs showed err=0x500 on 1733 glTexImage2D and 1684
 * glTexSubImage2D calls, and pre=0x500 on 1599 more where pop()'s error had
 * leaked into the next upload. Every one of those was our own, and they MASKED
 * the real upload status: a genuine failure would have looked identical. A
 * TEXDIAG probe sampling the error state between push() and the upload confirmed
 * it -- 24 of 24 already set before glTexImage2D ran.
 *
 * Keep the mirror (g_unpack_alignment) so the value is still tracked for the
 * repack decision below, but stop making the calls. Two fewer GL calls and two
 * fewer error-flag round-trips per upload, ~2900 uploads per scene load.
 *
 * NOTE the real consequence: vitaGL ignores unpack alignment entirely and always
 * reads tightly-packed rows. That is invisible for GL_RGBA (4 bytes/pixel is
 * always 4-byte aligned) which is 1685 of the 1734 uncompressed uploads here, but
 * NOT for the 36 GL_RGB and 13 GL_ALPHA ones, where a row whose byte count is not
 * a multiple of the alignment would shear. See mcsm_unpack_needs_repack. */
static GLint push_unpack_alignment_one(void) {
    return g_unpack_alignment;
}

static void pop_unpack_alignment(GLint old_align) {
    (void)old_align;
}

/* True when the engine's declared unpack alignment would pad a row and vitaGL,
 * which honours no alignment at all, would therefore misread the data. Only
 * possible for formats whose row byte count is not inherently aligned. */
static int mcsm_unpack_needs_repack(GLenum fmt, GLenum type, GLsizei w, GLint align) {
    if (align <= 1 || w <= 0) return 0;
    if (type != GL_UNSIGNED_BYTE) return 0;   /* packed 16-bit types are 2-byte units */
    int bpp;
    switch (fmt) {
        case GL_ALPHA: case GL_LUMINANCE:      bpp = 1; break;
        case GL_LUMINANCE_ALPHA:               bpp = 2; break;
        case GL_RGB:                           bpp = 3; break;
        default:                               return 0;  /* GL_RGBA: always aligned */
    }
    return (((GLsizei)bpp * w) % align) != 0;
}

/* No MCSM_DIAG_HELPER here, unlike the 565/4444 converters below: those are only
 * reachable on the PVR backend, but BGRA is converted on BOTH (see glTexImage2D_
 * soloader), so marking this one "possibly unused" would be a false note on a
 * function that is on the load path of every BGRA texture. */
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

MCSM_DIAG_HELPER static uint8_t *convert_rgba4444_to_rgba8888(const uint16_t *src, GLsizei width, GLsizei height) {
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

MCSM_DIAG_HELPER static uint8_t *alloc_zero_rgba8888(GLsizei width, GLsizei height) {
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

MCSM_DIAG_HELPER static uint8_t *convert_rgb565_to_rgba8888(const uint16_t *src, GLsizei width, GLsizei height) {
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
    return g_diag_bound_texture_2d[g_diag_active_texture_index];
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
    if (v < 0) { v = mcsm_cfg()->nearest_filter ? 1 : 0;
                 FILE *f = mcsm_open_setting("nearest_filter.txt", "r");   /* legacy override */
                 if (f) { fclose(f); v = 1; } }
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
    MCSM_DIAG_COUNT(s_count);

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
        FILE *f = mcsm_open_setting("mipmap_min.txt", "r");
        if (f) { int v = 0; if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 4096) g_mipmap_min = v; fclose(f); }
    }
    return g_mipmap_min;
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels) {
    MCSM_DIAG_COUNT(s_count);

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

    /* BGRA -> RGBA, on BOTH backends.
     *
     * ☠ DO NOT "OPTIMISE" THIS AWAY AGAIN. Passing BGRA straight to vitaGL was tried
     * on 2026-07-30 and corrupted textures on device (magenta/black blocks, broken UI
     * alpha). It had been justified from vitaGL's own source -- GL_BGRA == 0x80E1, a
     * `fast_store = GL_TRUE` direct-store branch, and a U8U8U8U8_ARGB mapping that is
     * the consistent reversal of GL_RGBA -> _ABGR -- and called certain by
     * construction. It was not; something in that chain does not hold for this
     * engine's data. The two backends were left as separate #ifdef branches doing the
     * identical thing afterwards, which is why they are merged here.
     *
     * ☠ The conversion ALLOCATES, and this port runs under real memory pressure. When
     * it fails there is no good outcome: the format fields are already committed to
     * GL_RGBA, so the original pointer uploads with R and B swapped, and restoring
     * GL_BGRA_EXT instead would take the known-corrupting path above. What is NOT
     * acceptable is doing it silently -- the vitaGL branch had no diagnostic at all,
     * so an out-of-memory texture would have surfaced as "some textures look wrong",
     * with nothing in the log to connect it to an allocation. Report it loudly and
     * take the deterministic-but-swapped result. */
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
        upload_internalformat = GL_RGBA;
        upload_format = GL_RGBA;
        if (pixels) {
            converted = convert_bgra_to_rgba((const uint8_t *)pixels, width, height);
            if (converted) {
                upload_pixels = converted;
            } else {
                l_error("glTexImage2D: BGRA->RGBA conversion could not allocate for "
                        "%dx%d — uploading the original pointer as GL_RGBA, so THIS "
                        "TEXTURE WILL HAVE RED AND BLUE SWAPPED", width, height);
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
        FILE *df = mcsm_open_setting("downsample_min.txt", "r");
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
    /* Report-only. vitaGL always reads tightly-packed rows, so if the engine's
     * declared alignment would pad this row the two disagree and the texture
     * shears. Whether that actually happens depends on how the engine laid the
     * data out, which cannot be read off from here -- so log it rather than
     * "fix" it by repacking, which would corrupt the opposite case. Rare by
     * construction (GL_RGBA can never trip it), so an unconditional warn is fine
     * and its absence is the useful result. */
    if (mcsm_unpack_needs_repack(upload_format, upload_type, width, old_unpack_align)) {
        l_warn("UNPACK: fmt=0x%X type=0x%X size=%dx%d align=%d — row is padded but vitaGL "
               "reads tight; this texture may shear", (unsigned)upload_format,
               (unsigned)upload_type, (int)width, (int)height, (int)old_unpack_align);
    }
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
    MCSM_DIAG_COUNT(s_count);

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

    /* Same conversion, same reasoning, and the same "do not remove it again" note as
     * glTexImage2D_soloader above -- including why the failure path shouts. This is
     * the STREAMING path (video frames, dynamic UI), so it recurs during play rather
     * than only at load, which makes a silent wrong-channel upload here even harder
     * to trace back to an allocation failure. */
    if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
        upload_format = GL_RGBA;
        converted = convert_bgra_to_rgba((const uint8_t *)pixels, width, height);
        if (converted) {
            upload_pixels = converted;
        } else if (pixels) {
            l_error("glTexSubImage2D: BGRA->RGBA conversion could not allocate for "
                    "%dx%d — uploading the original pointer as GL_RGBA, so THIS "
                    "TEXTURE WILL HAVE RED AND BLUE SWAPPED", width, height);
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
#ifdef DEBUG_SOLOADER
    static unsigned s_logged = 0;
    if (s_logged++ < 16U) {
        l_info("%s skipped invalid location=%d", name, location);
    }
#else
    (void)name;
#endif
    return 1;
}

static int uniform_vector_should_skip(const char *name, GLint location, GLsizei count, const void *value) {
    if (location >= 0 && count > 0 && value) {
        return 0;
    }
#ifdef DEBUG_SOLOADER
    static unsigned s_logged = 0;
    if (s_logged++ < 32U) {
        l_info("%s skipped location=%d count=%d value=%p", name, location, count, value);
    }
#else
    (void)name;
#endif
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
    MCSM_DIAG_COUNT(s_count);
    if (uniform_scalar_should_skip("glUniform1i", location)) return;

#if MCSM_FAST_FINAL_RUNTIME
    glUniform1i(location, v0);
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
    MCSM_DIAG_COUNT(s_count);
    if (uniform_vector_should_skip("glUniform1iv", location, count, value)) return;

#if MCSM_FAST_FINAL_RUNTIME
    glUniform1iv(location, count, value);
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
#ifdef DEBUG_SOLOADER
static int mcsm_anim_pose_diag_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        /* mcsm_open_setting(), like every other tunable: the raw sceIoOpen this
         * replaced looked only at the data root, so animdiag.txt in settings/ --
         * where the device actually keeps its config -- was silently ignored. */
        FILE *fd = mcsm_open_setting("animdiag.txt", "r");
        if (fd) {
            fclose(fd);
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
#endif

void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value) {
    if (uniform_vector_should_skip("glUniform4fv", location, count, value)) return;
#ifdef DEBUG_SOLOADER
    mcsm_log_anim_pose(location, count, value);
#endif

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
        if (s_fbz < 0) { s_fbz = mcsm_cfg()->fbfetch_zero ? 1 : 0;
                         FILE *fz = mcsm_open_setting("fbfetch_zero.txt", "r");   /* legacy override */
                         if (fz) { fclose(fz); s_fbz = 1; } }
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
        /* Explicit precision: this declaration is prepended to the TOP of the
         * shader, i.e. BEFORE the `precision mediump float;` statement. A bare
         * `float` there has no default precision yet — tolerant compilers assume
         * one, but a STRICT libshacccg errors/HANGS on "no precision specified".
         * Qualifying it mediump makes it valid regardless of position. */
        changed += glsl_prepend_alloc(src_io, len_io, "mediump float mcsm_unused_frag_depth;\n");
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
#ifdef DEBUG_SOLOADER
    if (precision_promotions) {
        static unsigned s_precision_logged = 0;
        if (s_precision_logged++ < 8U) {
            l_info("glShaderSource(%u): promoted %d lowp shader precision qualifiers to mediump.",
                   shader, precision_promotions);
        }
    }
#else
    (void)precision_promotions;
#endif

#ifndef USE_PVR_PSP2
#ifdef DEBUG_SOLOADER
    if (neutralize_unsupported_glsl(&str, &total_length)) {
        static unsigned s_neutralized_logged = 0;
        if (s_neutralized_logged++ < 8U) {
            l_info("glShaderSource(%u): neutralized unsupported GLSL features "
                   "(framebuffer-fetch/shadow/depth/lod) so the shader can compile.", shader);
        }
    }
#else
    (void)neutralize_unsupported_glsl(&str, &total_length);
#endif
    /* FRAGMENT-SHADER highp GUARD — the real cross-Vita fix. GLSL ES guarantees
     * highp in VERTEX shaders but makes it OPTIONAL in FRAGMENT shaders; a fragment
     * shader using highp (Telltale's `uhi`/bare `highp`) must guard it or a STRICT
     * libshacccg errors/HANGS, while a lenient one (the dev's .92) compiles it. This
     * is exactly why vertex shaders compile on the testers but the first highp
     * FRAGMENT shader hangs. Standard shim: demote highp->mediump ONLY where the
     * compiler doesn't advertise fragment-highp support -> zero change on devices
     * that do (no visual/behaviour change on a working setup). */
    {
        GLint stype = 0;
        glGetShaderiv(shader, GL_SHADER_TYPE, &stype);
        if (stype == GL_FRAGMENT_SHADER) {
#ifdef DEBUG_SOLOADER
            static unsigned s_hp_logged = 0;
#endif
            glsl_prepend_alloc(&str, &total_length,
                "#ifndef GL_FRAGMENT_PRECISION_HIGH\n#define highp mediump\n#endif\n");
#ifdef DEBUG_SOLOADER
            if (s_hp_logged++ < 4U)
                l_info("glShaderSource(%u): added fragment highp compatibility guard.", shader);
#endif
        }
    }
#endif

    /* DIAG (opt-in via dump_shaders.txt): write the cooked (post-neutralization)
     * source the compiler actually receives, to ux0:data/mcsm/cooked/shader_<id>.glsl,
     * so it can be pulled + inspected. No effect on normal boots. */
    {
        static int s_dump = -1;
        if (s_dump < 0) { FILE *df = mcsm_open_setting("dump_shaders.txt", "r");
                          s_dump = df ? (fclose(df), 1) : 0;
                          if (s_dump) sceIoMkdir("ux0:data/mcsm/cooked", 0777); }
        if (s_dump) {
            char p[96]; snprintf(p, sizeof(p), "ux0:data/mcsm/cooked/shader_%u.glsl", (unsigned)shader);
            SceUID fd = sceIoOpen(p, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (fd >= 0) { sceIoWrite(fd, str, total_length); sceIoClose(fd); }
        }
    }

    track_shader_source(shader, str, total_length);
    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_SOLOADER
    static uint32_t s_compile_counter = 0;
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        GLint status = GL_TRUE;
        GLenum err_before = GL_NO_ERROR;
        GLenum err_after = GL_NO_ERROR;
        shader_diag_entry *entry = get_shader_diag_entry(shader, 0);
#ifdef DEBUG_SOLOADER
        s_compile_counter++;
#endif

        err_before = glGetError();
#ifdef DEBUG_SOLOADER
        if (s_compile_counter <= 8U) {
            log_shader_compile_runtime_state(shader, "before", err_before);
        }
#else
        (void)err_before;
#endif
        if (entry && entry->owned_source) {
            const GLchar *src = entry->owned_source;
            glShaderSource(shader, 1, &src, NULL);
        }
        glCompileShader(shader);
        err_after = glGetError();
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
#ifdef DEBUG_SOLOADER
        if (s_compile_counter <= 8U || status != GL_TRUE || err_after != GL_NO_ERROR) {
            log_shader_compile_runtime_state(shader, "after", err_after);
        }
#else
        (void)err_after;
#endif
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
/* NOT bumped for the 2026-07-29 O0->O2 compiler change. Bumping the magic
 * invalidates EVERY binary on disk, which would have thrown away the CH1+CH2
 * cache that took real playthroughs to build. The compiler setting is mixed into
 * the KEY instead (see progcache_key_for_program), so O0 and O2 binaries get
 * different filenames and coexist -- switching shader_opt back to 0 re-derives
 * the original keys and the existing cache loads untouched, with no recompile. */
#define PROGCACHE_MAGIC  0x4d435034u   /* 'MCP4' — bumped 2026-07-20b: MCP3 clamped the engine's
                                        * over-long _length but device missdumps proved FRAGMENT
                                        * shaders still carried a NUL+varying-garbage tail (from
                                        * glsl_replace_alloc's strlen-copy / arithmetic-length
                                        * mismatch). track_shader_source now clamps the hashed source
                                        * to the first embedded NUL — the final, path-independent fix.
                                        * Keys change again vs MCP3, so those caches must invalidate. */
/* 'MCP5' was briefly written by r33, which raised the shader compiler to O2 and
 * bumped the magic -- that combination rewrote 31 of the 274 cached binaries in
 * place before it was caught. Those files are perfectly valid compiled programs;
 * only the compiler settings behind them differ, and a compiled binary is
 * self-contained, so mixing O0- and O2-built programs in one session is harmless.
 * ACCEPT them on read rather than rejecting them, which would force 31 needless
 * recompiles. Only MCP4 is ever WRITTEN, so the cache converges back on its own. */
#define PROGCACHE_MAGIC_O2 0x4d435035u
#define PROGCACHE_MAXBIN (1024u * 1024u)
static int g_progcache_dir_ready = 0;
#ifdef DEBUG_SOLOADER
static unsigned g_progcache_hits = 0, g_progcache_misses = 0;
/* SAVE DIAGNOSTIC counters (2026-07-18): the on-device cache came up EMPTY after
 * play. These pin WHY progcache_save wrote nothing, written via sceIo (reliable,
 * unlike newlib fprintf) to ux0:data/mcsm/progcache_diag.txt. */
static unsigned g_pc_links = 0, g_pc_key0 = 0, g_pc_notlinked = 0, g_pc_nobin = 0;
static unsigned g_pc_saveok = 0, g_pc_openfail = 0, g_pc_loadfail = 0;
static int g_pc_lastopen = 1;
#define MCSM_PC_DIAG(code) do { code; } while (0)
#else
enum {
    g_progcache_hits = 0, g_progcache_misses = 0,
    g_pc_links = 0, g_pc_key0 = 0, g_pc_notlinked = 0, g_pc_nobin = 0,
    g_pc_saveok = 0, g_pc_openfail = 0, g_pc_loadfail = 0,
    g_pc_lastopen = 1
};
#define MCSM_PC_DIAG(code) do { } while (0)
#endif

/* FNV-1a over the attached shaders' tracked (patched) sources. 0 = unkeyable. */
static uint64_t progcache_key_for_program(GLuint program) {
    GLuint sh[8]; GLsizei n = 0;
    glGetAttachedShaders(program, 8, &n, sh);
    if (n <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    /* 2026-07-29: fold the shader compiler opt level into the key so binaries
     * built at different opt levels never alias -- the cache stores a COMPILED
     * binary but is keyed on shader SOURCE, so without this an O0 binary would be
     * handed back while the compiler is set to O2 (and the change would silently
     * do nothing). Deliberately a no-op at opt 0: that keeps every key already on
     * disk bit-identical, so the existing CH1+CH2 cache stays valid and switching
     * shader_opt back to 0 costs zero recompiles. */
    {
        int opt = mcsm_cfg()->shader_opt;
        if (opt != 0) { h ^= (uint64_t)(unsigned)opt; h *= 1099511628211ull; }
    }
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
        (hdr[0] != PROGCACHE_MAGIC && hdr[0] != PROGCACHE_MAGIC_O2) ||
        hdr[3] == 0 || hdr[3] > PROGCACHE_MAXBIN) {
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
        MCSM_PC_DIAG(g_pc_loadfail++); /* file loaded but failed to restore */
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
    if (linked != GL_TRUE) { MCSM_PC_DIAG(g_pc_notlinked++); return; }
    GLint binlen = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binlen);
    if (binlen <= 0 || (unsigned)binlen > PROGCACHE_MAXBIN) { MCSM_PC_DIAG(g_pc_nobin++); return; }
    void *bin = malloc((size_t)binlen);
    if (!bin) return;
    GLsizei len = 0; GLenum fmt = 0;
    glGetProgramBinary(program, binlen, &len, &fmt, bin);
    if (len <= 0 || (unsigned)len > (unsigned)binlen) { free(bin); MCSM_PC_DIAG(g_pc_nobin++); return; }
    if (!g_progcache_dir_ready) { sceIoMkdir(PROGCACHE_DIR, 0777); g_progcache_dir_ready = 1; }
    char path[160]; progcache_path(key, path, sizeof(path));
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        /* SELF-HEAL (2026-07-18): the progcache dir may not exist (fresh install
         * with no seed, or user deleted it mid-session). Re-create + retry once. */
        sceIoMkdir(PROGCACHE_DIR, 0777);
        fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    }
    MCSM_PC_DIAG(g_pc_lastopen = (int)fd);
    if (fd >= 0) {
        uint32_t hdr[4] = { PROGCACHE_MAGIC, 1u, (uint32_t)fmt, (uint32_t)len };
        sceIoWrite(fd, hdr, sizeof(hdr));
        sceIoWrite(fd, bin, len);
        sceIoClose(fd);
        MCSM_PC_DIAG(g_pc_saveok++);
    } else {
        MCSM_PC_DIAG(g_pc_openfail++);
    }
    free(bin);
}
#endif /* !USE_PVR_PSP2 */

#if !defined(USE_PVR_PSP2) && defined(DEBUG_SOLOADER)
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
    MCSM_PC_DIAG(g_pc_links++);
    uint64_t pkey = progcache_key_for_program(program);
    if (pkey && progcache_try_load(program, pkey)) {
        /* Linked from disk — NO ShaccCg compile this run (the freeze killer). */
        MCSM_PC_DIAG(g_progcache_hits++);
        if (g_progcache_hits <= 80U)
            l_info("progcache HIT prog=%u key=%08X%08X (hits=%u miss=%u)", program,
                   (unsigned)(pkey >> 32), (unsigned)(pkey & 0xffffffffu),
                   g_progcache_hits, g_progcache_misses);
        log_program_link_failure(program);
        program_cache_refresh(program);
        return;
    }
    launch_state_mark_gl_phase(2);
    glLinkProgram(program);
    launch_state_mark_gl_phase(0);
    if (pkey) {
#ifdef DEBUG_SOLOADER
        g_progcache_misses++;
        dump_miss_sources(program, pkey);
#endif
        progcache_save(program, pkey);
        if (g_progcache_misses <= 80U)
            l_info("progcache MISS prog=%u key=%08X%08X compiled+saved (hits=%u miss=%u)", program,
                   (unsigned)(pkey >> 32), (unsigned)(pkey & 0xffffffffu),
                   g_progcache_hits, g_progcache_misses);
    } else {
        MCSM_PC_DIAG(g_pc_key0++);
        MCSM_DIAG_COUNT(s_uncached);
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
