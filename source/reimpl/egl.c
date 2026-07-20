/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/egl.h"

#include "utils/glutil.h"
#include "utils/logger.h"
#include "java_runtime.h"

#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef EGL_SUCCESS
#define EGL_SUCCESS 0x3000
#endif

#ifndef EGL_BAD_PARAMETER
#define EGL_BAD_PARAMETER 0x300C
#endif

extern void *lookup_symbol_soloader_quiet(const char *symbol);

typedef struct EglHandle {
    uint32_t tag;
} EglHandle;

enum {
    EGL_HANDLE_CONTEXT = 0x43545854,
    EGL_HANDLE_SURFACE = 0x53555246,
};

static int32_t g_egl_last_error = EGL_SUCCESS;

typedef struct EglThreadState {
    SceUID tid;
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw_surface;
    EGLSurface read_surface;
    int32_t last_error;
    uint8_t saw_make_current;
    uint8_t logged_null_display;
    uint8_t logged_null_context;
} EglThreadState;

#define EGL_THREAD_STATE_CAP 16

static atomic_flag g_egl_state_lock = ATOMIC_FLAG_INIT;
static EglThreadState g_egl_thread_states[EGL_THREAD_STATE_CAP];

static void egl_state_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_egl_state_lock, memory_order_acquire)) {
    }
}

static void egl_state_unlock(void) {
    atomic_flag_clear_explicit(&g_egl_state_lock, memory_order_release);
}

static SceUID egl_current_tid(void) {
    return sceKernelGetThreadId();
}

static EglThreadState *get_thread_state_locked(SceUID tid, int create) {
    EglThreadState *free_slot = NULL;

    for (size_t i = 0; i < EGL_THREAD_STATE_CAP; ++i) {
        if (g_egl_thread_states[i].tid == tid) {
            return &g_egl_thread_states[i];
        }
        if (create && !free_slot && g_egl_thread_states[i].tid == 0) {
            free_slot = &g_egl_thread_states[i];
        }
    }

    if (!create || !free_slot) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->tid = tid;
    free_slot->last_error = EGL_SUCCESS;
    return free_slot;
}

static void set_egl_last_error(int32_t error) {
    SceUID tid = egl_current_tid();

    egl_state_lock();
    EglThreadState *state = get_thread_state_locked(tid, 1);
    if (state) {
        state->last_error = error;
    } else {
        g_egl_last_error = error;
    }
    egl_state_unlock();
}

static int32_t take_egl_last_error(void) {
    int32_t error = EGL_SUCCESS;
    SceUID tid = egl_current_tid();

    egl_state_lock();
    EglThreadState *state = get_thread_state_locked(tid, 0);
    if (state) {
        error = state->last_error;
        state->last_error = EGL_SUCCESS;
    } else {
        error = g_egl_last_error;
        g_egl_last_error = EGL_SUCCESS;
    }
    egl_state_unlock();

    return error;
}

static void clear_destroyed_context_locked(EGLContext ctx) {
    for (size_t i = 0; i < EGL_THREAD_STATE_CAP; ++i) {
        if (g_egl_thread_states[i].tid != 0 && g_egl_thread_states[i].context == ctx) {
            g_egl_thread_states[i].context = NULL;
            g_egl_thread_states[i].display = NULL;
            g_egl_thread_states[i].draw_surface = NULL;
            g_egl_thread_states[i].read_surface = NULL;
            g_egl_thread_states[i].saw_make_current = 0;
        }
    }
}

static void clear_destroyed_surface_locked(EGLSurface surface) {
    for (size_t i = 0; i < EGL_THREAD_STATE_CAP; ++i) {
        if (g_egl_thread_states[i].tid == 0) {
            continue;
        }
        if (g_egl_thread_states[i].draw_surface == surface) {
            g_egl_thread_states[i].draw_surface = NULL;
        }
        if (g_egl_thread_states[i].read_surface == surface) {
            g_egl_thread_states[i].read_surface = NULL;
        }
    }
}

static void clear_terminated_display_locked(EGLDisplay dpy) {
    for (size_t i = 0; i < EGL_THREAD_STATE_CAP; ++i) {
        if (g_egl_thread_states[i].tid != 0 && g_egl_thread_states[i].display == dpy) {
            g_egl_thread_states[i].display = NULL;
            g_egl_thread_states[i].context = NULL;
            g_egl_thread_states[i].draw_surface = NULL;
            g_egl_thread_states[i].read_surface = NULL;
            g_egl_thread_states[i].saw_make_current = 0;
        }
    }
}

static void maybe_log_missing_current(const char *api, int want_context) {
    SceUID current_tid = egl_current_tid();
    SceUID owner_tid = 0;
    EGLDisplay owner_display = NULL;
    EGLContext owner_context = NULL;
    int should_log = 0;

    egl_state_lock();
    EglThreadState *state = get_thread_state_locked(current_tid, 1);
    if (state) {
        uint8_t *flag = want_context ? &state->logged_null_context : &state->logged_null_display;
        if (!*flag) {
            for (size_t i = 0; i < EGL_THREAD_STATE_CAP; ++i) {
                if (g_egl_thread_states[i].tid == 0 || g_egl_thread_states[i].tid == current_tid) {
                    continue;
                }
                if (g_egl_thread_states[i].display || g_egl_thread_states[i].context) {
                    should_log = 1;
                    *flag = 1;
                    owner_tid = g_egl_thread_states[i].tid;
                    owner_display = g_egl_thread_states[i].display;
                    owner_context = g_egl_thread_states[i].context;
                    break;
                }
            }
        }
    }
    egl_state_unlock();

    if (should_log) {
        l_warn("%s tid=0x%X -> NULL while tid=0x%X owns dpy=%p ctx=%p",
               api,
               (unsigned)current_tid,
               (unsigned)owner_tid,
               owner_display,
               owner_context);
    }
}

void mcsm_egl_get_current_state(EGLDisplay *dpy, EGLContext *ctx,
                                EGLSurface *draw, EGLSurface *read) {
    SceUID tid = egl_current_tid();

    if (dpy) {
        *dpy = NULL;
    }
    if (ctx) {
        *ctx = NULL;
    }
    if (draw) {
        *draw = NULL;
    }
    if (read) {
        *read = NULL;
    }

    egl_state_lock();
    EglThreadState *state = get_thread_state_locked(tid, 0);
    if (state) {
        if (dpy) {
            *dpy = state->display;
        }
        if (ctx) {
            *ctx = state->context;
        }
        if (draw) {
            *draw = state->draw_surface;
        }
        if (read) {
            *read = state->read_surface;
        }
    }
    egl_state_unlock();
}

static void *alloc_egl_handle(uint32_t tag) {
    EglHandle *handle = (EglHandle *)calloc(1, sizeof(EglHandle));
    if (!handle) {
        return NULL;
    }
    handle->tag = tag;
    return handle;
}

int32_t eglBindAPI(uint32_t api) {
    (void)api;
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

void *eglGetDisplay(void *native_display) {
    (void)native_display;
    set_egl_last_error(EGL_SUCCESS);
    return (void *)0x1;
}

int32_t eglGetError(void) {
    return take_egl_last_error();
}

void (*eglGetProcAddress(char const *procname))(void) {
    if (!procname) {
        set_egl_last_error(EGL_BAD_PARAMETER);
        return NULL;
    }

    void *proc = lookup_symbol_soloader_quiet(procname);
    set_egl_last_error(EGL_SUCCESS);
    if (!proc) {
        l_debug("eglGetProcAddress(%s): unresolved", procname);
    }
    return proc;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    l_debug("eglInitialize(0x%x)", (int)dpy);

    gl_init();

    if (major) *major = 2;
    if (minor) *minor = 2;

    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute,
                           EGLint *value) {
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_CONTEXT_CLIENT_TYPE:
            *value = EGL_OPENGL_ES_API;
            break;
        case EGL_CONTEXT_CLIENT_VERSION:
            *value = 2;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        default:
            l_error("eglQueryContext / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    return ret;
}


EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface eglSurface,
                           EGLint attribute, EGLint *value) {
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_WIDTH:
            *value = mcsm_get_framebuffer_width();
            break;
        case EGL_HEIGHT:
            *value = mcsm_get_framebuffer_height();
            break;
        case EGL_TEXTURE_FORMAT:
            *value = EGL_TEXTURE_RGBA;
            break;
        case EGL_TEXTURE_TARGET:
            *value = EGL_TEXTURE_2D;
            break;
        case EGL_SWAP_BEHAVIOR:
            *value = EGL_BUFFER_PRESERVED;
            break;
        case EGL_LARGEST_PBUFFER:
        case EGL_MIPMAP_TEXTURE:
            *value = EGL_FALSE;
            break;
        case EGL_MIPMAP_LEVEL:
            *value = 0;
            break;
        case EGL_MULTISAMPLE_RESOLVE:
            // ignored when creating the surface, return default
            *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT;
            break;
        case EGL_HORIZONTAL_RESOLUTION:
        case EGL_VERTICAL_RESOLUTION:
            *value = 220 * EGL_DISPLAY_SCALING; // VITA DPI is 220
            break;
        case EGL_PIXEL_ASPECT_RATIO:
            // Please don't ask why * EGL_DISPLAY_SCALING, the document says it
            *value = mcsm_get_framebuffer_width() / mcsm_get_framebuffer_height() * EGL_DISPLAY_SCALING;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        case EGL_VG_COLORSPACE:
            // ignored when creating the surface, return default
            *value = EGL_VG_COLORSPACE_sRGB;
            break;
        case EGL_VG_ALPHA_FORMAT:
            // ignored when creating the surface, return default
            *value = EGL_VG_ALPHA_FORMAT_NONPRE;
            break;
        case EGL_TIMESTAMPS_ANDROID:
            *value = EGL_FALSE;
            break;
        default:
            l_error("eglQuerySurface / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    return ret;
}


EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                              EGLint attribute, EGLint * value) {
    switch (attribute) {
        case EGL_ALPHA_SIZE: {
            *value = 8;
            break;
        }
        case EGL_ALPHA_MASK_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGB: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGBA: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BLUE_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BUFFER_SIZE: {
            *value = 32;
            break;
        }
        case EGL_COLOR_BUFFER_TYPE: {
            *value = EGL_RGB_BUFFER;
            break;
        }
        case EGL_CONFIG_CAVEAT: {
            *value = EGL_NONE;
            break;
        }
        case EGL_CONFIG_ID: {
            *value = 0;
            break;
        }
        case EGL_CONFORMANT: {
            *value = 0;
            break;
        }
        case EGL_DEPTH_SIZE: {
            *value = 24;
            break;
        }
        case EGL_GREEN_SIZE: {
            *value = 8;
            break;
        }
        case EGL_LEVEL: {
            *value = 0;
            break;
        }
        case EGL_LUMINANCE_SIZE: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_WIDTH: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_HEIGHT: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_PIXELS: {
            *value = 0;
            break;
        }
        case EGL_MAX_SWAP_INTERVAL: {
            *value = 0;
            break;
        }
        case EGL_MIN_SWAP_INTERVAL: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_RENDERABLE: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_ID: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_TYPE: {
            *value = 0;
            break;
        }
        case EGL_RED_SIZE: {
            *value = 8;
            break;
        }
        case EGL_RENDERABLE_TYPE: {
            *value = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_BIT;
            break;
        }
        case EGL_SAMPLE_BUFFERS: {
            *value = 0;
            break;
        }
        case EGL_SAMPLES: {
            *value = 0;
            break;
        }
        case EGL_STENCIL_SIZE: {
            *value = 8;
            break;
        }
        case EGL_SURFACE_TYPE: {
            *value = 0 | EGL_WINDOW_BIT;
            break;
        }
        case EGL_TRANSPARENT_TYPE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_RED_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_GREEN_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_BLUE_VALUE: {
            *value = 0;
            break;
        }
        default:
            l_error("eglGetConfigAttrib / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config) {
    if (!num_config) {
        set_egl_last_error(EGL_BAD_PARAMETER);
        return EGL_FALSE;
    }

    if (!configs) {
        *num_config = 1;
        set_egl_last_error(EGL_SUCCESS);
        return EGL_TRUE;
    }

    *configs = strdup("conf");
    *num_config = 1;
    set_egl_last_error(EGL_SUCCESS);

    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list) {
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)attrib_list;
    return alloc_egl_handle(EGL_HANDLE_CONTEXT);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  void * win, const EGLint *attrib_list) {
    (void)dpy;
    (void)config;
    (void)win;
    (void)attrib_list;
    return alloc_egl_handle(EGL_HANDLE_SURFACE);
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx) {
    const SceUID tid = egl_current_tid();
    int should_log = 0;

    egl_state_lock();
    EglThreadState *state = get_thread_state_locked(tid, 1);
    if (state) {
        should_log = !state->saw_make_current ||
                     state->display != dpy ||
                     state->draw_surface != draw ||
                     state->read_surface != read ||
                     state->context != ctx;
        state->display = dpy;
        state->draw_surface = draw;
        state->read_surface = read;
        state->context = ctx;
        state->last_error = EGL_SUCCESS;
        state->saw_make_current = (dpy || draw || read || ctx) ? 1 : 0;
        state->logged_null_display = 0;
        state->logged_null_context = 0;
    } else {
        g_egl_last_error = EGL_SUCCESS;
    }
    egl_state_unlock();

    if (should_log) {
        l_info("eglMakeCurrent tid=0x%X dpy=%p draw=%p read=%p ctx=%p",
               (unsigned)tid,
               dpy,
               draw,
               read,
               ctx);
    }
    return EGL_TRUE;
}

int32_t eglSwapInterval(void *display, int32_t interval) {
    (void)display;
    (void)interval;
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

int32_t eglSwapBuffers(void *display, void *surface) {
    static unsigned int s_swap_counter = 0;
    (void)display;
    (void)surface;

    gl_swap();

    s_swap_counter++;
    if (s_swap_counter <= 8 || (s_swap_counter & 0x7fU) == 0U) {
        l_info("eglSwapBuffers count=%u", s_swap_counter);
    }

    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglWaitNative(EGLint engine) {
    (void)engine;
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglWaitGL(void) {
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglDestroyContext (EGLDisplay dpy, EGLContext ctx) {
    (void)dpy;
    egl_state_lock();
    clear_destroyed_context_locked(ctx);
    egl_state_unlock();
    free(ctx);
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface (EGLDisplay dpy, EGLSurface surface) {
    (void)dpy;
    egl_state_lock();
    clear_destroyed_surface_locked(surface);
    egl_state_unlock();
    free(surface);
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    egl_state_lock();
    clear_terminated_display_locked(dpy);
    egl_state_unlock();
    set_egl_last_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext (void) {
    EGLContext ctx = NULL;
    mcsm_egl_get_current_state(NULL, &ctx, NULL, NULL);
    if (!ctx) {
        maybe_log_missing_current("eglGetCurrentContext", 1);
    }
    return ctx;
}

EGLDisplay eglGetCurrentDisplay(void) {
    EGLDisplay dpy = NULL;
    mcsm_egl_get_current_state(&dpy, NULL, NULL, NULL);
    if (!dpy) {
        maybe_log_missing_current("eglGetCurrentDisplay", 0);
    }
    return dpy;
}

char const * eglQueryString(EGLDisplay display, EGLint name) {
    switch (name) {
    case EGL_CLIENT_APIS:
        return "OpenGL OpenGL_ES";
    case EGL_VENDOR:
        return "Rinnegatamante";
    case EGL_VERSION:
        return "2.2 VitaGL";
    case EGL_EXTENSIONS:
        return "EGL_KHR_image "
               "EGL_KHR_image_base "
               "EGL_KHR_image_pixmap "
               "EGL_KHR_gl_texture_2D_image "
               "EGL_KHR_gl_texture_cubemap_image "
               "EGL_KHR_gl_renderbuffer_image "
               "EGL_KHR_fence_sync "
               "EGL_NV_system_time "
               "EGL_ANDROID_image_native_buffer ";
    default:
        return NULL;
    }
}

EGLBoolean eglGetConfigs(EGLDisplay display, EGLConfig * configs,
                         EGLint config_size, EGLint * num_config) {
    if (!num_config) {
        l_error("eglGetConfigs / EGL_BAD_PARAMETER");
        return EGL_FALSE;
    }

    if (configs && config_size > 0) {
        *configs = strdup("conf");
    }

    *num_config = 1;

    return EGL_TRUE;
}

void *ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env;
    return surface ? surface : (void *)0x1;
}

void ANativeWindow_release(void *window) {
    (void)window;
}

/* egl.c compiles for both vitaGL and PVR_PSP2 modes */
