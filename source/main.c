#include "utils/init.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/launch_state.h"
#include "utils/telemetry.h"
#include "java_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#ifdef USE_PVR_PSP2
#include "utils/pvr_init.h"
#include "utils/loading_screen.h"
#endif

#include "reimpl/controls.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"

/* Post-first-frame loading allocates a 40MB aligned block plus several 4MB
 * render/resource chunks. 96MB fragments too early; 160MB is Codex's value
 * that had the menu loading. (Restored — heap is not the boot-stall cause.) */
int _newlib_heap_size_user = 224 * 1024 * 1024; /* 2026-07-03: REVERTED 320/288 -> 224 (known-good). LESSON: bumping the newlib heap was BACKWARDS. vitaGL's texture RAM pool = sceKernelGetFreeMemorySize.free_user - ram_threshold, i.e. it comes from what's LEFT of the budget AFTER the newlib heap is reserved at startup. So a BIGGER heap => LESS free => SMALLER vitaGL texture pool => MORE black textures, and at 320 it also starved the ~60MB .so module mappings -> "could not load libGameEngine" at boot. The right lever for texture memory is the ram_threshold (glutil.c vglInitExtended), NOT this heap, and the engine genuinely needs its ~158MB mmap pools reserved by that threshold (dropping threshold to 2MB previously OOM'd the engine). 224 boots reliably; the engine used ~137MB of it this session (87MB free). DO NOT raise this — it hurts textures and boot. */

/* PVR_PSP2 driver modules (libgpu_es4_ext, libIMGEGL, libGLESv2) link against
 * SCE libc and perform internal allocations through it during eglCreateContext.
 * Without an explicit heap size, the default is tiny and eglCreateContext fails
 * with EGL_BAD_ALLOC (0x3003).  The gles2test1 cube sample always sets this
 * to 3 MB regardless of IO preferences. */
int sceLibcHeapSize = 4 * 1024 * 1024;

so_module so_mod_fmod;
so_module so_mod_fmodstudio;
so_module so_mod_sdl2;
so_module so_mod_gameengine;
so_module so_mod_main;

#define PRE_NATIVEINIT_TIMEOUT_MS (45000ULL)
// Boot now reaches resource-set assembly and loads ~776MB of .ttarch2 archives
// from ux0 storage, which legitimately takes minutes; the main thread isn't
// pumping input poll during this load, so don't kill it early.
#define NATIVEINIT_TIMEOUT_MS (600000ULL)
#define POST_NATIVEINIT_TIMEOUT_MS (90000ULL)
#define NATIVEINIT_POLL_STALE_MS (20000ULL)
#define NATIVEINIT_PROGRESS_STALE_MS (30000ULL)
#define NATIVEINIT_PRESENT_STALE_MS (5000ULL)
#define NATIVEINIT_NO_RENDER_TIMEOUT_MS (900000ULL)
#define NATIVEINIT_ALIVE_LOG_EVERY_MS (15000ULL)
#define NATIVEINIT_POKE_START_DELAY_US (500000)
#define NATIVEINIT_POKE_INTERVAL_US (2000000)
#define NATIVEINIT_POKE_MAX (64)
#define NATIVEINIT_LIFECYCLE_POKE_MAX (8)
#define NATIVEINIT_WINDOW_POKE_MAX (NATIVEINIT_LIFECYCLE_POKE_MAX)

typedef struct NativeInitPokeContext {
    void (*on_native_resize)(JNIEnv *env, jclass cls, jint w, jint h, jint format);
    void (*on_native_surface_changed)(JNIEnv *env, jclass cls);
    void (*native_resume)(JNIEnv *env, jclass cls);
    void *(*get_current_window)(void);
    int (*send_window_event)(void *window, unsigned char windowevent, int data1, int data2);
    void (*on_window_shown)(void *window);
    void (*on_window_restored)(void *window);
    void (*on_window_focus_gained)(void *window);
    void (*set_keyboard_focus)(void *window);
    void **android_window_slot;
    void *(*get_window_from_id)(uint32_t id);   /* SDL_GetWindowFromID */
    int fb_w;
    int fb_h;
} NativeInitPokeContext;

static void start_input_poll_thread(void);

#define SDL_WINDOWEVENT_SHOWN 1
#define SDL_WINDOWEVENT_EXPOSED 3
#define SDL_WINDOWEVENT_RESTORED 9
#define SDL_WINDOWEVENT_FOCUS_GAINED 12

static int should_log_nativeinit_poke(int poke_id) {
    return poke_id <= 4 || poke_id == 8 || (poke_id % 8) == 0;
}

static void *resolve_sdl_window(const NativeInitPokeContext *ctx) {
    void *window = NULL;

    if (!ctx) {
        return NULL;
    }

    if (ctx->get_current_window) {
        window = ctx->get_current_window();
    }

    if (!window && ctx->android_window_slot) {
        window = *ctx->android_window_slot;
    }

    /* Fallback: scan whatever windows SDL actually has. SDL assigns window IDs
     * starting at 1, so probe a few — this finds the window even when the
     * current-context and Android_Window slots are still NULL (which is why we
     * kept logging "SDL window not ready"). */
    if (!window && ctx->get_window_from_id) {
        for (uint32_t id = 1; id <= 8 && !window; id++) {
            window = ctx->get_window_from_id(id);
        }
    }

    return window;
}

static void poke_sdl_window_state(const NativeInitPokeContext *ctx, int poke_id) {
    const int verbose = should_log_nativeinit_poke(poke_id);
    void *window = resolve_sdl_window(ctx);
    if (!window) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL window not ready", poke_id);
        }
        return;
    }

    if (verbose) {
        telemetry_log("BOOT", "nativeInit poke #%d: SDL window=%p", poke_id, window);
    }

    if (ctx->send_window_event) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_SendWindowEvent(SHOWN)", poke_id);
        }
        ctx->send_window_event(window, SDL_WINDOWEVENT_SHOWN, 0, 0);
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_SendWindowEvent(EXPOSED)", poke_id);
        }
        ctx->send_window_event(window, SDL_WINDOWEVENT_EXPOSED, 0, 0);
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_SendWindowEvent(RESTORED)", poke_id);
        }
        ctx->send_window_event(window, SDL_WINDOWEVENT_RESTORED, 0, 0);
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_SendWindowEvent(FOCUS_GAINED)", poke_id);
        }
        ctx->send_window_event(window, SDL_WINDOWEVENT_FOCUS_GAINED, 0, 0);
    }

    if (ctx->on_window_shown) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_OnWindowShown", poke_id);
        }
        ctx->on_window_shown(window);
    }
    if (ctx->on_window_restored) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_OnWindowRestored", poke_id);
        }
        ctx->on_window_restored(window);
    }
    if (ctx->set_keyboard_focus) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_SetKeyboardFocus", poke_id);
        }
        ctx->set_keyboard_focus(window);
    }
    if (ctx->on_window_focus_gained) {
        if (verbose) {
            telemetry_log("BOOT", "nativeInit poke #%d: SDL_OnWindowFocusGained", poke_id);
        }
        ctx->on_window_focus_gained(window);
    }
}

static void emit_stall_report(const char *reason, const char *snapshot) {
    char hist[1024];
    char memstats[256];
    char alog_recent[4096];

    launch_state_dump_history(hist, sizeof(hist));
    mem_stats_snapshot(memstats, sizeof(memstats));
    android_log_dump_recent(alog_recent, sizeof(alog_recent));

    telemetry_log("STALL", "BEGIN");
    telemetry_log("STALL", "reason=%s", reason ? reason : "(unknown)");
    telemetry_log("STALL", "snapshot=%s", snapshot ? snapshot : "(none)");
    telemetry_log("STALL", "history=%s", hist);
    telemetry_log("STALL", "memstats=%s", memstats);
    telemetry_log("STALL", "recent_alog_begin");
    {
        const char *p = alog_recent;
        while (p && *p) {
            const char *nl = p;
            while (*nl && *nl != '\n') {
                nl++;
            }

            char line[320];
            size_t len = (size_t)(nl - p);
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1;
            }
            sceClibMemcpy(line, p, len);
            line[len] = '\0';
            telemetry_log("STALL", "alog=%s", line);

            p = (*nl == '\n') ? (nl + 1) : nl;
        }
    }
    telemetry_log("STALL", "recent_alog_end");
    telemetry_log("STALL", "telemetry_path=%s", telemetry_last_path() ? telemetry_last_path() : "(none)");
    telemetry_log("STALL", "END");
}

static void call_module_jni_onload(const char *module_name, so_module *module, LaunchStage stage) {
    launch_state_set_stage(stage);
    jint (*jni_onload)(JavaVM *vm, void *reserved) = (void *)so_symbol(module, "JNI_OnLoad");
    if (jni_onload) {
        telemetry_log("BOOT", "calling JNI_OnLoad (%s)", module_name);
        jni_onload(&jvm, NULL);
        telemetry_log("BOOT", "JNI_OnLoad returned (%s)", module_name);
    } else {
        telemetry_log("BOOT", "JNI_OnLoad missing (%s)", module_name);
    }
}

static void *watchdog_thread(void *arg) {
    (void)arg;
    uint64_t last_log_ms = 0;
    uint64_t last_nativeinit_alive_log_ms = 0;
    telemetry_log("WATCH", "watchdog thread started");

    while (1) {
        sceKernelDelayThread(1000000);

        const LaunchStage stage = launch_state_get_stage();
        const uint64_t stage_age_ms = launch_state_stage_age_ms();
        const uint64_t uptime_ms = launch_state_uptime_ms();

        if (uptime_ms - last_log_ms >= 5000) {
            char snap[320];
            char memstats[256];
            launch_state_snapshot(snap, sizeof(snap));
            mem_stats_snapshot(memstats, sizeof(memstats));
            telemetry_log("WATCH", "%s", snap);
            telemetry_log("WATCH", "%s", memstats);
            last_log_ms = uptime_ms;
        }

        // Early boot should not take this long unless blocked in loader setup.
        if (stage < LS_NATIVEINIT_CALL && stage_age_ms > PRE_NATIVEINIT_TIMEOUT_MS) {
            char snap[320];
            launch_state_snapshot(snap, sizeof(snap));
            emit_stall_report("before_nativeInit_timeout", snap);
            fatal_error("Launch stalled before nativeInit.\nSee STALL block in loader.log");
        }

        if (stage == LS_NATIVEINIT_CALL && stage_age_ms > NATIVEINIT_TIMEOUT_MS) {
            const uint32_t polls = launch_state_get_poll_count();
            const uint64_t poll_age_ms = launch_state_last_poll_age_ms();
            const int has_recent_poll = (polls > 0) && (poll_age_ms <= NATIVEINIT_POLL_STALE_MS);
            const uint32_t progress = launch_state_get_progress_count();
            const uint64_t progress_age_ms = launch_state_last_progress_age_ms();
            const int has_recent_progress = (progress > 0) && (progress_age_ms <= NATIVEINIT_PROGRESS_STALE_MS);

            // Ground truth for "actually running": frames pushed to the display.
            // The input poll heartbeat alone only proves the SDL event loop ticks,
            // not that anything is being rendered (classic black-screen case).
            const uint32_t frames = launch_state_get_present_count();
            const uint64_t present_age_ms = launch_state_last_present_age_ms();
            const int has_recent_present = (frames > 0) && (present_age_ms <= NATIVEINIT_PRESENT_STALE_MS);

            if (has_recent_present) {
                // Genuinely rendering. Report the honest, verified-alive state.
                if (uptime_ms - last_nativeinit_alive_log_ms >= NATIVEINIT_ALIVE_LOG_EVERY_MS) {
                    telemetry_log("WATCH",
                                  "nativeInit alive (RENDERING): frames=%u present_age=%llums poll=%u",
                                  frames,
                                  (unsigned long long)present_age_ms,
                                  polls);
                    last_nativeinit_alive_log_ms = uptime_ms;
                }
            } else if ((has_recent_poll || has_recent_progress) &&
                       frames == 0 &&
                       stage_age_ms <= NATIVEINIT_NO_RENDER_TIMEOUT_MS) {
                if (uptime_ms - last_nativeinit_alive_log_ms >= NATIVEINIT_ALIVE_LOG_EVERY_MS) {
                    telemetry_log("WATCH",
                                  "nativeInit LOADING: 0 frames presented, poll=%u poll_age=%llums "
                                  "progress=%u progress_age=%llums - still inside nativeInit",
                                  polls,
                                  (unsigned long long)poll_age_ms,
                                  progress,
                                  (unsigned long long)progress_age_ms);
                    last_nativeinit_alive_log_ms = uptime_ms;
                }
            } else {
                // 2026-06-22: THE GAME NOW RUNS (3D renders) but at ~5fps with
                // HORRENDOUS scene loads, so frames legitimately freeze for long
                // stretches mid-load while the game is very much alive (input poll,
                // progress and alog all keep ticking). The old fatal_error here
                // MURDERED working, input-responsive runs mid-load. Per user: do NOT
                // kill on a frozen-frame stall — just log it for telemetry and let
                // the game keep running. (A truly dead hang now just sits there; the
                // user can close it manually, which is far better than nuking a
                // working game.)
                if (uptime_ms - last_nativeinit_alive_log_ms >= NATIVEINIT_ALIVE_LOG_EVERY_MS) {
                    char snap[320];
                    launch_state_snapshot(snap, sizeof(snap));
                    telemetry_log("WATCH",
                                  "nativeInit frames frozen (slow load / low fps) — NOT killing: %s",
                                  snap);
                    last_nativeinit_alive_log_ms = uptime_ms;
                }
            }
        }

        // If nativeInit returned but menu never appears, capture that separately.
        if (stage == LS_NATIVEINIT_RETURN && stage_age_ms > POST_NATIVEINIT_TIMEOUT_MS) {
            char snap[320];
            launch_state_snapshot(snap, sizeof(snap));
            emit_stall_report("after_nativeInit_timeout", snap);
            fatal_error("Launch stalled after nativeInit return.\nSee STALL block in loader.log");
        }
    }

    return NULL;
}

static void *nativeinit_poke_thread(void *arg) {
    const NativeInitPokeContext *ctx = (const NativeInitPokeContext *)arg;
    int poke_count = 0;

    sceKernelDelayThread(NATIVEINIT_POKE_START_DELAY_US);

    while (launch_state_get_stage() == LS_NATIVEINIT_CALL && poke_count < NATIVEINIT_POKE_MAX) {
        const int poke_id = poke_count + 1;
        const int verbose = should_log_nativeinit_poke(poke_id);

        if (ctx->on_native_surface_changed && poke_count < NATIVEINIT_LIFECYCLE_POKE_MAX) {
            if (verbose) {
                telemetry_log("BOOT", "nativeInit poke #%d: onNativeSurfaceChanged", poke_id);
            }
            ctx->on_native_surface_changed(&jni, NULL);
        }

        if (ctx->native_resume && poke_count == 0) {
            telemetry_log("BOOT", "nativeInit poke #%d: nativeResume", poke_id);
            ctx->native_resume(&jni, NULL);
        }

        if (ctx->on_native_resize && poke_count == 0) {
            telemetry_log("BOOT", "nativeInit poke #%d: onNativeResize(%d,%d,RGBA8888)",
                          poke_id,
                          ctx->fb_w,
                          ctx->fb_h);
            ctx->on_native_resize(&jni, NULL, ctx->fb_w, ctx->fb_h, 1);
        }

        if ((ctx->get_current_window || ctx->android_window_slot ||
             ctx->get_window_from_id) &&
            poke_count < NATIVEINIT_WINDOW_POKE_MAX) {
            poke_sdl_window_state(ctx, poke_id);
        }

        poke_count++;
        sceKernelDelayThread(NATIVEINIT_POKE_INTERVAL_US);
    }

    telemetry_log("BOOT",
                  "nativeInit poke thread exit: stage=%s pokes=%d",
                  launch_state_get_stage_name(launch_state_get_stage()),
                  poke_count);
    return NULL;
}

#define INPUT_READY_PRESENT_FRAMES 60U

static int input_delivery_ready(void) {
    return launch_state_scene_active() ||
           launch_state_get_present_count() >= INPUT_READY_PRESENT_FRAMES;
}

static void *input_poll_thread(void *arg) {
    (void)arg;

    telemetry_log("INPUT", "background poll thread waiting for SDL init");
    while (launch_state_get_stage() < LS_NATIVE_RESIZE) {
        sceKernelDelayThread(50000);
    }

    int (*android_jni_setup_thread)(void) =
            (void *)so_symbol(&so_mod_sdl2, "Android_JNI_SetupThread");
    if (android_jni_setup_thread) {
        telemetry_log("INPUT", "Android_JNI_SetupThread -> %d", android_jni_setup_thread());
    } else {
        telemetry_log("INPUT", "Android_JNI_SetupThread missing; using global FalsoJNI env");
    }

    unsigned int poll_count = 0;
    unsigned int gated_count = 0;
    while (1) {
        if (!input_delivery_ready()) {
            gated_count++;
            if (gated_count <= 4U || (gated_count & 0x1fffU) == 0U) {
                telemetry_log("INPUT", "background poll gated count=%u stage=%s scene=%d frames=%u",
                              gated_count,
                              launch_state_get_stage_name(launch_state_get_stage()),
                              launch_state_scene_active(),
                              launch_state_get_present_count());
            }
            sceKernelDelayThread(50000);
            continue;
        }

        controls_poll();
        poll_count++;
        if (poll_count <= 4U || (poll_count & 0x1fffU) == 0U) {
            telemetry_log("INPUT", "background poll count=%u stage=%s frames=%u",
                          poll_count,
                          launch_state_get_stage_name(launch_state_get_stage()),
                          launch_state_get_present_count());
        }
        sceKernelDelayThread(16666);
    }

    return NULL;
}

static void start_input_poll_thread(void) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x40000);

    if (pthread_create(&thread, &attr, input_poll_thread, NULL) == 0) {
        pthread_detach(thread);
        telemetry_log("INPUT", "background poll thread started");
    } else {
        l_warn("failed to create input poll thread");
    }
}

static void *run_game(void *arg) {
    (void)arg;
    launch_state_set_stage(LS_RUN_THREAD_START);
    telemetry_log("BOOT", "run_game thread started");

    // FMOD keeps a JavaVM pointer in JNI_OnLoad; without this it can crash in audio bring-up.
    call_module_jni_onload("libfmod.so", &so_mod_fmod, LS_JNI_ONLOAD_FMOD);
    call_module_jni_onload("libfmodstudio.so", &so_mod_fmodstudio, LS_JNI_ONLOAD_FMODSTUDIO);
    call_module_jni_onload("libSDL2.so", &so_mod_sdl2, LS_JNI_ONLOAD_SDL2);

    // Force Vita-sized surface metrics into SDL Android glue before nativeInit.
    const int fb_w = mcsm_get_framebuffer_width();
    const int fb_h = mcsm_get_framebuffer_height();
    void (*on_native_resize)(JNIEnv *env, jclass cls, jint w, jint h, jint format) =
            (void *)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeResize");
    if (on_native_resize) {
        launch_state_set_stage(LS_NATIVE_RESIZE);
        telemetry_log("BOOT", "calling onNativeResize(%d,%d,RGBA8888)", fb_w, fb_h);
        on_native_resize(&jni, NULL, fb_w, fb_h, 1);
#ifdef USE_PVR_PSP2
        loading_screen_set_status("Surface configured: %dx%d", fb_w, fb_h);
        loading_screen_render();
#endif
    } else {
        l_warn("onNativeResize not found in libSDL2.so.");
    }

    void (*on_native_surface_changed)(JNIEnv *env, jclass cls) =
            (void *)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged");
    if (on_native_surface_changed) {
        launch_state_set_stage(LS_SURFACE_CHANGED);
        telemetry_log("BOOT", "calling onNativeSurfaceChanged");
        on_native_surface_changed(&jni, NULL);
    } else {
        l_warn("onNativeSurfaceChanged not found in libSDL2.so.");
    }

    void (*native_resume)(JNIEnv *env, jclass cls) =
            (void *)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_nativeResume");
    if (native_resume) {
        telemetry_log("BOOT", "nativeResume deferred to nativeInit poke");
    } else {
        l_warn("nativeResume not found in libSDL2.so.");
    }

    void *(*get_current_window)(void) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_GL_GetCurrentWindow_REAL");
    int (*send_window_event)(void *window, unsigned char windowevent, int data1, int data2) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_SendWindowEvent");
    void (*on_window_shown)(void *window) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_OnWindowShown");
    void (*on_window_restored)(void *window) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_OnWindowRestored");
    void (*on_window_focus_gained)(void *window) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_OnWindowFocusGained");
    void (*set_keyboard_focus)(void *window) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_SetKeyboardFocus");
    void **android_window_slot = (void **)so_symbol(&so_mod_sdl2, "Android_Window");
    void *(*get_window_from_id)(uint32_t id) =
            (void *)so_symbol(&so_mod_sdl2, "SDL_GetWindowFromID");

    void (*native_init)(JNIEnv *env, jclass cls, jobjectArray args) =
            (void *)so_symbol(&so_mod_main, "Java_org_libsdl_app_SDLActivity_nativeInit");
    if (native_init) {
        NativeInitPokeContext poke_ctx;
        poke_ctx.on_native_resize = on_native_resize;
        poke_ctx.on_native_surface_changed = on_native_surface_changed;
        poke_ctx.native_resume = native_resume;
        poke_ctx.get_current_window = get_current_window;
        poke_ctx.send_window_event = send_window_event;
        poke_ctx.on_window_shown = on_window_shown;
        poke_ctx.on_window_restored = on_window_restored;
        poke_ctx.on_window_focus_gained = on_window_focus_gained;
        poke_ctx.set_keyboard_focus = set_keyboard_focus;
        poke_ctx.get_window_from_id = get_window_from_id;
        poke_ctx.android_window_slot = android_window_slot;
        poke_ctx.fb_w = fb_w;
        poke_ctx.fb_h = fb_h;

        pthread_t poke_thread;
        int poke_thread_started = 0;

        if (on_native_resize || on_native_surface_changed || native_resume) {
            pthread_attr_t poke_attr;
            pthread_attr_init(&poke_attr);
            pthread_attr_setstacksize(&poke_attr, 0x40000);
            if (pthread_create(&poke_thread, &poke_attr, nativeinit_poke_thread, &poke_ctx) == 0) {
                poke_thread_started = 1;
                telemetry_log("BOOT", "started nativeInit lifecycle poke thread");
            } else {
                l_warn("failed to create nativeInit poke thread");
            }
        }

        launch_state_set_stage(LS_NATIVEINIT_CALL);
        telemetry_log("BOOT", "calling nativeInit");
#ifdef USE_PVR_PSP2
        loading_screen_set_status("Native engine starting...");
        loading_screen_set_progress(0.35f);
        loading_screen_render();
#endif

        native_init(&jni, NULL, NULL);
        launch_state_set_stage(LS_NATIVEINIT_RETURN);
        telemetry_log("BOOT", "nativeInit returned");
#ifdef USE_PVR_PSP2
        loading_screen_set_status("Game engine initialized.");
        loading_screen_set_progress(0.85f);
        loading_screen_render();
#endif

        if (poke_thread_started) {
            pthread_join(poke_thread, NULL);
        }
    } else {
        telemetry_log("BOOT", "nativeInit missing");
    }

    int (*sdl_main)(void) = (void *)so_symbol(&so_mod_main, "SDL_main");
    if (!sdl_main && !native_init) {
        telemetry_log("BOOT", "SDL_main missing");
        fatal_error("Neither nativeInit nor SDL_main was found in libmain.so.");
    }

    if (sdl_main) {
        launch_state_set_stage(LS_SDL_MAIN_CALL);
        telemetry_log("BOOT", "calling SDL_main");
#ifdef USE_PVR_PSP2
        loading_screen_set_status("Starting game...");
        loading_screen_set_progress(0.95f);
        loading_screen_render();
#endif
        sdl_main();
        launch_state_set_stage(LS_SDL_MAIN_RETURN);
        telemetry_log("BOOT", "SDL_main returned");
    }

    launch_state_set_stage(LS_IDLE_LOOP);
    telemetry_log("BOOT", "run_game idle loop entered");
    while (1) {
        sceKernelDelayThread(1000000);
    }

    return NULL;
}

int main(void) {
    launch_state_set_stage(LS_BOOT);

    telemetry_reset();
    if (telemetry_success_count() <= 0) {
        fatal_error("DIAG: No writable telemetry paths.\nChecked ux0:/ and ur0:/ data paths.");
    }
    telemetry_log("BOOT", "main entered");
    telemetry_log("BOOT", "telemetry path: %s", telemetry_last_path() ? telemetry_last_path() : "(none)");
    telemetry_log("BOOT", "build: %s %s", __DATE__, __TIME__);
    telemetry_log("BOOT", "heap_user_mb=%d", _newlib_heap_size_user / (1024 * 1024));

    launch_state_set_stage(LS_SOLOADER_INIT);
    telemetry_log("BOOT", "calling soloader_init_all");
    soloader_init_all();
    telemetry_log("BOOT", "soloader_init_all returned");
    start_input_poll_thread();

    pthread_t watchdog;
    pthread_attr_t watchdog_attr;
    pthread_attr_init(&watchdog_attr);
    pthread_attr_setstacksize(&watchdog_attr, 0x100000);
    pthread_create(&watchdog, &watchdog_attr, watchdog_thread, NULL);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0xA00000);
    telemetry_log("BOOT", "creating run_game thread");
    pthread_create(&thread, &attr, run_game, NULL);
    telemetry_log("BOOT", "waiting for run_game thread");
    pthread_join(thread, NULL);
    telemetry_log("BOOT", "run_game thread joined");

    sceKernelExitDeleteThread(0);
}

typedef void (*SdlNativeKeyFn)(JNIEnv *env, jclass cls, jint keycode);
typedef void (*SdlNativePadFn)(JNIEnv *env, jclass cls, jint device_id, jint keycode);
typedef void (*SdlNativeTouchFn)(JNIEnv *env,
                                 jclass cls,
                                 jint touch_device_id,
                                 jint pointer_finger_id,
                                 jint action,
                                 jfloat x,
                                 jfloat y,
                                 jfloat pressure);
typedef void (*SdlNativeJoyFn)(JNIEnv *env, jclass cls, jint device_id, jint axis, jfloat value);
/* Bundled SDL Android: nativeAddJoystick(device_id,name,desc,is_accel,nbuttons,naxes,nhats,nballs). */
typedef jint (*SdlAddJoystickFn)(JNIEnv *env, jclass cls, jint device_id, jobject name, jobject desc,
                                 jboolean is_accel, jint nbuttons, jint naxes, jint nhats, jint nballs);
typedef void *(*SdlGetWindowFn)(void);
typedef void *(*SdlGetWindowFromIdFn)(uint32_t id);
typedef void (*SdlSetWindowFocusFn)(void *window);
typedef int (*SdlSendMouseMotionFn)(void *window, uint32_t mouse_id, int relative, int x, int y);
typedef int (*SdlSendMouseButtonFn)(void *window, uint32_t mouse_id, uint8_t state, uint8_t button);
typedef int (*SdlAddTouchFn)(int64_t touch_id);
typedef int (*SdlSendTouchFn)(int64_t touch_id, int64_t finger_id, int down, float x, float y, float pressure);
typedef int (*SdlSendTouchMotionFn)(int64_t touch_id, int64_t finger_id, float x, float y, float pressure);

typedef struct SdlTouchFingerEventBridge {
    uint32_t type;
    uint32_t timestamp;
    int64_t touch_id;
    int64_t finger_id;
    float x;
    float y;
    float dx;
    float dy;
    float pressure;
    uint32_t window_id;
} SdlTouchFingerEventBridge;

typedef struct SdlMouseButtonEventBridge {
    uint32_t type;
    uint32_t timestamp;
    uint32_t window_id;
    uint32_t which;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    uint8_t padding1;
    int32_t x;
    int32_t y;
} SdlMouseButtonEventBridge;

typedef struct SdlKeyboardEventBridge {
    uint32_t type;
    uint32_t timestamp;
    uint32_t window_id;
    uint8_t state;
    uint8_t repeat;
    uint8_t padding2;
    uint8_t padding3;
    int32_t scancode;
    int32_t sym;
    uint16_t mod;
    uint16_t padding4;
    uint32_t unused;
} SdlKeyboardEventBridge;

typedef void (*AppSdlFingeringFn)(int event_type, const SdlTouchFingerEventBridge *event);
typedef void (*AppSdlMouseEventFn)(int event_type, const SdlMouseButtonEventBridge *event);
typedef void (*AppSdlKeyEventFn)(int event_type, const SdlKeyboardEventBridge *event);
typedef int (*GameWindowPlayMouseFn)(void *self, unsigned int message, int button, int cursor, int unused);
typedef void (*TouchSetLegacyPointerFn)(void *self, const int32_t *position);
typedef void (*EngineInputQueueFn)(int input_code,
                                   int event_type,
                                   float x,
                                   float y,
                                   void *agent,
                                   int priority,
                                   void *mapper_ptr);

static SdlNativeKeyFn g_sdl_key_down = NULL;
static SdlNativeKeyFn g_sdl_key_up = NULL;
static SdlNativePadFn g_sdl_pad_down = NULL;
static SdlNativePadFn g_sdl_pad_up = NULL;
static SdlNativeTouchFn g_sdl_touch = NULL;
static SdlNativeJoyFn g_sdl_joy = NULL;
static SdlAddJoystickFn g_sdl_add_joystick = NULL;
typedef int (*SdlAddMappingFn)(const char *);
static SdlAddMappingFn g_sdl_add_mapping = NULL;   /* SDL_GameControllerAddMapping (2026-07-17) */
static SdlGetWindowFn g_sdl_get_focus_window = NULL;
static SdlGetWindowFromIdFn g_sdl_get_window_from_id = NULL;
static SdlSetWindowFocusFn g_sdl_set_mouse_focus = NULL;
static SdlSetWindowFocusFn g_sdl_set_keyboard_focus = NULL;
static SdlSendMouseMotionFn g_sdl_send_mouse_motion = NULL;
static SdlSendMouseButtonFn g_sdl_send_mouse_button = NULL;
static SdlAddTouchFn g_sdl_add_touch = NULL;
static SdlSendTouchFn g_sdl_send_touch = NULL;
static SdlSendTouchMotionFn g_sdl_send_touch_motion = NULL;
static void **g_sdl_android_window_slot = NULL;
static AppSdlFingeringFn g_app_on_fingering = NULL;
static AppSdlMouseEventFn g_app_on_mouse_event = NULL;
static AppSdlKeyEventFn g_app_on_key_event = NULL;
static GameWindowPlayMouseFn g_gamewindow_mouse_move = NULL;
static GameWindowPlayMouseFn g_gamewindow_mouse_down = NULL;
static GameWindowPlayMouseFn g_gamewindow_mouse_up = NULL;
static void **g_gamewindow_slot = NULL;
static TouchSetLegacyPointerFn g_touch_set_legacy_pointer = NULL;
static void *g_touch_screen_state = NULL;
static EngineInputQueueFn g_platform_input_queue = NULL;
static EngineInputQueueFn g_input_mapper_queue = NULL;
static int g_sdl_input_symbols_resolved = 0;
static int g_legacy_touch_pointer_enabled = -1;

#define VITA_TOUCH_W 960.0f
#define VITA_TOUCH_H 544.0f

/* RENDER-SCALE TOUCH FIX 2026-06-29: touch/mouse pixel coords are built in the
 * native 960x544 touchscreen space, but with fb_override the engine's window is
 * the low-res FBO (e.g. 640x363). The engine hit-tests pixel coords in ITS space,
 * so unscaled 960-space pixels land at the wrong place -> imprecise touch and
 * in-game buttons that register in the loader but miss their target. Scale pixel
 * coords to the render-scale resolution. Normalized finger coords (nx,ny) are
 * resolution-independent and need no change. At native res this is identity. */
static int rs_map_px_x(float x960) {
    return (int)(x960 * (float)mcsm_get_render_scale_width() / VITA_TOUCH_W + 0.5f);
}
static int rs_map_px_y(float y960) {
    return (int)(y960 * (float)mcsm_get_render_scale_height() / VITA_TOUCH_H + 0.5f);
}

#define ANDROID_MOTION_ACTION_DOWN 0
#define ANDROID_MOTION_ACTION_UP   1
#define ANDROID_MOTION_ACTION_MOVE 2
#define ANDROID_MOTION_ACTION_POINTER_DOWN 5
#define ANDROID_MOTION_ACTION_POINTER_UP   6
#define ANDROID_AXIS_X 0
#define ANDROID_AXIS_Y 1
#define ANDROID_AXIS_Z 11
#define ANDROID_AXIS_RZ 14
#define SDL_MOUSE_ID_VITA 0U
#define SDL_RELEASED 0
#define SDL_PRESSED 1
#define SDL_BUTTON_LEFT 1
#define SDL_TOUCH_ID_VITA 1LL
#define SDL_JOYSTICK_ID_VITA 0
#define SDL_JOYSTICK_BUTTONS_VITA 22
#define SDL_JOYSTICK_AXES_VITA 6   /* 0-3 = 2 sticks, 4/5 = L2/R2 trigger axes (2026-07-17) */
#define SDL_JOYSTICK_HATS_VITA 0
#define SDL_KEYDOWN 0x300U
#define SDL_KEYUP 0x301U
#define SDL_FINGERDOWN 0x700U
#define SDL_FINGERUP 0x701U
#define SDL_FINGERMOTION 0x702U
#define SDL_MOUSEMOTION 0x400U
#define SDL_MOUSEBUTTONDOWN 0x401U
#define SDL_MOUSEBUTTONUP 0x402U
#define ENGINE_EVENT_DOWN 0
#define ENGINE_EVENT_UP 1
#define ENGINE_EVENT_MOVE 2
#define APP_SDL_TOUCHSCREENSTATE_DELTA 0x00ABFCE0U

#define SDLK_RETURN 13
#define SDLK_ESCAPE 27
#define SDLK_SPACE 32
#define SDLK_LEFT 0x40000050
#define SDLK_DOWN 0x40000051
#define SDLK_UP 0x40000052
#define SDLK_RIGHT 0x4000004F

#define MCSM_INPUT_BUTTON_0 0x0200
#define MCSM_INPUT_BUTTON_1 0x0201
#define MCSM_INPUT_BUTTON_2 0x0202
#define MCSM_INPUT_BUTTON_3 0x0203
#define MCSM_INPUT_BUTTON_4 0x0204
#define MCSM_INPUT_BUTTON_5 0x0205
#define MCSM_INPUT_BUTTON_6 0x0206
#define MCSM_INPUT_BUTTON_7 0x0207
#define MCSM_INPUT_BUTTON_BACK MCSM_INPUT_BUTTON_6
#define MCSM_INPUT_BUTTON_START MCSM_INPUT_BUTTON_7
#define MCSM_INPUT_DPAD_UP 0x020C
#define MCSM_INPUT_DPAD_DOWN 0x020D
#define MCSM_INPUT_DPAD_RIGHT 0x020E
#define MCSM_INPUT_DPAD_LEFT 0x020F
#define MCSM_INPUT_ABSTRACT_CONFIRM 0x0300
#define MCSM_INPUT_ABSTRACT_CANCEL 0x0301
#define MCSM_INPUT_LEFT_STICK_MOVE 0x0400
#define MCSM_INPUT_RIGHT_STICK_MOVE 0x0401

#define MCSM_VITA_SQUARE 0x1500
#define MCSM_VITA_TRIANGLE 0x1501
#define MCSM_VITA_CIRCLE 0x1502
#define MCSM_VITA_CROSS 0x1503
#define MCSM_VITA_L1 0x1504
#define MCSM_VITA_L2 0x1505
#define MCSM_VITA_R1 0x1507
#define MCSM_VITA_R2 0x1508
#define MCSM_VITA_START 0x150A
#define MCSM_VITA_SELECT 0x150B
#define MCSM_VITA_LEFT 0x150C
#define MCSM_VITA_RIGHT 0x150D
#define MCSM_VITA_UP 0x150E
#define MCSM_VITA_DOWN 0x150F

static float clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static int legacy_touch_pointer_enabled(void) {
    if (g_legacy_touch_pointer_enabled < 0) {
        SceUID fd = sceIoOpen("ux0:data/mcsm/legacytouch.txt", SCE_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            g_legacy_touch_pointer_enabled = 1;
            l_info("INPUT legacy TouchScreenState pointer path enabled by legacytouch.txt");
        } else {
            g_legacy_touch_pointer_enabled = 0;
        }
    }
    return g_legacy_touch_pointer_enabled;
}

/* Application_SDL::OnFingering rejects finger IDs >= 11. Vita touch report IDs
 * are hardware/system IDs, so keep a compact engine-facing slot table. */
#define MAX_ACTIVE_TOUCH_IDS 11

static int g_active_touch_ids[MAX_ACTIVE_TOUCH_IDS];
static uint8_t g_active_touch_used[MAX_ACTIVE_TOUCH_IDS];
static unsigned g_active_touch_count = 0;

static int active_touch_find(int32_t id) {
    for (int i = 0; i < MAX_ACTIVE_TOUCH_IDS; ++i) {
        if (g_active_touch_used[i] && g_active_touch_ids[i] == id) {
            return i;
        }
    }
    return -1;
}

static int active_touch_add(int32_t id) {
    const int existing = active_touch_find(id);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < MAX_ACTIVE_TOUCH_IDS; ++i) {
        if (!g_active_touch_used[i]) {
            g_active_touch_used[i] = 1;
            g_active_touch_ids[i] = id;
            g_active_touch_count++;
            return i;
        }
    }
    return -1;
}

static void active_touch_remove(int32_t id) {
    const int idx = active_touch_find(id);
    if (idx < 0) {
        return;
    }
    g_active_touch_used[idx] = 0;
    g_active_touch_ids[idx] = 0;
    if (g_active_touch_count > 0) {
        g_active_touch_count--;
    }
}

static int android_motion_action_for_touch(int32_t id, ControlsAction action) {
    switch (action) {
        case CONTROLS_ACTION_DOWN: {
            const int already_active = active_touch_find(id) >= 0;
            const int android_action =
                (!already_active && g_active_touch_count > 0)
                    ? ANDROID_MOTION_ACTION_POINTER_DOWN
                    : ANDROID_MOTION_ACTION_DOWN;
            (void)active_touch_add(id);
            return android_action;
        }
        case CONTROLS_ACTION_UP: {
            const int android_action =
                (active_touch_find(id) >= 0 && g_active_touch_count > 1)
                    ? ANDROID_MOTION_ACTION_POINTER_UP
                    : ANDROID_MOTION_ACTION_UP;
            active_touch_remove(id);
            return android_action;
        }
        case CONTROLS_ACTION_MOVE:
        default:
            if (active_touch_find(id) < 0) {
                const int android_action =
                    (g_active_touch_count > 0)
                        ? ANDROID_MOTION_ACTION_POINTER_DOWN
                        : ANDROID_MOTION_ACTION_DOWN;
                (void)active_touch_add(id);
                return android_action;
            }
            return ANDROID_MOTION_ACTION_MOVE;
    }
}

static void resolve_sdl_input_symbols(void) {
    if (g_sdl_input_symbols_resolved) {
        return;
    }
    g_sdl_input_symbols_resolved = 1;

    g_sdl_key_down = (SdlNativeKeyFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
    g_sdl_key_up = (SdlNativeKeyFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
    g_sdl_pad_down = (SdlNativePadFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativePadDown");
    g_sdl_pad_up = (SdlNativePadFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativePadUp");
    g_sdl_touch = (SdlNativeTouchFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeTouch");
    g_sdl_joy = (SdlNativeJoyFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_onNativeJoy");
    g_sdl_add_joystick = (SdlAddJoystickFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_nativeAddJoystick");
    g_sdl_get_focus_window = (SdlGetWindowFn)so_symbol(&so_mod_sdl2, "SDL_GetFocusWindow");
    if (!g_sdl_get_focus_window) {
        g_sdl_get_focus_window = (SdlGetWindowFn)so_symbol(&so_mod_sdl2, "SDL_GetFocusWindow_REAL");
    }
    g_sdl_get_window_from_id = (SdlGetWindowFromIdFn)so_symbol(&so_mod_sdl2, "SDL_GetWindowFromID");
    if (!g_sdl_get_window_from_id) {
        g_sdl_get_window_from_id = (SdlGetWindowFromIdFn)so_symbol(&so_mod_sdl2, "SDL_GetWindowFromID_REAL");
    }
    g_sdl_set_mouse_focus = (SdlSetWindowFocusFn)so_symbol(&so_mod_sdl2, "SDL_SetMouseFocus");
    g_sdl_set_keyboard_focus = (SdlSetWindowFocusFn)so_symbol(&so_mod_sdl2, "SDL_SetKeyboardFocus");
    g_sdl_send_mouse_motion = (SdlSendMouseMotionFn)so_symbol(&so_mod_sdl2, "SDL_SendMouseMotion");
    g_sdl_send_mouse_button = (SdlSendMouseButtonFn)so_symbol(&so_mod_sdl2, "SDL_SendMouseButton");
    g_sdl_add_touch = (SdlAddTouchFn)so_symbol(&so_mod_sdl2, "SDL_AddTouch");
    g_sdl_send_touch = (SdlSendTouchFn)so_symbol(&so_mod_sdl2, "SDL_SendTouch");
    g_sdl_send_touch_motion = (SdlSendTouchMotionFn)so_symbol(&so_mod_sdl2, "SDL_SendTouchMotion");
    g_sdl_android_window_slot = (void **)so_symbol(&so_mod_sdl2, "Android_Window");
    g_app_on_key_event = (AppSdlKeyEventFn)so_symbol(&so_mod_gameengine, "_ZN15Application_SDL10OnKeyEventEN11InputMapper9EventTypeERK17SDL_KeyboardEvent");
    g_app_on_fingering = (AppSdlFingeringFn)so_symbol(&so_mod_gameengine, "_ZN15Application_SDL11OnFingeringEN11InputMapper9EventTypeERK20SDL_TouchFingerEvent");
    g_app_on_mouse_event = (AppSdlMouseEventFn)so_symbol(&so_mod_gameengine, "_ZN15Application_SDL12OnMouseEventEN11InputMapper9EventTypeERK20SDL_MouseButtonEvent");
    g_gamewindow_mouse_move = (GameWindowPlayMouseFn)so_symbol(&so_mod_gameengine, "_ZN19GameWindow_PlayMode11OnMouseMoveEjiii");
    g_gamewindow_mouse_down = (GameWindowPlayMouseFn)so_symbol(&so_mod_gameengine, "_ZN19GameWindow_PlayMode11OnMouseDownEjiii");
    g_gamewindow_mouse_up = (GameWindowPlayMouseFn)so_symbol(&so_mod_gameengine, "_ZN19GameWindow_PlayMode9OnMouseUpEjiii");
    g_gamewindow_slot = (void **)so_symbol(&so_mod_gameengine, "_ZN10GameWindow10smpGameWinE");
    g_touch_set_legacy_pointer = (TouchSetLegacyPointerFn)so_symbol(&so_mod_gameengine, "_ZN16TouchScreenState24SetLegacyPointerPositionERK8Vector2I");
    g_platform_input_queue = (EngineInputQueueFn)so_symbol(&so_mod_gameengine, "_ZN19PlatformInputMapper10QueueEventEiN11InputMapper9EventTypeEffP5Agenti3PtrIS0_E");
    g_input_mapper_queue = (EngineInputQueueFn)so_symbol(&so_mod_gameengine, "_ZN11InputMapper10QueueEventEiNS_9EventTypeEffP5Agenti3PtrIS_E");
    {
        uintptr_t get_system_pointer_pos =
            so_symbol(&so_mod_gameengine, "_ZN15Application_SDL19GetSystemPointerPosEi");
        if (get_system_pointer_pos) {
            g_touch_screen_state =
                (void *)(get_system_pointer_pos + APP_SDL_TOUCHSCREENSTATE_DELTA);
        }
    }

    l_info("SDL input symbols: key=%d/%d pad=%d/%d touch=%d joy=%d addjoy=%d mouse=%d/%d direct_touch=%d/%d/%d win=%d/%d/%d engine=%d/%d/%d queues=%d/%d gw=%d/%d/%d/%d legacy=%d/%p",
           g_sdl_key_down != NULL,
           g_sdl_key_up != NULL,
           g_sdl_pad_down != NULL,
           g_sdl_pad_up != NULL,
           g_sdl_touch != NULL,
           g_sdl_joy != NULL,
           g_sdl_add_joystick != NULL,
           g_sdl_send_mouse_motion != NULL,
           g_sdl_send_mouse_button != NULL,
           g_sdl_add_touch != NULL,
           g_sdl_send_touch != NULL,
           g_sdl_send_touch_motion != NULL,
           g_sdl_get_focus_window != NULL,
           g_sdl_get_window_from_id != NULL,
           g_sdl_android_window_slot != NULL,
           g_app_on_key_event != NULL,
           g_app_on_fingering != NULL,
           g_app_on_mouse_event != NULL,
           g_platform_input_queue != NULL,
           g_input_mapper_queue != NULL,
           g_gamewindow_mouse_move != NULL,
           g_gamewindow_mouse_down != NULL,
           g_gamewindow_mouse_up != NULL,
           g_gamewindow_slot != NULL,
           g_touch_set_legacy_pointer != NULL,
           g_touch_screen_state);
}

static int controls_engine_event_type(ControlsAction action) {
    switch (action) {
        case CONTROLS_ACTION_DOWN:
            return ENGINE_EVENT_DOWN;
        case CONTROLS_ACTION_UP:
            return ENGINE_EVENT_UP;
        case CONTROLS_ACTION_MOVE:
        default:
            return ENGINE_EVENT_MOVE;
    }
}

static uint32_t controls_sdl_finger_event_type(ControlsAction action) {
    switch (action) {
        case CONTROLS_ACTION_DOWN:
            return SDL_FINGERDOWN;
        case CONTROLS_ACTION_UP:
            return SDL_FINGERUP;
        case CONTROLS_ACTION_MOVE:
        default:
            return SDL_FINGERMOTION;
    }
}

static int controls_emit_engine_touch(int compact_id,
                                      float x,
                                      float y,
                                      float nx,
                                      float ny,
                                      ControlsAction action) {
    static unsigned log_count = 0;
    const int event_type = controls_engine_event_type(action);
    const int px = rs_map_px_x(x);
    const int py = rs_map_px_y(y);
    void *gamewindow = (g_gamewindow_slot && *g_gamewindow_slot) ? *g_gamewindow_slot : NULL;
    const int gamewindow_ready = gamewindow != NULL;
    int finger_sent = 0;
    int mouse_sent = 0;
    int legacy_sent = 0;
    int gw_rc = 0;
    int gw_sent = 0;

    if (legacy_touch_pointer_enabled() && g_touch_set_legacy_pointer && g_touch_screen_state) {
        int32_t pos[2] = {px, py};
        g_touch_set_legacy_pointer(g_touch_screen_state, pos);
        legacy_sent = 1;
    }

    if (g_app_on_fingering) {
        SdlTouchFingerEventBridge finger_event = {0};
        finger_event.type = controls_sdl_finger_event_type(action);
        finger_event.touch_id = SDL_TOUCH_ID_VITA;
        finger_event.finger_id = (int64_t)compact_id;
        finger_event.x = nx;
        finger_event.y = ny;
        finger_event.pressure = 1.0f;
        g_app_on_fingering(event_type, &finger_event);
        finger_sent = 1;
    }

    if (g_app_on_mouse_event &&
        (action == CONTROLS_ACTION_DOWN || action == CONTROLS_ACTION_UP)) {
        SdlMouseButtonEventBridge mouse_event = {0};
        mouse_event.type = (action == CONTROLS_ACTION_DOWN) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        mouse_event.which = 0;
        mouse_event.button = SDL_BUTTON_LEFT;
        mouse_event.state = (action == CONTROLS_ACTION_DOWN) ? SDL_PRESSED : SDL_RELEASED;
        mouse_event.clicks = 1;
        mouse_event.x = px;
        mouse_event.y = py;
        g_app_on_mouse_event(event_type, &mouse_event);
        mouse_sent = 1;
    }

    if (!mouse_sent && gamewindow_ready) {
        if (action == CONTROLS_ACTION_DOWN && g_gamewindow_mouse_down) {
            gw_rc = g_gamewindow_mouse_down(gamewindow, SDL_MOUSEBUTTONDOWN, SDL_BUTTON_LEFT, px, py);
            gw_sent = 1;
        } else if (action == CONTROLS_ACTION_UP && g_gamewindow_mouse_up) {
            gw_rc = g_gamewindow_mouse_up(gamewindow, SDL_MOUSEBUTTONUP, SDL_BUTTON_LEFT, px, py);
            gw_sent = 1;
        }
    }

    if (action == CONTROLS_ACTION_MOVE && gamewindow_ready && g_gamewindow_mouse_move) {
        gw_rc = g_gamewindow_mouse_move(gamewindow, SDL_MOUSEMOTION, 0, px, py);
        gw_sent = 1;
    }

    if (log_count < 64U) {
        l_info("INPUT engine action=%d slot=%d event=%d xy=%d,%d legacy=%d finger=%d mouse=%d gw=%d/%d gamewin=%p",
               action,
               compact_id,
               event_type,
               px,
               py,
               legacy_sent,
               finger_sent,
               mouse_sent,
               gw_sent,
               gw_rc,
               gamewindow);
        log_count++;
    }
    return finger_sent || mouse_sent || gw_sent;
}

static int controls_sdl_key_for_android_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:
            return SDLK_UP;
        case AKEYCODE_DPAD_DOWN:
            return SDLK_DOWN;
        case AKEYCODE_DPAD_LEFT:
            return SDLK_LEFT;
        case AKEYCODE_DPAD_RIGHT:
            return SDLK_RIGHT;
        case AKEYCODE_DPAD_CENTER:
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_BUTTON_START:
            return SDLK_RETURN;
        case AKEYCODE_BACK:
        case AKEYCODE_BUTTON_B:
        case AKEYCODE_BUTTON_SELECT:
            return SDLK_ESCAPE;
        case AKEYCODE_BUTTON_X:
        case AKEYCODE_BUTTON_Y:
            return SDLK_SPACE;
        default:
            return 0;
    }
}

static int controls_vita_platform_code_for_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:
            return MCSM_VITA_UP;
        case AKEYCODE_DPAD_DOWN:
            return MCSM_VITA_DOWN;
        case AKEYCODE_DPAD_LEFT:
            return MCSM_VITA_LEFT;
        case AKEYCODE_DPAD_RIGHT:
            return MCSM_VITA_RIGHT;
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_DPAD_CENTER:
            return MCSM_VITA_CROSS;
        case AKEYCODE_BUTTON_B:
        case AKEYCODE_BACK:
            return MCSM_VITA_CIRCLE;
        case AKEYCODE_BUTTON_X:
            return MCSM_VITA_SQUARE;
        case AKEYCODE_BUTTON_Y:
            return MCSM_VITA_TRIANGLE;
        case AKEYCODE_BUTTON_L1:
            return MCSM_VITA_L2;   /* Vita has one shoulder pair; MCSM binds L/R to console L2/R2 */
        case AKEYCODE_BUTTON_R1:
            return MCSM_VITA_R2;
        case AKEYCODE_BUTTON_START:
            return MCSM_VITA_START;
        case AKEYCODE_BUTTON_SELECT:
            return MCSM_VITA_SELECT;
        default:
            return 0;
    }
}

static int controls_generic_input_code_for_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:
            return MCSM_INPUT_DPAD_UP;
        case AKEYCODE_DPAD_DOWN:
            return MCSM_INPUT_DPAD_DOWN;
        case AKEYCODE_DPAD_LEFT:
            return MCSM_INPUT_DPAD_LEFT;
        case AKEYCODE_DPAD_RIGHT:
            return MCSM_INPUT_DPAD_RIGHT;
        case AKEYCODE_DPAD_CENTER:
        case AKEYCODE_BUTTON_A:
            return MCSM_INPUT_BUTTON_0;
        case AKEYCODE_BUTTON_B:
            return MCSM_INPUT_BUTTON_1;
        case AKEYCODE_BUTTON_X:
            return MCSM_INPUT_BUTTON_2;
        case AKEYCODE_BUTTON_Y:
            return MCSM_INPUT_BUTTON_3;
        case AKEYCODE_BUTTON_L1:
            return MCSM_INPUT_BUTTON_4;
        case AKEYCODE_BUTTON_R1:
            return MCSM_INPUT_BUTTON_5;
        case AKEYCODE_BACK:
        case AKEYCODE_BUTTON_SELECT:
            return MCSM_INPUT_BUTTON_BACK;
        case AKEYCODE_BUTTON_START:
            return MCSM_INPUT_BUTTON_START;
        default:
            return 0;
    }
}

static int controls_abstract_input_code_for_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_CENTER:
        case AKEYCODE_BUTTON_A:
            return MCSM_INPUT_ABSTRACT_CONFIRM;
        case AKEYCODE_BACK:
        case AKEYCODE_BUTTON_B:
            return MCSM_INPUT_ABSTRACT_CANCEL;
        default:
            return 0;
    }
}

static void controls_queue_engine_input(EngineInputQueueFn queue_fn,
                                        const char *route,
                                        int input_code,
                                        ControlsAction action,
                                        float x,
                                        float y) {
    static unsigned log_count = 0;

    if (!queue_fn || input_code == 0) {
        return;
    }

    void *null_mapper = NULL;
    queue_fn(input_code,
             controls_engine_event_type(action),
             x,
             y,
             NULL,
             -1,
             &null_mapper);

    if (log_count < 24U) {
        l_info("INPUT %s queue code=0x%04X action=%d xy=%.3f,%.3f",
               route ? route : "engine",
               input_code,
               action,
               x,
               y);
        log_count++;
    }
}

/* SAVE-RENAME KEYBOARD delivery (2026-07-18): feed the IME-entered name to the
 * engine as SDL key events (the same route physical keys use). Best-effort:
 * printable ASCII char -> SDL keysym (== ASCII for the basic Latin range). */
void mcsm_ime_deliver(const char *utf8) {
    if (!g_app_on_key_event || !utf8) return;
    for (const char *p = utf8; *p; ++p) {
        int sym = (unsigned char)*p;
        if (sym < 0x20 || sym > 0x7e) continue;   /* printable ASCII only */
        SdlKeyboardEventBridge ev = {0};
        ev.type = SDL_KEYDOWN; ev.state = SDL_PRESSED; ev.sym = sym;
        g_app_on_key_event(controls_engine_event_type(CONTROLS_ACTION_DOWN), &ev);
        ev.type = SDL_KEYUP; ev.state = SDL_RELEASED;
        g_app_on_key_event(controls_engine_event_type(CONTROLS_ACTION_UP), &ev);
    }
    l_info("KEYBOARD: delivered '%s' to engine", utf8);
}

static int controls_emit_engine_key(int32_t keycode, ControlsAction action) {
    static unsigned log_count = 0;
    const int sdl_key = controls_sdl_key_for_android_keycode(keycode);

    if (!g_app_on_key_event || sdl_key == 0) {
        return 0;
    }

    SdlKeyboardEventBridge event = {0};
    event.type = (action == CONTROLS_ACTION_UP) ? SDL_KEYUP : SDL_KEYDOWN;
    event.state = (action == CONTROLS_ACTION_UP) ? SDL_RELEASED : SDL_PRESSED;
    event.sym = sdl_key;

    g_app_on_key_event(controls_engine_event_type(action), &event);

    if (log_count < 16U) {
        l_info("INPUT engine-key code=%d sdl=0x%08X action=%d",
               keycode,
               (unsigned)sdl_key,
               action);
        log_count++;
    }
    return 1;
}

static int is_android_gamepad_button_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_BUTTON_B:
        case AKEYCODE_BUTTON_X:
        case AKEYCODE_BUTTON_Y:
        case AKEYCODE_BUTTON_L1:
        case AKEYCODE_BUTTON_R1:
        case AKEYCODE_BUTTON_START:
        case AKEYCODE_BUTTON_SELECT:
            return 1;
        default:
            return 0;
    }
}

static int is_android_sdl_pad_keycode(int32_t keycode) {
    switch (keycode) {
        case AKEYCODE_DPAD_UP:
        case AKEYCODE_DPAD_DOWN:
        case AKEYCODE_DPAD_LEFT:
        case AKEYCODE_DPAD_RIGHT:
        case AKEYCODE_DPAD_CENTER:
        case AKEYCODE_BACK:
            return 1;
        default:
            return is_android_gamepad_button_keycode(keycode);
    }
}

/* Register a normal SDL joystick so Telltale's SDL/GameController path sees
 * Vita as one pad with real buttons and stick axes. */
void mcsm_register_virtual_controller(void) {
    static int s_registered = 0;
    static int s_attempts = 0;
    if (s_registered) {
        return;
    }
    resolve_sdl_input_symbols();
    if (!g_sdl_add_joystick) {
        /* Re-resolve directly in case the symbol wasn't ready at first resolve. */
        g_sdl_add_joystick = (SdlAddJoystickFn)so_symbol(&so_mod_sdl2, "Java_org_libsdl_app_SDLActivity_nativeAddJoystick");
    }
    if (s_attempts < 4) {
        l_info("INPUT: register_virtual_controller attempt=%d add_joystick=%p", s_attempts, (void *)g_sdl_add_joystick);
        s_attempts++;
    }
    if (!g_sdl_add_joystick) {
        return;
    }
    s_registered = 1;
    jobject name = jni->NewStringUTF(&jni, "PS Vita Controller");
    jobject desc = jni->NewStringUTF(&jni, "0300000000000000564954410000fefe");
    /* CONTROLLER FIX (2026-07-17): register a GameController mapping for this GUID
     * BEFORE adding the joystick, so SDL_GameControllerOpen succeeds and the engine
     * reads L2/R2 as trigger AXES (the ONLY input the fight ButtonMash QTE samples).
     * Without a mapping SDL opens it as a raw joystick and NEVER retries as a
     * controller -> triggers permanently dead. Button indices are decoded from SDL's
     * Android keycode->button table in libSDL2.so, GUID byte-matches `desc` above. */
    if (!g_sdl_add_mapping)
        g_sdl_add_mapping = (SdlAddMappingFn)so_symbol(&so_mod_sdl2, "SDL_GameControllerAddMapping");
    if (g_sdl_add_mapping) {
        int mrc = g_sdl_add_mapping(
            "0300000000000000564954410000fefe,PS Vita Controller,platform:Android,"
            "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
            "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
            "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,");
        l_info("INPUT: SDL_GameControllerAddMapping(Vita) -> %d", mrc);
    }
    jint rc = g_sdl_add_joystick(&jni, NULL, SDL_JOYSTICK_ID_VITA, name, desc,
                                 JNI_FALSE,
                                 SDL_JOYSTICK_BUTTONS_VITA,
                                 SDL_JOYSTICK_AXES_VITA,
                                 SDL_JOYSTICK_HATS_VITA,
                                 0 /*nballs*/);
    l_info("INPUT: nativeAddJoystick(virtual Vita controller buttons=%d axes=%d hats=%d device=%d) -> %d",
           SDL_JOYSTICK_BUTTONS_VITA,
           SDL_JOYSTICK_AXES_VITA,
           SDL_JOYSTICK_HATS_VITA,
           SDL_JOYSTICK_ID_VITA,
           (int)rc);
}

static int controls_emit_sdl_pad_button(int32_t keycode, ControlsAction action) {
    static unsigned log_count = 0;

    if ((action != CONTROLS_ACTION_DOWN && action != CONTROLS_ACTION_UP) ||
        !is_android_sdl_pad_keycode(keycode)) {
        return 0;
    }

    /* L1/R1 FIX (2026-07-18): previously L/R were suppressed here (return 0) and
     * ONLY drove the trigger axes a4/a5, so they never registered as the shoulder
     * BUTTONS the game expects (leftshoulder b9 / rightshoulder b10). Now that L2/R2
     * is carried by the SDL trigger AXES (controls_emit_sdl_trigger), emitting the
     * shoulder button here no longer suppresses anything — so let L/R fall through
     * and fire b9/b10 as normal, WHILE the caller still drives the a4/a5 axes for
     * the QTE. Result: L1/R1 register as buttons AND work as L2/R2 triggers. */

    if (!g_sdl_pad_down && !g_sdl_pad_up) {
        return 0;
    }

    int sent = 0;
    if (action == CONTROLS_ACTION_DOWN && g_sdl_pad_down) {
        g_sdl_pad_down(&jni, NULL, SDL_JOYSTICK_ID_VITA, keycode);
        sent = 1;
    } else if (action == CONTROLS_ACTION_UP && g_sdl_pad_up) {
        g_sdl_pad_up(&jni, NULL, SDL_JOYSTICK_ID_VITA, keycode);
        sent = 1;
    }

    if (sent && log_count < 16U) {
        l_info("INPUT sdl-pad code=%d action=%d device=%d",
               keycode,
               action,
               SDL_JOYSTICK_ID_VITA);
        log_count++;
    }
    return sent;
}

/* CONTROLLER FIX (2026-07-17): drive Vita L/R as SDL trigger AXES a4/a5
 * (SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT). The fight ButtonMash QTE reads L2/R2
 * ONLY via SDL_GameControllerGetAxis, so the shoulders must MOVE these axes — the
 * native platform-mapper route is never sampled by that QTE. SDL's Android_OnJoy
 * scales the float by 32767, so rest 0.0->0, press 1.0->32767 (trigger convention). */
static int controls_emit_sdl_trigger(int32_t keycode, ControlsAction action) {
    if (!g_sdl_joy) return 0;
    if (action != CONTROLS_ACTION_DOWN && action != CONTROLS_ACTION_UP) return 0;
    const int axis = (keycode == AKEYCODE_BUTTON_L1) ? 4 : 5;
    const float v = (action == CONTROLS_ACTION_DOWN) ? 1.0f : 0.0f;
    g_sdl_joy(&jni, NULL, SDL_JOYSTICK_ID_VITA, axis, v);
    return 1;
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    static unsigned log_count = 0;
    static unsigned gated_log_count = 0;
    if (!input_delivery_ready()) {
        if (gated_log_count < 8U) {
            l_info("INPUT key gated before input-ready code=%d action=%d scene=%d frames=%u",
                   keycode,
                   action,
                   launch_state_scene_active(),
                   launch_state_get_present_count());
            gated_log_count++;
        }
        return;
    }
    resolve_sdl_input_symbols();
    /* With native-engine-only input this is intentionally a logged no-op. */
    mcsm_register_virtual_controller();

    /* SAVE-RENAME KEYBOARD (2026-07-18): MCSM never requests a soft keyboard
     * through any path we can hook — Lua, SDL, and JNI (regular + static) were
     * all traced and stayed empty. So repurpose the otherwise-redundant SELECT
     * button (Circle/O already maps to ESCAPE for back/cancel) to raise the Vita
     * IME on demand. Press SELECT on the rename screen, type, hit Enter; the
     * result is fed to the engine from gl_swap via mcsm_ime_deliver(). */
    if (keycode == AKEYCODE_BUTTON_SELECT) {
        extern void mcsm_ime_begin(const char *);
        extern int mcsm_ime_is_active(void);
        if (action == CONTROLS_ACTION_DOWN && !mcsm_ime_is_active()) {
            l_info("KEYBOARD: SELECT -> raising Vita IME");
            mcsm_ime_begin("");
        }
        return;   /* consume SELECT entirely; Circle handles back/cancel */
    }

    const int vita_platform_code = controls_vita_platform_code_for_keycode(keycode);
    const int generic_input_code = controls_generic_input_code_for_keycode(keycode);
    const int abstract_input_code = controls_abstract_input_code_for_keycode(keycode);
    const int sdl_pad_candidate = is_android_sdl_pad_keycode(keycode);
    int engine_key_sent = 0;
    int sdl_pad_sent = 0;
    /* The Vita has ONE shoulder pair -> dedicate physical L1/R1 to console L2/R2.
     * The fight ButtonMash QTE reads L2/R2 ONLY as SDL trigger axes, so route the
     * shoulders to the trigger axes (controls_emit_sdl_trigger) — NOT the platform
     * mapper queue (which that QTE never samples) and NOT the generic route (below,
     * suppressed) which would inject a spurious second InputMapper event. */
    const int is_shoulder_remap =
        (keycode == AKEYCODE_BUTTON_L1 || keycode == AKEYCODE_BUTTON_R1);

    sdl_pad_sent = controls_emit_sdl_pad_button(keycode, action);

    if (is_shoulder_remap) {
        controls_emit_sdl_trigger(keycode, action);
    }
    /* SDL is the primary controller route. Queue Telltale's native mapper only
     * if SDL could not take the pad event; double-delivery makes menu focus move
     * multiple times per physical press. */
    else if (!sdl_pad_sent && vita_platform_code) {
        controls_queue_engine_input(g_platform_input_queue,
                                    "platform",
                                    vita_platform_code,
                                    action,
                                    0.0f,
                                    0.0f);
    }
    if (!sdl_pad_sent && !is_shoulder_remap && generic_input_code) {
        controls_queue_engine_input(g_input_mapper_queue,
                                    "generic-key",
                                    generic_input_code,
                                    action,
                                    0.0f,
                                    0.0f);
    }
    if (!sdl_pad_sent && abstract_input_code) {
        controls_queue_engine_input(g_input_mapper_queue,
                                    "abstract-key",
                                    abstract_input_code,
                                    action,
                                    0.0f,
                                    0.0f);
    }

    /* Keyboard fallback only for non-pad Android keys. Vita pad input uses the
     * native Telltale queues plus SDL's Android pad path. */
    if (!sdl_pad_candidate) {
        engine_key_sent = controls_emit_engine_key(keycode, action);
    }

    if (log_count < 16U) {
        l_info("INPUT key code=%d action=%d platform=0x%04X generic=0x%04X abstract=0x%04X sdl_pad=%d engine_key=%d route=%s",
               keycode,
               action,
               vita_platform_code,
               generic_input_code,
               abstract_input_code,
               sdl_pad_sent,
               engine_key_sent,
               "engine");
        log_count++;
    }
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    static unsigned log_count = 0;
    static unsigned gated_log_count = 0;
    if (!input_delivery_ready()) {
        if (gated_log_count < 8U) {
            l_info("INPUT touch gated before input-ready id=%d action=%d xy=%.1f,%.1f scene=%d frames=%u",
                   id,
                   action,
                   x,
                   y,
                   launch_state_scene_active(),
                   launch_state_get_present_count());
            gated_log_count++;
        }
        return;
    }
    resolve_sdl_input_symbols();

    int compact_id = active_touch_find(id);
    const int android_action = android_motion_action_for_touch(id, action);
    if (compact_id < 0) {
        compact_id = active_touch_find(id);
    }
    if (compact_id < 0 || compact_id >= MAX_ACTIVE_TOUCH_IDS) {
        compact_id = 0;
    }

    const float nx = clamp01(x / VITA_TOUCH_W);
    const float ny = clamp01(y / VITA_TOUCH_H);
    int native_touch_sent = 0;
    int direct_sdl_sent = 0;
    int direct_engine_sent = 0;

    /* The engine-direct path (controls_emit_engine_touch) delivers correct
     * coordinates to Application_SDL::OnFingering, OnMouseEvent,
     * GameWindow_PlayMode, and TouchScreenState. The SDL-based paths below
     * (native onNativeTouch, SDL_SendTouch, SDL_SendMouseButton) cause the
     * engine's internal SDL event loop to generate duplicate
     * GameWindow_PlayMode::OnMouseDown calls with cursor=0,arg=0 (coordinates
     * lost in the SDL pipeline), which breaks touch interaction. Disable them
     * and rely solely on the engine-direct path. */
    (void)android_action;
    direct_engine_sent = controls_emit_engine_touch(compact_id, x, y, nx, ny, action);

    if (log_count < 32U) {
        l_info("INPUT touch id=%d slot=%d action=%d android=%d active=%u native=%d direct=%d/%d xy=%.1f,%.1f norm=%.3f,%.3f",
               id,
               compact_id,
               action,
               android_action,
               g_active_touch_count,
               native_touch_sent,
               direct_sdl_sent,
               direct_engine_sent,
               x,
               y,
               nx,
               ny);
        log_count++;
    }
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    static unsigned log_count = 0;
    if (!input_delivery_ready()) {
        return;
    }
    resolve_sdl_input_symbols();
    mcsm_register_virtual_controller();

    const int input_code =
        (which == CONTROLS_STICK_RIGHT) ? MCSM_INPUT_RIGHT_STICK_MOVE : MCSM_INPUT_LEFT_STICK_MOVE;
    const int axis_x = (which == CONTROLS_STICK_RIGHT) ? 2 : 0;
    const int axis_y = (which == CONTROLS_STICK_RIGHT) ? 3 : 1;
    int sdljoy_sent = 0;
    int native_engine_sent = 0;

    if (g_sdl_joy) {
        g_sdl_joy(&jni, NULL, SDL_JOYSTICK_ID_VITA, axis_x, x);
        g_sdl_joy(&jni, NULL, SDL_JOYSTICK_ID_VITA, axis_y, y);
        sdljoy_sent = 1;
    }

    if (!sdljoy_sent) {
        controls_queue_engine_input(g_input_mapper_queue,
                                    (which == CONTROLS_STICK_RIGHT) ? "right-stick" : "left-stick",
                                    input_code,
                                    action,
                                    x,
                                    y);
        native_engine_sent = 1;
    }

    if (log_count < 16U) {
        l_info("INPUT analog stick=%d action=%d code=0x%04X xy=%.3f,%.3f sdljoy=%d native_engine=%d",
               which,
               action,
               input_code,
               x,
               y,
               sdljoy_sent,
               native_engine_sent);
        log_count++;
    }
}
