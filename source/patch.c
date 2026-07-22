/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge/kubridge.h>
#include <so_util/so_util.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>      /* scePowerSetArmClockFrequency for the adaptive clock governor */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "reimpl/mem.h"
#include "utils/glutil.h"
#include "utils/launch_state.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/config.h"

/* Live loading-screen hooks: track the current asset + a boot/load timer.
 * In PVR mode, redraws are opportunistic: loading_screen_tick() renders only
 * when the single EGL context is already current here or is not owned by the
 * engine render thread. */
#include "utils/loading_screen.h"
#ifdef USE_PVR_PSP2
#define LS_SET_ASSET(n) loading_screen_set_asset(n)
#define LS_TICK()       loading_screen_tick()
#define LS_DONE()       loading_screen_mark_loaded()
#else
#define LS_SET_ASSET(n) ((void)0)
#define LS_TICK()       ((void)0)
#define LS_DONE()       ((void)0)
/* Set while a scene is loading so glutil's texture-upload path animates the
 * loading screen instead of letting the sim freeze appear "stuck". Defined in
 * glutil.c (vitaGL build). */
extern int g_scene_loading;
#endif

extern so_module so_mod_gameengine;
extern so_module so_mod_fmod;
extern so_module so_mod_fmodstudio;
extern so_module so_mod_sdl2;
extern void mcsm_register_virtual_controller(void);

#define ENABLE_UNSAFE_ARCHIVE_DIAG_HOOKS 0
#define ENABLE_HOT_RENDER_VIEW_DIAG_HOOKS 0

#define SO_CONTINUE_VOID(h, ...) do { \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).orig_instr, sizeof((h).orig_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).orig_instr)); \
    if ((h).thumb_addr) { \
        ((void(*)())(h).thumb_addr)(__VA_ARGS__); \
    } else { \
        ((void(*)())(h).addr)(__VA_ARGS__); \
    } \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).patch_instr, sizeof((h).patch_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).patch_instr)); \
} while (0)

#define INLINE_HOOK_BYTES (sizeof(uint32_t) * 2U)

static so_hook g_hook_gameengine_start;
static so_hook g_hook_gameengine_loop;
static so_hook g_hook_gameengine_render;
static so_hook g_hook_metrics_new_frame;
static so_hook g_hook_render_begin_frame;
static so_hook g_hook_render_end_frame;
static so_hook g_hook_render_present;
static so_hook g_hook_app_swap;
static so_hook g_hook_suspend_game_loop;
static so_hook g_hook_renderthread_submit_current_frame;
static so_hook g_hook_renderthread_end_frame;
static so_hook g_hook_renderthread_finish_frame;
static so_hook g_hook_renderframe_execute;
static so_hook g_hook_renderframe_allocate_view;
static so_hook g_hook_renderframe_push_view;
static so_hook g_hook_viewport_prepare_view;
static so_hook g_hook_rendertexture_prepare_view;
static so_hook g_hook_switch_default_render_target;
static so_hook g_hook_gamerender_render_frame;
static so_hook g_hook_gamerender_render_scene;
static so_hook g_hook_playback_controller_update;
static so_hook g_hook_android_pump_events;
static so_hook g_hook_android_jni_poll_input_devices;
static so_hook g_hook_sdl_wait_event_real;
static so_hook g_hook_sdl_wait_event_timeout_real;
static so_hook g_hook_app_on_fingering;
static so_hook g_hook_app_on_mouse_event;
static so_hook g_hook_gamewindow_mouse_move;
static so_hook g_hook_gamewindow_mouse_down;
static so_hook g_hook_gamewindow_mouse_up;
static so_hook g_hook_touch_set_legacy_pointer;
static so_hook g_hook_fmod_studio_initialize;

static int mcsm_mega_diag_enabled(void);

static uintptr_t g_renderframe_push_view_addr = 0;
static size_t g_renderframe_push_view_size = 0;
static uintptr_t g_renderoverlay_update_render_thread_addr = 0;
static size_t g_renderoverlay_update_render_thread_size = 0;
typedef void (*engine_bool_setter_fn)(int);
static engine_bool_setter_fn g_set_chore_filter_includes_non_skeleton = NULL;
static engine_bool_setter_fn g_set_fix_recursive_animation_contribution = NULL;
static uint8_t *g_fix_recursive_animation_contribution = NULL;
static int g_animation_flag_symbols_resolved = 0;
/* Set once the first real scene renders = boot/loading is done. While this is
 * 0 we yield CPU in the render-thread overlay loop (below) so the worker
 * threads doing async resource loading aren't starved by the render thread
 * spinning at ~490fps (it presents but vsync is off, so it never blocks). */
static volatile int g_boot_scene_active = 0;
static volatile uintptr_t g_overlay_render_frame = 0;

enum {
    FMOD_OUTPUTTYPE_AUTODETECT = 0,
    FMOD_OUTPUTTYPE_UNKNOWN = 1,
    FMOD_OUTPUTTYPE_NOSOUND = 2,
    FMOD_OUTPUTTYPE_AUDIOTRACK = 15,
    FMOD_OUTPUTTYPE_OPENSL = 16,
    FMOD_OUTPUTTYPE_AUDIOOUT = 18,
};

typedef int (*fmod_studio_get_low_level_system_fn)(void *studio_system, void **low_level_system);
typedef int (*fmod_system_set_output_fn)(void *system, int output);
typedef int (*fmod_system_get_output_fn)(void *system, int *output);

static fmod_studio_get_low_level_system_fn g_fmod_studio_get_low_level_system = NULL;
static fmod_system_set_output_fn g_fmod_system_set_output = NULL;
static fmod_system_get_output_fn g_fmod_system_get_output = NULL;

static const char *fmod_output_name(int output) {
    switch (output) {
        case FMOD_OUTPUTTYPE_AUTODETECT: return "AUTODETECT";
        case FMOD_OUTPUTTYPE_UNKNOWN: return "UNKNOWN";
        case FMOD_OUTPUTTYPE_NOSOUND: return "NOSOUND";
        case FMOD_OUTPUTTYPE_AUDIOTRACK: return "AUDIOTRACK";
        case FMOD_OUTPUTTYPE_OPENSL: return "OPENSL";
        case FMOD_OUTPUTTYPE_AUDIOOUT: return "AUDIOOUT";
        default: return "UNRECOGNIZED";
    }
}

static const char *fmod_result_name(int result) {
    switch (result) {
        case 0: return "FMOD_OK";
        case 41: return "FMOD_ERR_OUTPUT_INIT";
        case 54: return "FMOD_ERR_INVALID_PARAM";
        case 70: return "FMOD_ERR_UNSUPPORTED";
        default: return "FMOD_RESULT_UNKNOWN";
    }
}

static int fmod_choose_forced_output(void) {
    char path[256];
    snprintf(path, sizeof(path), DATA_PATH "fmod_output.txt");
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fp = fopen("ux0:data/mcsm/fmod_output.txt", "r");
    }
    if (!fp) {
        return FMOD_OUTPUTTYPE_OPENSL;
    }

    char buf[64];
    char *line = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    if (!line) {
        return FMOD_OUTPUTTYPE_OPENSL;
    }
    if (strstr(line, "audiotrack") || strstr(line, "AUDIOTRACK") ||
        strstr(line, "AudioTrack")) {
        return FMOD_OUTPUTTYPE_AUDIOTRACK;
    }
    if (strstr(line, "autodetect") || strstr(line, "AUTODETECT")) {
        return FMOD_OUTPUTTYPE_AUTODETECT;
    }
    if (strstr(line, "nosound") || strstr(line, "NOSOUND")) {
        return FMOD_OUTPUTTYPE_NOSOUND;
    }
    return FMOD_OUTPUTTYPE_OPENSL;
}

static void resolve_fmod_audio_symbols(void) {
    if (!g_fmod_studio_get_low_level_system) {
        g_fmod_studio_get_low_level_system =
            (fmod_studio_get_low_level_system_fn)so_symbol(&so_mod_fmodstudio,
                                                           "FMOD_Studio_System_GetLowLevelSystem");
    }
    if (!g_fmod_system_set_output) {
        g_fmod_system_set_output =
            (fmod_system_set_output_fn)so_symbol(&so_mod_fmod, "FMOD_System_SetOutput");
    }
    if (!g_fmod_system_get_output) {
        g_fmod_system_get_output =
            (fmod_system_get_output_fn)so_symbol(&so_mod_fmod, "FMOD_System_GetOutput");
    }
}

static int hook_symbol_checked(so_module *mod,
                               const char *symbol,
                               const char *label,
                               uintptr_t dst,
                               so_hook *out) {
    uintptr_t fn = so_symbol(mod, symbol);
    if (!fn) {
        l_warn("Patch: %s symbol not found.", label);
        return 0;
    }

    size_t symbol_size = so_symbol_size(mod, symbol);
    if (symbol_size != 0 && symbol_size < INLINE_HOOK_BYTES) {
        l_warn("Patch: skipping hook for %s at %p; function too small (%u bytes).",
               label,
               (void *)fn,
               (unsigned)symbol_size);
        return 0;
    }

    *out = hook_addr(fn, dst);
    l_info("Patch: hooked %s at %p.", label, (void *)fn);
    return 1;
}

static int hook_fmod_studio_initialize(void *studio_system,
                                       int max_channels,
                                       unsigned int studio_flags,
                                       unsigned int low_level_flags,
                                       void *extra_driver_data) {
    resolve_fmod_audio_symbols();

    void *low_level = NULL;
    int output_before = -1;
    int output_after = -1;
    int get_low_result = -1;
    int set_output_result = -1;
    const int requested_output = fmod_choose_forced_output();

    if (g_fmod_studio_get_low_level_system && g_fmod_system_set_output) {
        get_low_result = g_fmod_studio_get_low_level_system(studio_system, &low_level);
        if (get_low_result == 0 && low_level) {
            if (g_fmod_system_get_output) {
                (void)g_fmod_system_get_output(low_level, &output_before);
            }
            if (requested_output != FMOD_OUTPUTTYPE_AUTODETECT) {
                set_output_result = g_fmod_system_set_output(low_level, requested_output);
            } else {
                set_output_result = 0;
            }
            if (g_fmod_system_get_output) {
                (void)g_fmod_system_get_output(low_level, &output_after);
            }
        }
    }

    if (mcsm_mega_diag_enabled()) {
        l_info("AUDIODIAG: FMOD Studio::initialize pre studio=%p low=%p getLow=%d(%s) setOutput=%d(%s) "
               "want=%d(%s) before=%d(%s) after=%d(%s) max=%d studioFlags=0x%08X lowFlags=0x%08X extra=%p",
               studio_system,
               low_level,
               get_low_result,
               fmod_result_name(get_low_result),
               set_output_result,
               fmod_result_name(set_output_result),
               requested_output,
               fmod_output_name(requested_output),
               output_before,
               fmod_output_name(output_before),
               output_after,
               fmod_output_name(output_after),
               max_channels,
               studio_flags,
               low_level_flags,
               extra_driver_data);
    }

    const int result = SO_CONTINUE(int,
                                   g_hook_fmod_studio_initialize,
                                   studio_system,
                                   max_channels,
                                   studio_flags,
                                   low_level_flags,
                                   extra_driver_data);

    int final_output = -1;
    if (g_fmod_system_get_output && low_level) {
        (void)g_fmod_system_get_output(low_level, &final_output);
    }
    if (mcsm_mega_diag_enabled()) {
        l_info("AUDIODIAG: FMOD Studio::initialize result=%d(%s) finalOutput=%d(%s)",
               result,
               fmod_result_name(result),
               final_output,
               fmod_output_name(final_output));
    }
    return result;
}

static void patch_fmod_audio_hooks(void) {
    resolve_fmod_audio_symbols();
    if (mcsm_mega_diag_enabled()) {
        l_info("AUDIODIAG: FMOD symbols getLow=%p setOutput=%p getOutput=%p",
               (void *)g_fmod_studio_get_low_level_system,
               (void *)g_fmod_system_set_output,
               (void *)g_fmod_system_get_output);
    }
    (void)hook_symbol_checked(&so_mod_fmodstudio,
                              "_ZN4FMOD6Studio6System10initializeEijjPv",
                              "FMOD::Studio::System::initialize",
                              (uintptr_t)&hook_fmod_studio_initialize,
                              &g_hook_fmod_studio_initialize);
}

static int patch_arm32_instruction(const char *label,
                                   uintptr_t addr,
                                   uint32_t expected,
                                   uint32_t replacement) {
    if (!addr) {
        l_warn("Patch: %s instruction address is null.", label);
        return 0;
    }

    const uint32_t current = *(volatile uint32_t *)(void *)addr;
    if (current == replacement) {
        l_info("Patch: %s already patched at %p (%08X).",
               label,
               (void *)addr,
               current);
        return 1;
    }
    if (current != expected) {
        l_warn("Patch: %s at %p has unexpected instruction %08X; expected %08X, skip.",
               label,
               (void *)addr,
               current,
               expected);
        return 0;
    }

    kuKernelCpuUnrestrictedMemcpy((void *)addr, &replacement, sizeof(replacement));
    kuKernelFlushCaches((void *)addr, sizeof(replacement));
    l_info("Patch: %s at %p %08X->%08X.",
           label,
           (void *)addr,
           expected,
           replacement);
    return 1;
}

static uint64_t now_ms(void) {
    return sceKernelGetSystemTimeWide() / 1000ULL;
}

/* FRAME PACING (2026-06-23): the animation judder isn't low fps (avg ~50-60 in
 * menus) -- it's VARIANCE. Plain vsync makes the per-frame delta beat 17ms<->34ms
 * (60<->30) because the GPU keeps missing vblank by a hair, so the engine advances
 * chores/animation by an uneven amount each frame = stutter. Pacing the game loop
 * to a fixed period makes the delta the engine sees constant -> smooth motion.
 * Default 30fps (the cadence the original console builds used). Tunable at runtime
 * with no rebuild via ux0:data/mcsm/fps_cap.txt (an integer fps; <=0 = uncapped).
 * This must honor the user's configured cap directly; forcing 60 down to 30
 * made character/menu animation visibly regress in heavy scenes. */
static uint32_t mcsm_frame_pace_us(void) {
    static int initialized = 0;
    /* Default 30 fps, vblank-quantized (2*16667 - 2500 = 30834) — identical to what
     * the fps_cap.txt=30 path below computes. A plain 33333 lands the sleep-release
     * exactly ON the 2-vblank boundary, so adaptive vsync misses it by a hair and
     * snaps to the next vblank -> idle frames beat to 20fps. Undershooting by 2500us
     * makes the frame ready just before vblank so vsync catches the intended one. */
    static uint32_t pace_us = 30834; /* 30 fps (vblank-quantized) */
    static int requested_fps = 30;
    if (!initialized) {
        initialized = 1;
        int fps = mcsm_cfg()->fps_cap;
        requested_fps = fps;
        if (fps <= 0 || fps > 120) {
            pace_us = 0; /* uncapped */
        } else {
            /* Same vblank-quantized undershoot as the present-lock (glutil.c
             * gl_init) so the loop pace and present lock share one phase. */
            const int vb = 16667;
            int per = 1000000 / fps;
            int k = (per + vb / 2) / vb;
            if (k < 1) k = 1;
            pace_us = (uint32_t)(k * vb - 2500);
        }
        l_info("Frame pace: %u us (%s requested=%d)",
               pace_us,
               pace_us ? "capped" : "uncapped",
               requested_fps);
    }
    return pace_us;
}

static void mcsm_pace_frame(void) {
    const uint32_t target = mcsm_frame_pace_us();
    if (!target) {
        return;
    }
    static uint64_t last_us = 0;
    uint64_t now = sceKernelGetSystemTimeWide();
    if (last_us != 0) {
        const uint64_t elapsed = now - last_us;
        if (elapsed < target) {
            sceKernelDelayThread((SceUInt)(target - elapsed));
            now = sceKernelGetSystemTimeWide();
        }
    }
    last_us = now;
}

/* ADAPTIVE ARM-CLOCK GOVERNOR (2026-07-20, battery) ------------------------
 * The game is CPU/draw-submit bound, but only in HEAVY scenes; menus, dialogue
 * and most gameplay finish the engine sim loop far under the frame budget.
 * Holding the ARM at a flat 444MHz there just burns battery. This scales the
 * ARM clock to the ACTUAL per-frame sim work (sim_us, already measured in
 * hook_gameengine_loop — pure engine work, excludes our frame pace and the
 * render thread):
 *   - ESCALATE straight to the ceiling the instant one frame nears the budget,
 *     so a starved clock is NEVER sustained -> performance can't be worse than
 *     static-max by more than a 1-2 frame transient (and the game already has
 *     multi-second scene-load freezes, so that transient is noise), and
 *   - STEP DOWN one notch only after sustained headroom (~2s of light frames),
 *     rate-limited so the clock changes at most ~once/2s (syscall cost nil).
 * Provably non-oscillating: down-threshold 0.55*budget and up-threshold
 * 0.80*budget leave a 1.45x gap, wider than any single step's work-inflation
 * ratio (444/333=1.33, 333/266=1.25) — so a step-down can't immediately trip
 * the step-up. Budget follows the configured pace (fps_cap.txt), default 30.
 * RENDER-BOUND GUARD: sim_us can't see the render thread's ~900-draw submission
 * cost, so the step-down is ALSO gated on the TRUE present cadence
 * (g_mcsm_present_dt_us from gl_swap) — it never downclocks a scene whose frame
 * is already dropping, and escalates when it is. Biased to the ceiling (safe for
 * the stable-30 goal; a wrong present read only costs battery, never fps).
 * ONE-DOC config + opt-out: settings/clock.txt — "off" pins the boot clock;
 * "min"/"max" set the ARM floor/ceiling (defaults 266/444), "gpu" the GPU clock.
 * Both this governor and the boot clock (init.c) read it via mcsm_read_clock_cfg(). */
static void mcsm_clock_governor_tick(uint32_t sim_us) {
    static int inited = 0;
    static int enabled = 0;
    static int levels[8];
    static int n_levels = 0;
    static int cur_idx = 0;
    static uint32_t up_us = 24000, down_us = 17000, budget_us = 33333;
    static int low_streak = 0;
    static int frames_since_change = 0;

    if (!inited) {
        inited = 1;
        McsmClockCfg cfg;
        mcsm_read_clock_cfg(&cfg);        /* the ONE doc: clock.txt (off / min / max / gpu) */
        if (cfg.governor_off) {
            l_info("clock-gov: DISABLED (clock.txt 'off') — ARM pinned to boot clock");
            return; /* enabled stays 0 */
        }
        int ceiling = cfg.arm_max;
        int floor_mhz = cfg.arm_min;
        if (floor_mhz > ceiling) floor_mhz = ceiling;
        /* achievable stock ARM steps (ascending); keep those in [floor, ceiling] */
        static const int steps[] = { 111, 166, 222, 266, 333, 444, 500 };
        for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
            if (steps[i] >= floor_mhz && steps[i] <= ceiling && n_levels < (int)(sizeof(levels)/sizeof(levels[0])))
                levels[n_levels++] = steps[i];
        }
        if (n_levels == 0) levels[n_levels++] = ceiling;
        cur_idx = n_levels - 1;           /* start at the ceiling — init.c already booted there */
        budget_us = mcsm_frame_pace_us();
        if (!budget_us) budget_us = 33333; /* uncapped -> target 30fps power budget */
        up_us   = (uint32_t)((uint64_t)budget_us * 80 / 100);
        down_us = (uint32_t)((uint64_t)budget_us * 55 / 100);
        enabled = (n_levels > 1);         /* nothing to govern with a single level */
        l_info("clock-gov: %s levels=%d [%d..%dMHz] budget=%uus up=%uus down=%uus (clock.txt)",
               enabled ? "ON" : "single-level(off)", n_levels,
               levels[0], levels[n_levels - 1], budget_us, up_us, down_us);
    }
    if (!enabled) return;

    if (frames_since_change < 1000000) frames_since_change++;

    /* RENDER-BOUND GUARD (2026-07-20): sim_us is blind to the ~900-draw/frame
     * submission cost carried by the render/present thread. Read the true present
     * cadence (glutil.c gl_swap) so the governor NEVER downclocks a scene that is
     * already dropping frames (render/draw-bound), and escalates when it is. Biased
     * toward the ceiling: if the present signal looks bad we hold high (safe for the
     * stable-30 goal) — the only cost of a wrong read is less battery. */
    extern volatile uint32_t g_mcsm_present_dt_us;
    uint32_t pdt = g_mcsm_present_dt_us;
    int render_slow  = (pdt > budget_us + budget_us / 4);              /* >1.25x budget = clearly dropping */
    int frame_smooth = (pdt != 0) && (pdt <= budget_us + budget_us / 8); /* <=~1.12x budget = holding target */

    /* Diagnostic heartbeat (logging builds only; compiled out in production):
     * confirms the governor is alive and shows the clock it settled on. ~1/256 frames. */
    { static uint32_t hb = 0;
      if (((++hb) & 0xFFu) == 0u)
          l_info("clock-gov: heartbeat ARM=%dMHz sim=%uus present=%uus (down<%u up>%u)",
                 levels[cur_idx], sim_us, pdt, down_us, up_us); }

    /* ESCALATE immediately on CPU pressure OR a render-bound frame. */
    if (sim_us > up_us || render_slow) {
        low_streak = 0;
        if (cur_idx < n_levels - 1) {
            cur_idx = n_levels - 1;       /* jump straight to ceiling for fast recovery */
            scePowerSetArmClockFrequency(levels[cur_idx]);
            frames_since_change = 0;
            l_info("clock-gov: %s -> ARM up to %dMHz",
                   render_slow ? "render dropping" : "sim>up", levels[cur_idx]);
        }
        return;
    }

    /* STEP DOWN one notch only when sim is light AND the frame is comfortably
     * holding the target (never downclock a barely-holding scene). Rate-limited. */
    if (sim_us < down_us && frame_smooth) {
        if (++low_streak >= 60 && frames_since_change >= 60 && cur_idx > 0) {
            cur_idx--;
            scePowerSetArmClockFrequency(levels[cur_idx]);
            frames_since_change = 0;
            low_streak = 0;
            l_info("clock-gov: sustained light+smooth -> ARM down to %dMHz", levels[cur_idx]);
        }
    } else {
        low_streak = 0;                   /* pressure, roughness, or not-yet-measured -> hold */
    }
}

static int mcsm_mega_diag_enabled(void) {
    return 0; /* verbose diagnostics removed — off by default */
}

static int should_log_diag_count(uint32_t count) {
    return mcsm_mega_diag_enabled() && (count <= 8 || (count % 256U) == 0U);
}

static void log_diag_counter(const char *name, uint32_t count, uint64_t first_ms, const char *extra) {
    if (!mcsm_mega_diag_enabled()) {
        return;
    }
    const uint64_t elapsed = first_ms ? (now_ms() - first_ms) : 0;
    l_info("Diag: %s count=%u elapsed=%llums tid=0x%X%s%s",
           name,
           count,
           (unsigned long long)elapsed,
           sceKernelGetThreadId(),
           extra ? " " : "",
           extra ? extra : "");
}

typedef struct MetricsDiagState {
    float *frame_time;
    float *actual_frame_time;
    float *average_frame_time;
    float *total_time;
    float *scale;
    float *next_frame_time;
    float *fixed_time_step;
    float *delay;
    float *min_frame_time;
    uint32_t *frame_num;
    uint64_t *frame_stamp;
    uint8_t *reset;
    uint8_t *use_time_get_time;
    int initialized;
} MetricsDiagState;

static MetricsDiagState g_metrics_diag;

typedef struct RenderGateDiagState {
    uint8_t *app_wait_for_messages;
    uint8_t *app_active;
    uint8_t *game_suspend_loop;
    uint8_t *game_post_update_script_call;
    uint8_t *game_browser_during_shutdown;
    uint8_t *game_requested_quit;
    uint8_t *game_skip_platform_controller_screen;
    uint8_t *render_enable;
    uint8_t *render_device_initialized;
    uint8_t *render_in_frame;
    uint32_t *render_hwnd;
    uint32_t *device_width;
    uint32_t *device_height;
    uint32_t *game_width;
    uint32_t *game_height;
    int initialized;
} RenderGateDiagState;

static RenderGateDiagState g_render_gate_diag;

#define VITA_NATIVE_RENDER_WIDTH  960U
#define VITA_NATIVE_RENDER_HEIGHT 544U

static uint32_t float_bits(float value) {
    union {
        float f;
        uint32_t u;
    } cvt;
    cvt.f = value;
    return cvt.u;
}

static float float_from_bits(uint32_t value) {
    union {
        float f;
        uint32_t u;
    } cvt;
    cvt.u = value;
    return cvt.f;
}

static int float_bits_finite(float value) {
    return (float_bits(value) & 0x7F800000U) != 0x7F800000U;
}

static int float_bits_sane_range(float value, float min_value, float max_value) {
    return float_bits_finite(value) && value >= min_value && value <= max_value;
}

static float *metrics_float_symbol(const char *symbol) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: Metrics symbol not found: %s", symbol);
        return NULL;
    }
    return (float *)addr;
}

static uint32_t *metrics_u32_symbol(const char *symbol) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: Metrics symbol not found: %s", symbol);
        return NULL;
    }
    return (uint32_t *)addr;
}

static uint64_t *metrics_u64_symbol(const char *symbol) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: Metrics symbol not found: %s", symbol);
        return NULL;
    }
    return (uint64_t *)addr;
}

static uint8_t *metrics_u8_symbol(const char *symbol) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: Metrics symbol not found: %s", symbol);
        return NULL;
    }
    return (uint8_t *)addr;
}

static uint8_t *diag_u8_symbol(const char *symbol, const char *label) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: render gate symbol not found: %s (%s)", label, symbol);
        return NULL;
    }
    return (uint8_t *)addr;
}

static uint32_t *diag_u32_symbol(const char *symbol, const char *label) {
    uintptr_t addr = so_symbol(&so_mod_gameengine, symbol);
    if (!addr) {
        l_warn("Diag: render gate symbol not found: %s (%s)", label, symbol);
        return NULL;
    }
    return (uint32_t *)addr;
}

static void init_metrics_diag(void) {
    g_metrics_diag.frame_time =
        metrics_float_symbol("_ZN7Metrics10mFrameTimeE");
    g_metrics_diag.actual_frame_time =
        metrics_float_symbol("_ZN7Metrics16mActualFrameTimeE");
    g_metrics_diag.average_frame_time =
        metrics_float_symbol("_ZN7Metrics17mAverageFrameTimeE");
    g_metrics_diag.total_time =
        metrics_float_symbol("_ZN7Metrics10mTotalTimeE");
    g_metrics_diag.scale =
        metrics_float_symbol("_ZN7Metrics6mScaleE");
    g_metrics_diag.next_frame_time =
        metrics_float_symbol("_ZN7Metrics14mNextFrameTimeE");
    g_metrics_diag.fixed_time_step =
        metrics_float_symbol("_ZN7Metrics14mFixedTimeStepE");
    g_metrics_diag.delay =
        metrics_float_symbol("_ZN7Metrics6mDelayE");
    g_metrics_diag.min_frame_time =
        metrics_float_symbol("_ZN7Metrics13mMinFrameTimeE");
    g_metrics_diag.frame_num =
        metrics_u32_symbol("_ZN7Metrics9mFrameNumE");
    g_metrics_diag.frame_stamp =
        metrics_u64_symbol("_ZN7Metrics11mFrameStampE");
    g_metrics_diag.reset =
        metrics_u8_symbol("_ZN7Metrics7mbResetE");
    g_metrics_diag.use_time_get_time =
        metrics_u8_symbol("_ZN7Metrics16mbUseTimeGetTimeE");
    g_metrics_diag.initialized = 1;
}

static void init_render_gate_diag(void) {
    g_render_gate_diag.app_wait_for_messages =
        diag_u8_symbol("_ZN11Application18msbWaitForMessagesE", "Application::msbWaitForMessages");
    g_render_gate_diag.app_active =
        diag_u8_symbol("_ZN11Application20msbApplicationActiveE", "Application::msbApplicationActive");
    g_render_gate_diag.game_suspend_loop =
        diag_u8_symbol("_ZN10GameEngine17mbSuspendGameLoopE", "GameEngine::mbSuspendGameLoop");
    g_render_gate_diag.game_post_update_script_call =
        diag_u8_symbol("_ZN10GameEngine24mbDoPostUpdateScriptCallE", "GameEngine::mbDoPostUpdateScriptCall");
    g_render_gate_diag.game_browser_during_shutdown =
        diag_u8_symbol("_ZN10GameEngine23mbBrowserDuringShutdownE", "GameEngine::mbBrowserDuringShutdown");
    g_render_gate_diag.game_requested_quit =
        diag_u8_symbol("_ZN10GameEngine15mbRequestedQuitE", "GameEngine::mbRequestedQuit");
    g_render_gate_diag.game_skip_platform_controller_screen =
        diag_u8_symbol("_ZN10GameEngine30mbSkipPlatformControllerScreenE", "GameEngine::mbSkipPlatformControllerScreen");
    g_render_gate_diag.render_enable =
        diag_u8_symbol("_ZN12RenderDevice17mbEnableRenderingE", "RenderDevice::mbEnableRendering");
    g_render_gate_diag.render_device_initialized =
        diag_u8_symbol("_ZN12RenderDevice20mDeviceIsInitializedE", "RenderDevice::mDeviceIsInitialized");
    g_render_gate_diag.render_in_frame =
        diag_u8_symbol("_ZN12RenderDevice9mbInFrameE", "RenderDevice::mbInFrame");
    g_render_gate_diag.render_hwnd =
        diag_u32_symbol("_ZN12RenderDevice12msRenderHwndE", "RenderDevice::msRenderHwnd");
    g_render_gate_diag.device_width =
        diag_u32_symbol("_ZN12RenderDevice12mDeviceWidthE", "RenderDevice::mDeviceWidth");
    g_render_gate_diag.device_height =
        diag_u32_symbol("_ZN12RenderDevice13mDeviceHeightE", "RenderDevice::mDeviceHeight");
    g_render_gate_diag.game_width =
        diag_u32_symbol("_ZN12RenderDevice10mGameWidthE", "RenderDevice::mGameWidth");
    g_render_gate_diag.game_height =
        diag_u32_symbol("_ZN12RenderDevice11mGameHeightE", "RenderDevice::mGameHeight");
    g_render_gate_diag.initialized = 1;
}

static int repair_metric_float(float *slot,
                               const char *name,
                               float replacement,
                               float min_value,
                               float max_value,
                               char *repairs,
                               size_t repairs_size) {
    if (!slot) {
        return 0;
    }

    const float before = *slot;
    if (float_bits_sane_range(before, min_value, max_value)) {
        return 0;
    }

    *slot = replacement;
    if (repairs && repairs_size > 0) {
        const size_t used = strlen(repairs);
        if (used < repairs_size - 1U) {
            snprintf(repairs + used, repairs_size - used, "%s%s", used ? "," : "", name);
        }
    }
    return 1;
}

static int diag_read_u8(const uint8_t *slot) {
    return slot ? (int)*slot : -1;
}

static uint32_t diag_read_u32(const uint32_t *slot) {
    return slot ? *slot : 0U;
}

/* From java.c — the low-res the game renders into the FBO before upscaling. */
extern int mcsm_get_render_scale_width(void);
extern int mcsm_get_render_scale_height(void);

static void force_native_render_dimensions(const char *phase) {
    static uint32_t force_count = 0;
    static uint32_t change_count = 0;

    if (!g_render_gate_diag.initialized) {
        return;
    }

    /* RENDER-SCALE FIX 2026-06-29: force the engine's render dimensions to the
     * RENDER-SCALE size (= the FBO size), not hardcoded native 960x544. With
     * fb_override.txt set to a lower res, the engine was still rendering full
     * native into the smaller FBO -> the image filled only a corner / spilled off
     * one side. Matching the engine dims to the FBO makes it render fullscreen at
     * the low res, then gl_swap bilinear-upscales the FBO to native. At native
     * (no override) render_scale = 960x544 so behaviour is unchanged. */
    const uint32_t rw = (uint32_t)mcsm_get_render_scale_width();
    const uint32_t rh = (uint32_t)mcsm_get_render_scale_height();

    const uint32_t before_dw = diag_read_u32(g_render_gate_diag.device_width);
    const uint32_t before_dh = diag_read_u32(g_render_gate_diag.device_height);
    const uint32_t before_gw = diag_read_u32(g_render_gate_diag.game_width);
    const uint32_t before_gh = diag_read_u32(g_render_gate_diag.game_height);
    const int changed =
        before_dw != rw ||
        before_dh != rh ||
        before_gw != rw ||
        before_gh != rh;

    if (g_render_gate_diag.device_width) {
        *g_render_gate_diag.device_width = rw;
    }
    if (g_render_gate_diag.device_height) {
        *g_render_gate_diag.device_height = rh;
    }
    if (g_render_gate_diag.game_width) {
        *g_render_gate_diag.game_width = rw;
    }
    if (g_render_gate_diag.game_height) {
        *g_render_gate_diag.game_height = rh;
    }

    force_count++;
    if (changed) {
        change_count++;
    }

    if (force_count <= 8U || (changed && (change_count <= 32U || (change_count % 256U) == 0U))) {
        l_info("Diag: RenderDims force[%s] count=%u changes=%u dev=%ux%u->%ux%u game=%ux%u->%ux%u",
               phase ? phase : "?",
               force_count,
               change_count,
               before_dw,
               before_dh,
               rw,
               rh,
               before_gw,
               before_gh,
               rw,
               rh);
    }
}

static void log_render_gate_diag(uint32_t loop_count, const char *phase) {
    if (!g_render_gate_diag.initialized) {
        return;
    }

    const uint32_t frame_num = g_metrics_diag.frame_num ? *g_metrics_diag.frame_num : 0U;
    l_info("Diag: RenderGate[%s] loop=%u frame=%u app_wait=%d app_active=%d ge_suspend=%d ge_post=%d ge_browser_shutdown=%d ge_quit=%d ge_skip_platform=%d rd_enable=%d rd_init=%d rd_inframe=%d hwnd=0x%08X dev=%ux%u game=%ux%u",
           phase,
           loop_count,
           frame_num,
           diag_read_u8(g_render_gate_diag.app_wait_for_messages),
           diag_read_u8(g_render_gate_diag.app_active),
           diag_read_u8(g_render_gate_diag.game_suspend_loop),
           diag_read_u8(g_render_gate_diag.game_post_update_script_call),
           diag_read_u8(g_render_gate_diag.game_browser_during_shutdown),
           diag_read_u8(g_render_gate_diag.game_requested_quit),
           diag_read_u8(g_render_gate_diag.game_skip_platform_controller_screen),
           diag_read_u8(g_render_gate_diag.render_enable),
           diag_read_u8(g_render_gate_diag.render_device_initialized),
           diag_read_u8(g_render_gate_diag.render_in_frame),
           diag_read_u32(g_render_gate_diag.render_hwnd),
           diag_read_u32(g_render_gate_diag.device_width),
           diag_read_u32(g_render_gate_diag.device_height),
           diag_read_u32(g_render_gate_diag.game_width),
           diag_read_u32(g_render_gate_diag.game_height));
}

static void force_application_active(const char *phase) {
    static uint32_t log_count = 0;
    const int had_wait = diag_read_u8(g_render_gate_diag.app_wait_for_messages);
    const int had_active = diag_read_u8(g_render_gate_diag.app_active);

    if (g_render_gate_diag.app_wait_for_messages) {
        *g_render_gate_diag.app_wait_for_messages = 0;
    }
    if (g_render_gate_diag.app_active) {
        *g_render_gate_diag.app_active = 1;
    }

    if ((had_wait != 0 || had_active != 1) &&
        (log_count < 32U || (log_count % 256U) == 0U)) {
        l_info("Diag: AppState force[%s] wait=%d->0 active=%d->1",
               phase ? phase : "?",
               had_wait,
               had_active);
        log_count++;
    }
}

static void log_metrics_diag(uint32_t loop_count, const char *phase, int repairs, const char *repair_names) {
    if (!g_metrics_diag.initialized) {
        return;
    }

    const int log_this = repairs != 0 || loop_count <= 8U || (loop_count % 1024U) == 0U;
    if (!log_this) {
        return;
    }

    const float ft = g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : 0.0f;
    const float aft = g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : 0.0f;
    const float avg = g_metrics_diag.average_frame_time ? *g_metrics_diag.average_frame_time : 0.0f;
    const float total = g_metrics_diag.total_time ? *g_metrics_diag.total_time : 0.0f;
    const float scale = g_metrics_diag.scale ? *g_metrics_diag.scale : 0.0f;
    const float next = g_metrics_diag.next_frame_time ? *g_metrics_diag.next_frame_time : 0.0f;
    const float fixed = g_metrics_diag.fixed_time_step ? *g_metrics_diag.fixed_time_step : 0.0f;
    const float delay = g_metrics_diag.delay ? *g_metrics_diag.delay : 0.0f;
    const float min_frame = g_metrics_diag.min_frame_time ? *g_metrics_diag.min_frame_time : 0.0f;
    const uint32_t frame_num = g_metrics_diag.frame_num ? *g_metrics_diag.frame_num : 0U;
    const uint64_t stamp = g_metrics_diag.frame_stamp ? *g_metrics_diag.frame_stamp : 0ULL;
    const uint32_t reset = g_metrics_diag.reset ? (uint32_t)*g_metrics_diag.reset : 0U;
    const uint32_t timeget = g_metrics_diag.use_time_get_time ? (uint32_t)*g_metrics_diag.use_time_get_time : 0U;

    l_info("Diag: Metrics[%s] loop=%u frame=%u stamp=%llu reset=%u timeget=%u repairs=%d%s%s",
           phase,
           loop_count,
           frame_num,
           (unsigned long long)stamp,
           reset,
           timeget,
           repairs,
           (repair_names && repair_names[0]) ? " fixed=" : "",
           (repair_names && repair_names[0]) ? repair_names : "");
    l_info("Diag: Metrics values ft=%.6f/%08X aft=%.6f/%08X avg=%.6f/%08X total=%.6f/%08X scale=%.6f/%08X next=%.6f/%08X fixed=%.6f/%08X delay=%.6f/%08X min=%.6f/%08X",
           ft, float_bits(ft),
           aft, float_bits(aft),
           avg, float_bits(avg),
           total, float_bits(total),
           scale, float_bits(scale),
           next, float_bits(next),
           fixed, float_bits(fixed),
           delay, float_bits(delay),
           min_frame, float_bits(min_frame));
}

static void metrics_diag_tick(uint32_t loop_count, const char *phase) {
    char repairs[96] = {0};
    int repair_count = 0;

    repair_count += repair_metric_float(g_metrics_diag.frame_time,
                                        "mFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.actual_frame_time,
                                        "mActualFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.average_frame_time,
                                        "mAverageFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.total_time,
                                        "mTotalTime",
                                        0.0f,
                                        0.0f,
                                        86400.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.scale,
                                        "mScale",
                                        1.0f,
                                        0.0f,
                                        10.0f,
                                        repairs,
                                        sizeof(repairs));
    /* Metrics::NewFrame stores -1.0 in mNextFrameTime as a sentinel. */
    repair_count += repair_metric_float(g_metrics_diag.fixed_time_step,
                                        "mFixedTimeStep",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.delay,
                                        "mDelay",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));
    repair_count += repair_metric_float(g_metrics_diag.min_frame_time,
                                        "mMinFrameTime",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
                                        sizeof(repairs));

    log_metrics_diag(loop_count, phase, repair_count, repairs);
}

static int playback_dt_is_usable(float value);

/* SLOW-ANIMATION FIX 2026-06-29: the animation dt was clamped to a max of 1/30s
 * (33ms). The governor uses REAL elapsed time as the dt, so any frame slower than
 * 30fps (every heavy cutscene/gameplay scene) got its animation advance capped at
 * 33ms while real time advanced much more -> animations crawled in slow motion
 * (log proof: elapsed=310ms -> out=33ms = 0.1x speed). Widen the window so the
 * animation advances at the TRUE frame delta down to ~10fps; only genuine
 * multi-100ms stalls get clamped (so a scene-load freeze can't fling the pose).
 * MIN lowered so >60fps frames don't over-advance (fast motion). */
#define ANIM_DT_MIN_SECONDS   (1.0f / 240.0f)
#define ANIM_DT_MAX_SECONDS   (1.0f / 10.0f)
#define ANIM_STALL_RESET_US   120000ULL

static float clamp_animation_dt(float value) {
    if (!float_bits_finite(value) || value <= 0.0f) {
        return ANIM_DT_MIN_SECONDS;
    }
    if (value < ANIM_DT_MIN_SECONDS) {
        return ANIM_DT_MIN_SECONDS;
    }
    if (value > ANIM_DT_MAX_SECONDS) {
        return ANIM_DT_MAX_SECONDS;
    }
    return value;
}

static float abs_float_delta(float a, float b) {
    return (a >= b) ? (a - b) : (b - a);
}

static float g_anim_governed_dt = ANIM_DT_MIN_SECONDS;
static uint64_t g_anim_governor_last_us = 0;
static int g_anim_governor_initialized = 0;

static float animation_governor_update(float engine_frame_time,
                                       float engine_actual_frame_time,
                                       const char *phase,
                                       uint32_t count) {
    const uint64_t now = sceKernelGetSystemTimeWide();
    float target = ANIM_DT_MIN_SECONDS;
    uint64_t elapsed_us = 0;

    if (g_anim_governor_last_us != 0ULL && now >= g_anim_governor_last_us) {
        elapsed_us = now - g_anim_governor_last_us;
    }
    g_anim_governor_last_us = now;

    if (elapsed_us >= 8000ULL && elapsed_us <= ANIM_STALL_RESET_US) {
        target = clamp_animation_dt((float)elapsed_us / 1000000.0f);
    } else if (elapsed_us > ANIM_STALL_RESET_US) {
        /* STALL (scene-load hitch / pause): do NOT fold this big gap into the
         * smoothed dt. Folding MAX in here spiked the animation speed for several
         * frames AFTER every hitch (the "trip a lot" jerk). Keep the last good
         * smoothed value so animation resumes at the true frame rate, smoothly. */
        if (g_anim_governor_initialized) {
            return clamp_animation_dt(g_anim_governed_dt);
        }
        target = ANIM_DT_MIN_SECONDS;
    } else if (playback_dt_is_usable(engine_frame_time)) {
        target = clamp_animation_dt(engine_frame_time);
    } else if (playback_dt_is_usable(engine_actual_frame_time)) {
        target = clamp_animation_dt(engine_actual_frame_time);
    }

    if (!g_anim_governor_initialized) {
        g_anim_governed_dt = target;
        g_anim_governor_initialized = 1;
    } else {
        g_anim_governed_dt = (g_anim_governed_dt * 0.50f) + (target * 0.50f);
        g_anim_governed_dt = clamp_animation_dt(g_anim_governed_dt);
    }

    if (count <= 16U ||
        elapsed_us > ANIM_STALL_RESET_US ||
        (abs_float_delta(engine_frame_time, g_anim_governed_dt) > 0.020f) ||
        (count % 512U) == 0U) {
        l_info("ANIM: dt governor[%s] count=%u elapsed=%lluus engine=%.6f/%.6f target=%.6f out=%.6f",
               phase ? phase : "?",
               count,
               (unsigned long long)elapsed_us,
               engine_frame_time,
               engine_actual_frame_time,
               target,
               g_anim_governed_dt);
    }

    return g_anim_governed_dt;
}

static float animation_governor_current_or_repair(float frame_time,
                                                  float actual_frame_time) {
    if (g_anim_governor_initialized) {
        return clamp_animation_dt(g_anim_governed_dt);
    }
    if (playback_dt_is_usable(frame_time)) {
        return clamp_animation_dt(frame_time);
    }
    if (playback_dt_is_usable(actual_frame_time)) {
        return clamp_animation_dt(actual_frame_time);
    }
    return ANIM_DT_MIN_SECONDS;
}

static void metrics_force_animation_dt(float dt, const char *phase, uint32_t count) {
    if (!g_boot_scene_active || !g_metrics_diag.initialized) {
        return;
    }

    /* REVERTED 2026-06-30: forcing dt = the fixed frame-pace period made
     * animations play in slow motion / near-frozen (user: "extremely slow,
     * worked better before"). Use the governor-derived dt (real elapsed time) so
     * animation plays at correct wall-clock speed. The real stutter fix is the
     * O0 shader-compiler change, not the dt. */
    const float fixed_dt = clamp_animation_dt(dt);
    const float old_ft = g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : fixed_dt;
    const float old_aft = g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : fixed_dt;
    const float old_total = g_metrics_diag.total_time ? *g_metrics_diag.total_time : 0.0f;
    int changed = 0;

    if (g_metrics_diag.frame_time && abs_float_delta(*g_metrics_diag.frame_time, fixed_dt) > 0.0005f) {
        *g_metrics_diag.frame_time = fixed_dt;
        changed = 1;
    }
    if (g_metrics_diag.actual_frame_time &&
        abs_float_delta(*g_metrics_diag.actual_frame_time, fixed_dt) > 0.0005f) {
        *g_metrics_diag.actual_frame_time = fixed_dt;
        changed = 1;
    }
    if (g_metrics_diag.average_frame_time &&
        abs_float_delta(*g_metrics_diag.average_frame_time, fixed_dt) > 0.0005f) {
        *g_metrics_diag.average_frame_time = fixed_dt;
        changed = 1;
    }
    if (g_metrics_diag.fixed_time_step &&
        abs_float_delta(*g_metrics_diag.fixed_time_step, fixed_dt) > 0.0005f) {
        *g_metrics_diag.fixed_time_step = fixed_dt;
        changed = 1;
    }
    if (g_metrics_diag.delay && *g_metrics_diag.delay != 0.0f) {
        *g_metrics_diag.delay = 0.0f;
        changed = 1;
    }

    if (g_metrics_diag.total_time &&
        playback_dt_is_usable(old_ft) &&
        old_ft > fixed_dt &&
        old_ft <= 1.0f &&
        *g_metrics_diag.total_time >= old_ft) {
        *g_metrics_diag.total_time += (fixed_dt - old_ft);
        changed = 1;
    }

    if (changed &&
        (count <= 32U ||
         old_ft > ANIM_DT_MAX_SECONDS ||
         old_aft > ANIM_DT_MAX_SECONDS ||
         (count % 512U) == 0U)) {
        l_info("ANIM: metrics fixed[%s] count=%u ft %.6f->%.6f aft %.6f->%.6f total %.3f->%.3f",
               phase ? phase : "?",
               count,
               old_ft,
               g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : fixed_dt,
               old_aft,
               g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : fixed_dt,
               old_total,
               g_metrics_diag.total_time ? *g_metrics_diag.total_time : old_total);
    }
}

static int return_address_in_symbol(void *retaddr, uintptr_t symbol_addr, size_t symbol_size) {
    const uintptr_t addr = (uintptr_t)retaddr;

    return symbol_addr &&
           symbol_size &&
           addr >= symbol_addr &&
           addr < symbol_addr + symbol_size;
}

static void describe_return_address(void *retaddr, char *dst, size_t dst_size) {
    const uintptr_t addr = (uintptr_t)retaddr;

    if (!dst || dst_size == 0) {
        return;
    }

    if (return_address_in_symbol(retaddr,
                                 g_renderoverlay_update_render_thread_addr,
                                 g_renderoverlay_update_render_thread_size)) {
        snprintf(dst,
                 dst_size,
                 "%p(RenderOverlay::UpdateRenderThread+0x%X)",
                 retaddr,
                 (unsigned)(addr - g_renderoverlay_update_render_thread_addr));
        return;
    }

    if (return_address_in_symbol(retaddr,
                                 g_renderframe_push_view_addr,
                                 g_renderframe_push_view_size)) {
        snprintf(dst,
                 dst_size,
                 "%p(RenderFrame::PushView+0x%X)",
                 retaddr,
                 (unsigned)(addr - g_renderframe_push_view_addr));
        return;
    }

    snprintf(dst, dst_size, "%p", retaddr);
}

static void stream_pump_preload(void);  /* forward decl; defined after boot hooks */

static void resolve_animation_runtime_flags(void) {
    if (g_animation_flag_symbols_resolved) {
        return;
    }
    g_animation_flag_symbols_resolved = 1;

    g_set_chore_filter_includes_non_skeleton =
        (engine_bool_setter_fn)so_symbol(&so_mod_gameengine,
            "_ZN10GameEngine43SetChoreAgentGroupFilterIncludesNonSkeletonEb");
    g_set_fix_recursive_animation_contribution =
        (engine_bool_setter_fn)so_symbol(&so_mod_gameengine,
            "_ZN10GameEngine36SetFixRecursiveAnimationContributionEb");
    g_fix_recursive_animation_contribution =
        (uint8_t *)so_symbol(&so_mod_gameengine,
            "_ZN10GameEngine35mbFixRecursiveAnimationContributionE");

    l_info("ANIM: runtime flag symbols nonSkeleton=%p fixRecursiveSetter=%p fixRecursiveValue=%p",
           (void *)g_set_chore_filter_includes_non_skeleton,
           (void *)g_set_fix_recursive_animation_contribution,
           (void *)g_fix_recursive_animation_contribution);
}

/* Codex's recursive-bone-contribution fix is what makes skeletal animation
 * work, but recursive per-bone contribution is CPU-heavy and is the prime
 * suspect for the ~40ms sim spikes in animated scenes. Make it tunable so its
 * real cost can be measured / the user can pick perf-vs-animation:
 * ux0:data/mcsm/anim_full.txt = "0" -> disable (faster, animation degraded),
 * absent or "1" -> full (default, current behaviour). */
static int mcsm_anim_full(void) { return mcsm_cfg()->skinning_full; }

/* ANIMATION UPDATE-RATE THROTTLE (2026-07-20, priority-3 sim lever). The heaviest
 * scenes spend ~50ms in the sim, dominated by animation blend across many
 * PlaybackControllers. Advancing them every Nth frame with N*accumulated-dt halves
 * (N=2) the blend cost at CORRECT wall-clock speed (no slow-mo). Opt-in + tunable:
 * ux0:data/mcsm/settings/anim_rate.txt = 1 (full, default) / 2 (half) / 3 (third).
 * Visual trade: close-up motion + lip-sync step at ~15Hz (N=2), so it's for heavy
 * gameplay/crowd scenes, not dialogue. Default 1 = zero change. */
static int mcsm_anim_rate(void) {
    static int rate = -1;
    if (rate < 0) {
        rate = 1;
        FILE *f = mcsm_open_setting("anim_rate.txt", "r");
        if (f) { int v = 1; if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 3) rate = v; fclose(f); }
        l_info("ANIM: update-rate = 1/%d", rate);
    }
    return rate;
}

static void force_animation_runtime_flags(const char *phase) {
    static uint32_t count = 0;
    static int last_recursive_full = -1;
    resolve_animation_runtime_flags();
    const int recursive_full = mcsm_anim_full();
    const int recursive_value =
        g_fix_recursive_animation_contribution ? (int)*g_fix_recursive_animation_contribution : -1;
    const int refresh = (count < 8U) || ((count & 0x3fU) == 0U);
    const int recursive_needs_write =
        refresh || last_recursive_full != recursive_full || recursive_value != recursive_full;

    if (g_set_chore_filter_includes_non_skeleton && refresh) {
        g_set_chore_filter_includes_non_skeleton(1);
    }
    if (g_set_fix_recursive_animation_contribution && recursive_needs_write) {
        g_set_fix_recursive_animation_contribution(recursive_full);
    }
    if (g_fix_recursive_animation_contribution && recursive_needs_write) {
        *g_fix_recursive_animation_contribution = (uint8_t)recursive_full;
    }
    last_recursive_full = recursive_full;

    count++;
    if (count <= 8U || (count & 0x1ffU) == 0U) {
        l_info("ANIM: forced runtime flags #%u phase=%s nonSkeleton=1 recursive=%d value=%u",
               count,
               phase ? phase : "?",
               recursive_full,
               g_fix_recursive_animation_contribution ? (unsigned)*g_fix_recursive_animation_contribution : 0U);
    }
}

static void patch_chore_full_update_path(void) {
    static int applied = 0;
    if (applied) {
        return;
    }
    applied = 1;

    /* The Android engine's global chore tick calls ChoreInst::Update(false),
     * which in turn passes false down into ChoreAgentInst::SetCurrentTime.
     * On Vita this leaves menu/diorama agents with advancing controllers but
     * incomplete value application. Patch the call sites to use the engine's
     * own full-update path without adding hot per-chore hooks. */
    uintptr_t update_all = so_symbol(&so_mod_gameengine,
        "_ZN9ChoreInst20UpdateChoreInstancesEv");
    uintptr_t set_controller = so_symbol(&so_mod_gameengine,
        "_ZN14ChoreAgentInst13SetControllerE3PtrI18PlaybackControllerE");

    int ok_update_all = 0;
    int ok_set_controller = 0;
    if (update_all) {
        ok_update_all = patch_arm32_instruction(
            "ANIM ChoreInst::UpdateChoreInstances full-agent update",
            update_all + 0x1cU,
            0xE3A01000U, /* mov r1,#0 */
            0xE3A01001U  /* mov r1,#1 */);
    } else {
        l_warn("ANIM: ChoreInst::UpdateChoreInstances symbol not found.");
    }

    if (set_controller) {
        ok_set_controller = patch_arm32_instruction(
            "ANIM ChoreAgentInst::SetController initial full update",
            set_controller + 0x178U,
            0xE1A01009U, /* mov r1,r9 (r9 is zero here) */
            0xE3A01001U  /* mov r1,#1 */);
    } else {
        l_warn("ANIM: ChoreAgentInst::SetController symbol not found.");
    }

    l_info("ANIM: full chore update patches update_all=%d set_controller=%d",
           ok_update_all,
           ok_set_controller);
}

static void hook_gameengine_start(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    log_diag_counter("GameEngine_Start", count, first_ms, NULL);
    force_animation_runtime_flags("start-pre");
    SO_CONTINUE_VOID(g_hook_gameengine_start);
    force_animation_runtime_flags("start-post");
}

static void hook_metrics_new_frame(uint32_t min_frame_time_bits) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    typedef void (*metrics_new_frame_raw_fn)(uint32_t);
    kuKernelCpuUnrestrictedMemcpy((void *)g_hook_metrics_new_frame.addr,
                                  g_hook_metrics_new_frame.orig_instr,
                                  sizeof(g_hook_metrics_new_frame.orig_instr));
    kuKernelFlushCaches((void *)g_hook_metrics_new_frame.addr,
                        sizeof(g_hook_metrics_new_frame.orig_instr));
    metrics_new_frame_raw_fn fn = g_hook_metrics_new_frame.thumb_addr
        ? (metrics_new_frame_raw_fn)g_hook_metrics_new_frame.thumb_addr
        : (metrics_new_frame_raw_fn)g_hook_metrics_new_frame.addr;
    fn(min_frame_time_bits);
    kuKernelCpuUnrestrictedMemcpy((void *)g_hook_metrics_new_frame.addr,
                                  g_hook_metrics_new_frame.patch_instr,
                                  sizeof(g_hook_metrics_new_frame.patch_instr));
    kuKernelFlushCaches((void *)g_hook_metrics_new_frame.addr,
                        sizeof(g_hook_metrics_new_frame.patch_instr));

    const float engine_ft = g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : 0.0f;
    const float engine_aft = g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : 0.0f;
    const float fixed_dt = animation_governor_update(engine_ft, engine_aft, "newframe", count);
    metrics_force_animation_dt(fixed_dt, "newframe", count);

    if (should_log_diag_count(count)) {
        char extra[128];
        snprintf(extra,
                 sizeof(extra),
                 "min=%.6f scene=%d governed=%.6f",
                 float_from_bits(min_frame_time_bits),
                 (int)g_boot_scene_active,
                 fixed_dt);
        log_diag_counter("Metrics::NewFrame", count, first_ms, extra);
    }
}

static void hook_gameengine_loop(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    /* STREAMING FIX: pump the async preload batch every frame while a
     * ScenePreload is in flight. */
    stream_pump_preload();
    if ((count & 0x0fU) == 1U) {
        force_animation_runtime_flags("loop");
    }

    force_application_active("loop-pre");
    force_native_render_dimensions("loop-pre");
    if (should_log_diag_count(count)) {
        log_diag_counter("GameEngine_Loop", count, first_ms, NULL);
        log_render_gate_diag(count, "loop-pre");
    }
    metrics_diag_tick(count, "pre");
    /* DIP PROFILER: time the actual engine sim work (excludes our frame pace).
     * If the sim loop itself spikes (>22ms) the bottleneck is engine-side
     * logic/animation, not rendering. Keep this to severe spikes only; logging
     * every 20-50ms frame became part of the stutter during animation tests. */
    const uint64_t sim_t0 = sceKernelGetSystemTimeWide();
    SO_CONTINUE_VOID(g_hook_gameengine_loop);
    const uint32_t sim_us = (uint32_t)(sceKernelGetSystemTimeWide() - sim_t0);
    const uint32_t sim_ms = sim_us / 1000U;
    /* Feed the pure sim-work cost to the adaptive ARM-clock governor (battery):
     * downclock when scenes are light, jump to the ceiling the moment they aren't. */
    mcsm_clock_governor_tick(sim_us);
    /* DIAG: log gameplay CPU-sim cost on dips (>20ms), throttled. Compare against
     * DIP-RENDER dt for the same frames: sim≈dt => CPU-bound; sim<<dt => GPU/VRAM-bound. */
    { static unsigned s_simc = 0;
      if (sim_ms > 20U && (s_simc++ & 0xFU) == 0U) l_info("DIP-SIM frame=%u work=%ums", count, sim_ms); }
    if (should_log_diag_count(count)) {
        l_info("Diag: GameEngine_Loop returned count=%u", count);
    }
    force_application_active("loop-post");
    force_native_render_dimensions("loop-post");
    metrics_diag_tick(count, "post");
    metrics_force_animation_dt(
        animation_governor_current_or_repair(
            g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : 0.0f,
            g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : 0.0f),
        "loop-post",
        count);
    if (should_log_diag_count(count)) {
        log_render_gate_diag(count, "loop-post");
    }
    /* Steady frame pacing so the engine's animation delta is consistent. */
    mcsm_pace_frame();
}

static void hook_gameengine_render(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    force_native_render_dimensions("render-enter");
    if (should_log_diag_count(count)) {
        log_diag_counter("GameEngine::Render", count, first_ms, NULL);
        log_render_gate_diag(count, "render-enter");
    }
    SO_CONTINUE_VOID(g_hook_gameengine_render);
    force_native_render_dimensions("render-exit");
    if (should_log_diag_count(count)) {
        log_render_gate_diag(count, "render-exit");
    }
}

static int hook_renderdevice_begin_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    force_native_render_dimensions("begin-frame-pre");
    int ret = SO_CONTINUE(int, g_hook_render_begin_frame);
    force_native_render_dimensions("begin-frame-post");
    if (should_log_diag_count(count)) {
        char extra[32];
        snprintf(extra, sizeof(extra), "ret=%d", ret);
        log_diag_counter("RenderDevice::BeginFrame", count, first_ms, extra);
    }
    return ret;
}

static void hook_renderdevice_end_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (should_log_diag_count(count)) {
        log_diag_counter("RenderDevice::EndFrame", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_render_end_frame);
}

static void hook_renderdevice_present(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (should_log_diag_count(count)) {
        log_diag_counter("RenderDevice::Present", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_render_present);
}

static void hook_application_sdl_swap(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (should_log_diag_count(count)) {
        log_diag_counter("Application_SDL::Swap", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_app_swap);
}

static void hook_application_sdl_on_fingering(int event_type, const void *event) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        int64_t finger_id = -1;
        float x = 0.0f;
        float y = 0.0f;
        if (event) {
            const uint8_t *bytes = (const uint8_t *)event;
            finger_id = *(const int64_t *)(const void *)(bytes + 16);
            x = *(const float *)(const void *)(bytes + 24);
            y = *(const float *)(const void *)(bytes + 28);
        }
        l_info("Diag: Application_SDL::OnFingering count=%u type=%d finger=%lld xy=%.3f,%.3f",
               count,
               event_type,
               (long long)finger_id,
               x,
               y);
    }
    SO_CONTINUE_VOID(g_hook_app_on_fingering, event_type, event);
}

static void hook_application_sdl_on_mouse_event(int event_type, const void *event) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        int which = -1;
        int button = -1;
        int x = 0;
        int y = 0;
        if (event) {
            const uint8_t *bytes = (const uint8_t *)event;
            which = *(const int32_t *)(const void *)(bytes + 12);
            button = bytes[16];
            x = *(const int32_t *)(const void *)(bytes + 20);
            y = *(const int32_t *)(const void *)(bytes + 24);
        }
        l_info("Diag: Application_SDL::OnMouseEvent count=%u type=%d which=%d button=%d xy=%d,%d",
               count,
               event_type,
               which,
               button,
               x,
               y);
    }
    SO_CONTINUE_VOID(g_hook_app_on_mouse_event, event_type, event);
}

static int hook_gamewindow_playmode_mouse_move(void *self, unsigned int message, int button, int cursor, int unused) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        l_info("Diag: GameWindow_PlayMode::OnMouseMove count=%u self=%p msg=%u button=%d cursor=%d arg=%d",
               count,
               self,
               message,
               button,
               cursor,
               unused);
    }
    return SO_CONTINUE(int, g_hook_gamewindow_mouse_move, self, message, button, cursor, unused);
}

static int hook_gamewindow_playmode_mouse_down(void *self, unsigned int message, int button, int cursor, int unused) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        l_info("Diag: GameWindow_PlayMode::OnMouseDown count=%u self=%p msg=%u button=%d cursor=%d arg=%d",
               count,
               self,
               message,
               button,
               cursor,
               unused);
    }
    return SO_CONTINUE(int, g_hook_gamewindow_mouse_down, self, message, button, cursor, unused);
}

static int hook_gamewindow_playmode_mouse_up(void *self, unsigned int message, int button, int cursor, int unused) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        l_info("Diag: GameWindow_PlayMode::OnMouseUp count=%u self=%p msg=%u button=%d cursor=%d arg=%d",
               count,
               self,
               message,
               button,
               cursor,
               unused);
    }
    return SO_CONTINUE(int, g_hook_gamewindow_mouse_up, self, message, button, cursor, unused);
}

static void hook_touchscreenstate_set_legacy_pointer(void *self, const void *position) {
    static uint32_t count = 0;
    count++;
    if (count <= 64U) {
        int x = 0;
        int y = 0;
        if (position) {
            const int32_t *xy = (const int32_t *)position;
            x = xy[0];
            y = xy[1];
        }
        l_info("Diag: TouchScreenState::SetLegacyPointerPosition count=%u self=%p xy=%d,%d",
               count,
               self,
               x,
               y);
    }
    SO_CONTINUE_VOID(g_hook_touch_set_legacy_pointer, self, position);
}

static void hook_android_jni_poll_input_devices(void) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_poll();
    mcsm_register_virtual_controller();
    if (count <= 16U || (count % 256U) == 0U) {
        log_diag_counter("Android_JNI_PollInputDevices bypass", count, 0, NULL);
    }
}

static void hook_android_pump_events(void) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_poll();
    if (count <= 16U || (count % 256U) == 0U) {
        log_diag_counter("Android_PumpEvents bypass", count, 0, NULL);
    }
}

/* Some SDL wait calls happen on an engine-created thread with SP at the stack
 * guard page after Project Lua loads. Keep these hooks stackless: even a nested
 * diagnostic call can fault in the prologue before the first game loop. */
__attribute__((naked)) static int hook_sdl_wait_event_real(void *event) {
    (void)event;
    __asm__ volatile(
        "movs r0, #0\n"
        "bx lr\n"
    );
}

__attribute__((naked)) static int hook_sdl_wait_event_timeout_real(void *event, int timeout_ms) {
    (void)event;
    (void)timeout_ms;
    __asm__ volatile(
        "movs r0, #0\n"
        "bx lr\n"
    );
}

static void hook_suspend_game_loop(int suspend) {
    static int last_suspend = -1;
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (suspend != last_suspend || should_log_diag_count(count)) {
        char extra[32];
        snprintf(extra, sizeof(extra), "suspend=%d", suspend);
        log_diag_counter("GameEngine::SetSuspendGameLoop", count, first_ms, extra);
        last_suspend = suspend;
    }
    SO_CONTINUE_VOID(g_hook_suspend_game_loop, suspend);
}

static void hook_renderthread_submit_current_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (should_log_diag_count(count)) {
        log_diag_counter("RenderThread::SubmitCurrentFrame", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_renderthread_submit_current_frame);
}

static void hook_renderthread_end_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (should_log_diag_count(count)) {
        log_diag_counter("RenderThread::EndFrame", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_renderthread_end_frame);
}

static void hook_renderthread_finish_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (should_log_diag_count(count)) {
        log_diag_counter("RenderThread::FinishFrame", count, first_ms, NULL);
    }
    SO_CONTINUE_VOID(g_hook_renderthread_finish_frame);
}

static int hook_renderframe_execute(void *self, void *other_frame) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    static uint32_t paired_overlay_count = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    const uintptr_t overlay_frame = g_overlay_render_frame;
    const int paired_overlay_frame =
        g_boot_scene_active &&
        overlay_frame &&
        (uintptr_t)other_frame == overlay_frame;
    if (paired_overlay_frame) {
        paired_overlay_count++;
    }

    int ret = SO_CONTINUE(int, g_hook_renderframe_execute, self, other_frame);
    /* Bootstrap needs forced presents to escape the historical black-screen
     * path. After the first real scene render, leave the frame pairing exactly
     * as the engine requested; replacing the paired overlay frame with NULL
     * produced live RenderScene calls but unstable color-only output. */
    int forced = (!ret && !g_boot_scene_active) ? 1 : ret;
    const int log_paired =
        paired_overlay_frame &&
        (paired_overlay_count <= 8U || (paired_overlay_count % 256U) == 0U);
    if (should_log_diag_count(count) ||
        (!g_boot_scene_active && ret != 0) ||
        (g_boot_scene_active && (ret == 0 || log_paired))) {
        char extra[160];
        snprintf(extra,
                 sizeof(extra),
                 "self=%p other=%p used=%p overlay=%p ret=%d forced=%d scene=%d drop=0 filter=0 pair=%d",
                 self,
                 other_frame,
                 other_frame,
                 (void *)overlay_frame,
                 ret,
                 forced,
                 (int)g_boot_scene_active,
                 paired_overlay_frame);
        log_diag_counter("RenderFrame::Execute", count, first_ms, extra);
    }
    return forced;
}

static void hook_gamerender_render_frame(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (should_log_diag_count(count)) {
        log_diag_counter("GameRender::RenderFrame", count, first_ms, NULL);
    }
    force_native_render_dimensions("game-render-frame");
    SO_CONTINUE_VOID(g_hook_gamerender_render_frame);
}

static void hook_gamerender_render_scene(void *scene_ctx, const void *params) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }
    if (count == 1U) {
        /* First real scene render = assets loaded, game is starting. Stop the
         * loading screen and log the total asset-load time. */
        g_boot_scene_active = 1;
        launch_state_mark_scene_active();
        LS_DONE();
    }
    if (count <= 8U || (count % 1024U) == 0U) {
        char extra[96];
        snprintf(extra, sizeof(extra), "scene_ctx=%p params=%p", scene_ctx, params);
        log_diag_counter("GameRender::RenderScene", count, first_ms, extra);
    }
    force_native_render_dimensions("game-render-scene");
    SO_CONTINUE_VOID(g_hook_gamerender_render_scene, scene_ctx, params);
}

static int playback_dt_is_usable(float value) {
    return float_bits_finite(value) && value > 0.0001f && value < 1.0f;
}

static void hook_playback_controller_update(uint32_t frame_time_bits,
                                            uint32_t actual_frame_time_bits) {
    static uint32_t count = 0;
    static uint32_t repaired = 0;
    static uint64_t first_ms = 0;

    /* Animation-rate throttle (opt-in anim_rate.txt): on non-Nth frames, accumulate
     * this frame's dt and SKIP the whole update (controllers don't advance); on the
     * Nth frame, run the update with the accumulated dt so wall-clock speed stays
     * correct. Not applied during boot (governor owns that phase). Halves/thirds the
     * per-frame anim-blend sim cost in heavy scenes. */
    {
        const int rate = mcsm_anim_rate();
        /* FPS-GATED anim throttle: skip controller updates ONLY while the frame is
         * genuinely dropping (present cadence from gl_swap), so normal ~30fps scenes
         * keep FULL-rate animation. This is exactly what the earlier unconditional
         * throttle got wrong — it slowed the chest-open in LIGHT scenes. Device data:
         * light=~31ms present, heavy=~47-59ms. Quick engage (5 slow frames) / slow
         * release (90 healthy frames) with a wide 33-38ms dead-band, so a throttle
         * that lands a scene mid-band HOLDS steady instead of oscillating. */
        static int   throttle_active = 0;
        static uint32_t phase = 0;
        static float acc_ft = 0.0f, acc_aft = 0.0f;
        if (rate > 1 && !g_boot_scene_active) {
            extern volatile uint32_t g_mcsm_present_dt_us;
            const uint32_t pdt = g_mcsm_present_dt_us;
            static uint32_t slow = 0, fast = 0;
            if      (pdt > 38000u) { slow++; fast = 0; }   /* < ~26 fps: dropping   */
            else if (pdt < 33000u) { fast++; slow = 0; }   /* > ~30 fps: healthy    */
            else                   { slow = 0; fast = 0; } /* dead-band: hold state */
            if (!throttle_active && slow >= 5u) {
                throttle_active = 1; phase = 0; acc_ft = 0.0f; acc_aft = 0.0f;
                l_info("ANIM: throttle ON 1/%d (present=%uus dropping)", rate, pdt);
            } else if (throttle_active && fast >= 90u) {
                throttle_active = 0;
                l_info("ANIM: throttle OFF (present=%uus recovered)", pdt);
            }
        } else if (throttle_active) {
            throttle_active = 0;
        }
        if (throttle_active) {
            acc_ft  += float_from_bits(frame_time_bits);
            acc_aft += float_from_bits(actual_frame_time_bits);
            if ((++phase % (uint32_t)rate) != 0u) {
                return;   /* skip: hook stays armed, controllers hold this frame */
            }
            frame_time_bits        = float_bits(acc_ft);
            actual_frame_time_bits = float_bits(acc_aft);
            acc_ft = 0.0f; acc_aft = 0.0f;
        }
    }

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    const float frame_time = float_from_bits(frame_time_bits);
    const float actual_frame_time = float_from_bits(actual_frame_time_bits);
    uint32_t out_frame_bits = frame_time_bits;
    uint32_t out_actual_bits = actual_frame_time_bits;
    float out_frame_time = frame_time;
    float out_actual_frame_time = actual_frame_time;
    int did_repair = 0;

    if (g_boot_scene_active) {
        const float governed_dt =
            animation_governor_current_or_repair(frame_time, actual_frame_time);
        if (abs_float_delta(out_frame_time, governed_dt) > 0.0005f) {
            out_frame_time = governed_dt;
            out_frame_bits = float_bits(out_frame_time);
            did_repair = 1;
        }
        if (abs_float_delta(out_actual_frame_time, governed_dt) > 0.0005f) {
            out_actual_frame_time = governed_dt;
            out_actual_bits = float_bits(out_actual_frame_time);
            did_repair = 1;
        }
        metrics_force_animation_dt(governed_dt, "playback", count);
    }

    if (did_repair) {
        repaired++;
    }
    if (count <= 16U ||
        (did_repair && (repaired <= 8U || (repaired % 512U) == 0U)) ||
        (count % 2048U) == 0U) {
        char extra[256];
        snprintf(extra,
                 sizeof(extra),
                 "scene=%d ft=%.6f aft=%.6f out_ft=%.6f out_aft=%.6f repair=%d repaired=%u",
                 (int)g_boot_scene_active,
                 frame_time,
                 actual_frame_time,
                 out_frame_time,
                 out_actual_frame_time,
                 did_repair,
                 repaired);
        log_diag_counter("PlaybackController::Update", count, first_ms, extra);
    }

    /* libGameEngine was built softfp, so these float arguments arrive in r0/r1.
     * Keep both hook entry and original call in integer registers and only
     * reinterpret the bits locally for validation. */
    typedef void (*playback_update_raw_fn)(uint32_t, uint32_t);
    kuKernelCpuUnrestrictedMemcpy((void *)g_hook_playback_controller_update.addr,
                                  g_hook_playback_controller_update.orig_instr,
                                  sizeof(g_hook_playback_controller_update.orig_instr));
    kuKernelFlushCaches((void *)g_hook_playback_controller_update.addr,
                        sizeof(g_hook_playback_controller_update.orig_instr));
    playback_update_raw_fn fn = g_hook_playback_controller_update.thumb_addr
        ? (playback_update_raw_fn)g_hook_playback_controller_update.thumb_addr
        : (playback_update_raw_fn)g_hook_playback_controller_update.addr;
    fn(out_frame_bits, out_actual_bits);
    kuKernelCpuUnrestrictedMemcpy((void *)g_hook_playback_controller_update.addr,
                                  g_hook_playback_controller_update.patch_instr,
                                  sizeof(g_hook_playback_controller_update.patch_instr));
    kuKernelFlushCaches((void *)g_hook_playback_controller_update.addr,
                        sizeof(g_hook_playback_controller_update.patch_instr));
}

static void *hook_renderframe_allocate_view(void *self, const void *params) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (!self || !params) {
        if (count <= 8U || (count % 256U) == 0U) {
            char extra[128];
            char caller_desc[96];
            describe_return_address(__builtin_return_address(0), caller_desc, sizeof(caller_desc));
            snprintf(extra, sizeof(extra), "caller=%s self=%p params=%p skipped=1", caller_desc, self, params);
            log_diag_counter("RenderFrameScene::AllocateView", count, first_ms, extra);
        }
        return NULL;
    }

    void *view = SO_CONTINUE(void *, g_hook_renderframe_allocate_view, self, params);
    if (!view) {
        if (should_log_diag_count(count)) {
            char extra[80];
            char caller_desc[96];
            describe_return_address(__builtin_return_address(0), caller_desc, sizeof(caller_desc));
            snprintf(extra, sizeof(extra), "caller=%s view=NULL", caller_desc);
            log_diag_counter("RenderFrameScene::AllocateView", count, first_ms, extra);
        }
        return NULL;
    }

    const uint8_t *u8 = (const uint8_t *)view;
    const uint32_t pass_type = *(const uint32_t *)(const void *)(u8 + 64);
    const uint32_t target_ref = *(const uint32_t *)(const void *)(u8 + 176);
    const int log_this = (count <= 16U) || ((count % 512U) == 0U) || (u8[144] != 0U);

    if (log_this) {
        char extra[256];
        char caller_desc[96];
        describe_return_address(__builtin_return_address(0), caller_desc, sizeof(caller_desc));
        snprintf(extra,
                 sizeof(extra),
                 "caller=%s view=%p pass=%u flags128=%u/%u/%u flags144=%u/%u/%u mask=%u aux=%u target=%08X",
                 caller_desc,
                 view,
                 pass_type,
                 u8[128],
                 u8[129],
                 u8[130],
                 u8[144],
                 u8[145],
                 u8[146],
                 u8[147],
                 u8[148],
                 target_ref);
        log_diag_counter("RenderFrameScene::AllocateView", count, first_ms, extra);
    }

    return view;
}

static void *hook_renderframe_push_view(void *self, void *frame_scene, const void *params) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    static uint32_t overlay_track_logs = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    void *caller = __builtin_return_address(0);
    if (!self || !frame_scene || !params) {
        if (count <= 8U || (count % 256U) == 0U) {
            char extra[192];
            char caller_desc[96];
            describe_return_address(caller, caller_desc, sizeof(caller_desc));
            snprintf(extra,
                     sizeof(extra),
                     "caller=%s self=%p frame=%p params=%p skipped=1",
                     caller_desc,
                     self,
                     frame_scene,
                     params);
            log_diag_counter("RenderFrame::PushView", count, first_ms, extra);
        }
        return NULL;
    }

    const int from_overlay =
        return_address_in_symbol(caller,
                                 g_renderoverlay_update_render_thread_addr,
                                 g_renderoverlay_update_render_thread_size);
    if (from_overlay && self && g_overlay_render_frame != (uintptr_t)self) {
        g_overlay_render_frame = (uintptr_t)self;
        overlay_track_logs++;
        if (overlay_track_logs <= 8U) {
            l_info("Patch: tracking RenderOverlay frame %p for post-scene filtering.", self);
        }
    }

    void *view = SO_CONTINUE(void *, g_hook_renderframe_push_view, self, frame_scene, params);
    if (count <= 8U || (count % 256U) == 0U) {
        char extra[224];
        char caller_desc[96];
        describe_return_address(caller, caller_desc, sizeof(caller_desc));
        snprintf(extra,
                 sizeof(extra),
                 "caller=%s self=%p frame=%p params=%p ret=%p",
                 caller_desc,
                 self,
                 frame_scene,
                 params,
                 view);
        log_diag_counter("RenderFrame::PushView", count, first_ms, extra);
    }
    return view;
}

static void *hook_renderobject_viewport_prepare_view(void *self, void *frame_scene, void *target_ctx) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (!self || !frame_scene) {
        if (count <= 8U || (count % 128U) == 0U) {
            char extra[128];
            snprintf(extra,
                     sizeof(extra),
                     "self=%p frame=%p rtctx=%p skipped=1",
                     self,
                     frame_scene,
                     target_ctx);
            log_diag_counter("RenderObject_Viewport::PrepareView", count, first_ms, extra);
        }
        return NULL;
    }

    void *view = SO_CONTINUE(void *, g_hook_viewport_prepare_view, self, frame_scene, target_ctx);
    if (count <= 8U || (count % 128U) == 0U || !view) {
        const uint32_t *u32 = (const uint32_t *)self;
        void *scene_ptr = self ? (void *)(uintptr_t)u32[0x08 / 4] : NULL;
        void *camera_slot = self ? (void *)(uintptr_t)u32[0x10 / 4] : NULL;
        char extra[192];

        snprintf(extra,
                 sizeof(extra),
                 "self=%p frame=%p rtctx=%p scene=%p camera_slot=%p ret=%p",
                 self,
                 frame_scene,
                 target_ctx,
                 scene_ptr,
                 camera_slot,
                 view);
        log_diag_counter("RenderObject_Viewport::PrepareView", count, first_ms, extra);
    }

    return view;
}

static void hook_rendertexture_prepare_view(void *self, void *frame_scene, void *target_ctx,
                                            void *render_scene_ctx, int index) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (!self || !frame_scene) {
        if (count <= 8U || (count % 256U) == 0U) {
            char extra[128];
            snprintf(extra,
                     sizeof(extra),
                     "self=%p frame=%p rtctx=%p scene_ctx=%p index=%d skipped=1",
                     self,
                     frame_scene,
                     target_ctx,
                     render_scene_ctx,
                     index);
            log_diag_counter("RenderTexture::PrepareView", count, first_ms, extra);
        }
        return;
    }

    if (count <= 8U || (count % 256U) == 0U) {
        char extra[160];
        const uint32_t *u32 = (const uint32_t *)self;
        void *camera_slot = self ? (void *)(uintptr_t)u32[0x14 / 4] : NULL;
        snprintf(extra,
                 sizeof(extra),
                 "self=%p frame=%p rtctx=%p scene_ctx=%p index=%d camera_slot=%p width=%u",
                 self,
                 frame_scene,
                 target_ctx,
                 render_scene_ctx,
                 index,
                 camera_slot,
                 self ? u32[0x38 / 4] : 0U);
        log_diag_counter("RenderTexture::PrepareView", count, first_ms, extra);
    }

    SO_CONTINUE_VOID(g_hook_rendertexture_prepare_view, self, frame_scene, target_ctx, render_scene_ctx, index);
}

/* 2026-07-02 (3rd pass, part 2): SAVETRACE proved the Lua-level save chain
 * (SaveLoadPreSave/Create/Save/SaveLoadPostSave) all ENTER+RETURN cleanly,
 * yet the whole session produced ZERO filesystem writes -- not even routine
 * prefs saves. Disasm of the native `HandleObjectInfo::QuickSave()` (what
 * `luaSave` ultimately calls) shows it reads a "ResourceConcreteLocation"
 * pointer at `this+28`; if that pointer is NULL, QuickSave takes an early
 * exit branch and returns WITHOUT ever calling the real serialize/write
 * operation (`Meta::MetaOperation_Save`). This hook logs that pointer on
 * every call to confirm/refute the theory that resources created under the
 * `<User>` logical location never get a real disk location bound. */
static so_hook g_hook_handleobjectinfo_quicksave;
static int hook_handleobjectinfo_quicksave(void *self) {
    static uint32_t count = 0;
    count++;
    const uint32_t *u32_in = (const uint32_t *)self;
    void *location_before = self ? (void *)(uintptr_t)u32_in[28 / 4] : NULL;
    l_info("SAVETRACE: QuickSave ENTER #%u self=%p location=%p",
           count, self, location_before);
    int ret = SO_CONTINUE(int, g_hook_handleobjectinfo_quicksave, self);
    const uint32_t *u32_out = (const uint32_t *)self;
    void *location_after = self ? (void *)(uintptr_t)u32_out[28 / 4] : NULL;
    l_info("SAVETRACE: QuickSave RETURN #%u self=%p location=%p ret=%d",
           count, self, location_after, ret);
    return ret;
}

/* 2026-07-02 (3rd pass, part 3): QuickSave's location pointer is confirmed
 * NULL on EVERY call (device log). One level up: `Platform_Android::
 * GetBaseUserDirectoryEv` resolves the base "<User>" directory via
 * SDL_AndroidGetJNIEnv + a JNI FindClass/GetStaticMethodID/
 * CallStaticObjectMethod chain into libSDL2.so's own Android JNI bridge
 * (NOT our java.c method table directly -- SDL2 has its own hardcoded
 * class/method expectations). If that chain returns empty/garbage on our
 * FalsoJNI stub, the whole `<User>` resource location would never resolve
 * to a real directory, explaining why every resource under it gets a NULL
 * ResourceConcreteLocation. Itanium ABI: a function returning a non-trivial
 * class by value takes the caller-allocated return slot as arg0, so this
 * hook's first param is the destination `String` object, second is `this`.
 * Telltale's String layout (from disasm of String::String(const char*)):
 * the object holds a pointer to a heap block laid out as
 * [refcount@0][capacity@4][length@8][chars@12...]. Read-only, defensive
 * null/bounds checks throughout -- this only runs once or a few times at
 * boot, not a hot path. */
/* CORRECTED (2026-07-02, 4th pass): device log showed length=1935895855 and
 * garbage chars -- proof the read was wrong, not proof the string itself was
 * garbage. Re-disassembled String::String(const char*) fully: the object's
 * single field stores a pointer DIRECTLY TO THE CHARACTER DATA (chars_ptr),
 * not to the allocation base -- `str r3,[r6]` where r3 = block+12 (the data
 * start returned by memcpy). The refcount/capacity/length header therefore
 * sits at NEGATIVE offsets from that pointer: length is `*(chars_ptr - 4)`,
 * not `*(block_ptr + 8)`. Fixed the diagnostic read accordingly.
 *
 * Per the user's request: don't just diagnose, force it. Rather than keep
 * chasing why libSDL2.so's internal JNI bridge resolves this on our FalsoJNI
 * stub, directly replace the result with a String built from a directory we
 * fully control (DATA_PATH "User/", already created by
 * ensure_runtime_directories() in init.c). Built via the engine's own
 * `String::String(const char*)` constructor so allocation/refcounting stays
 * consistent with this build's allocator -- we only splice the resulting
 * data pointer into the caller's return slot, we don't hand-roll the heap
 * block ourselves. NOTE (6th pass): the override itself was removed, see
 * below -- kept the read-only diagnostic. */
static so_hook g_hook_platform_android_get_base_user_directory;
static void *hook_platform_android_get_base_user_directory(void *out_str, void *this_ptr) {
    void *ret = SO_CONTINUE(void *, g_hook_platform_android_get_base_user_directory, out_str, this_ptr);
    if (out_str) {
        uint32_t chars_ptr = *(uint32_t *)out_str;
        char safe[161];
        uint32_t length = 0;
        if (chars_ptr && chars_ptr < 0x90000000u) {
            length = *(uint32_t *)(uintptr_t)(chars_ptr - 4);
            const char *chars = (const char *)(uintptr_t)chars_ptr;
            uint32_t n = length < 160U ? length : 160U;
            uint32_t i;
            for (i = 0; i < n; ++i) {
                char c = chars[i];
                safe[i] = (c >= 32 && c < 127) ? c : '.';
            }
            safe[i] = '\0';
        } else {
            safe[0] = '\0';
        }
        l_info("SAVETRACE: GetBaseUserDirectory this=%p out=%p chars_ptr=0x%08X length=%u chars='%s'",
               this_ptr, out_str, chars_ptr, length, safe);
        /* 2026-07-02 (6th pass): REMOVED the forced override. SAVEREDIRECT2's
         * device log proved it was actively harmful: <Temp> is apparently
         * defined RELATIVE to this same base-directory string, so forcing it
         * to DATA_PATH "User/" shifted <Temp> to nest under it too, and every
         * save wrote to the doubled path "ux0:data/mcsm/User/Temp/..."
         * instead of the known-good "ux0:data/mcsm/Temp/...". <Temp> writes
         * were ALREADY proven working (SAVEIO logs) in sessions before this
         * hook ever existed, using whatever this function originally
         * returned -- leave it alone and let <Temp> resolve exactly as it
         * always did. This hook is now read-only diagnostic (kept in case
         * `<User>`-relative behavior ever needs inspecting again). The real
         * fix is entirely in redirect_logical_user_to_temp(). */
    } else {
        l_info("SAVETRACE: GetBaseUserDirectory this=%p out=NULL", this_ptr);
    }
    return ret;
}

static void hook_switch_default_render_target(const void *clear) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;

    count++;
    if (!first_ms) {
        first_ms = now_ms();
    }

    if (count <= 8U || (count % 128U) == 0U) {
        char extra[96];
        const uint32_t *u32 = (const uint32_t *)clear;
        snprintf(extra,
                 sizeof(extra),
                 "clear=%p rgba=%08X/%08X/%08X/%08X",
                 clear,
                 clear ? u32[0] : 0U,
                 clear ? u32[1] : 0U,
                 clear ? u32[2] : 0U,
                 clear ? u32[3] : 0U);
        log_diag_counter("RenderDevice::SwitchDefaultRenderTarget", count, first_ms, extra);
    }

    SO_CONTINUE_VOID(g_hook_switch_default_render_target, clear);
}

typedef int (*allocate_gl_buffer_fn)(unsigned int buffer,
                                     unsigned int target,
                                     unsigned int size,
                                     const void *data,
                                     unsigned int usage);

static GLenum patch_drain_gl_errors(void) {
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

static allocate_gl_buffer_fn resolve_allocate_gl_buffer(void) {
    static allocate_gl_buffer_fn fn = NULL;
    static int resolved = 0;

    if (!resolved) {
        resolved = 1;
        fn = (allocate_gl_buffer_fn)so_symbol(&so_mod_gameengine, "_ZN12RenderDevice16AllocateGLBufferEjjjPKvj");
        if (!fn) {
            l_error("Patch: RenderDevice::AllocateGLBuffer symbol not found.");
        }
    }

    return fn;
}

static int upload_cpu_buffer(unsigned int buffer,
                             unsigned int target,
                             size_t size,
                             const void *data,
                             const char *label) {
    allocate_gl_buffer_fn alloc = resolve_allocate_gl_buffer();
    if (!alloc) {
        return 0;
    }

    if (!data || size == 0) {
        return 1;
    }

    if (size > UINT32_MAX) {
        l_error("Patch: %s upload too large (%u bytes).", label, (unsigned)size);
        return 0;
    }

    static uint32_t s_upload_count = 0;
    s_upload_count++;
    GLenum pre_err = patch_drain_gl_errors();
    int ret = alloc(buffer, target, (unsigned int)size, data, GL_STREAM_DRAW);
    GLenum err = glGetError();
    if (s_upload_count <= 32U || err != GL_NO_ERROR || pre_err != GL_NO_ERROR) {
        l_info("Patch: %s AllocateGLBuffer #%u buffer=%u target=0x%X size=%u data=%p ret=%d pre=0x%X err=0x%X",
               label,
               (unsigned)s_upload_count,
               buffer,
               target,
               (unsigned)size,
               data,
               ret,
               (unsigned)pre_err,
               (unsigned)err);
    }
    return 1;
}

static int hook_vertexbuffer_platform_lock(void *self, int read_only) {
    (void)read_only;
    if (!self) {
        return 0;
    }

    uint32_t *u32 = (uint32_t *)self;
    uint32_t elem_count = u32[0xc0 / 4];
    uint32_t mode = u32[0xd4 / 4];
    uint32_t lock_count = u32[0xe0 / 4];
    void *ptr = (void *)(uintptr_t)u32[0xd0 / 4];

    if (elem_count == 0) {
        return 0;
    }

    if (mode == 2) {
        if (ptr) {
            lock_count += 1;
            u32[0xe0 / 4] = lock_count;
        }
        return lock_count > 0;
    }

    if (lock_count != 0) {
        if (ptr) {
            lock_count += 1;
            u32[0xe0 / 4] = lock_count;
        }
        return lock_count > 0;
    }

    if (!ptr) {
        uint32_t stride = u32[0xc4 / 4];
        size_t bytes = (size_t)elem_count * (size_t)stride;
        if (bytes == 0) {
            return 0;
        }

        ptr = malloc_soloader(bytes);
        if (!ptr) {
            l_error("Patch: T3VertexBuffer::PlatformLock fallback malloc failed (%u bytes).", (unsigned)bytes);
            return 0;
        }

        u32[0xd0 / 4] = (uint32_t)(uintptr_t)ptr;
        /* Throttled: this fired 7420x/run (the #1 log-spam line) — synchronous
         * file writes that pile up during the already-slow scene loads. */
        static unsigned int s_vb_log = 0;
        if (s_vb_log++ < 16u)
            l_info("Patch: T3VertexBuffer::PlatformLock allocated CPU vertex buffer (%u bytes).", (unsigned)bytes);
    }

    u32[0xe0 / 4] = 1;
    return 1;
}

static int hook_indexbuffer_platform_lock(void *self, int read_only) {
    (void)read_only;
    if (!self) {
        return 0;
    }

    uint32_t *u32 = (uint32_t *)self;

    // Offsets inferred from libGameEngine::T3IndexBuffer::PlatformLock(bool)
    uint32_t count = u32[0x2c / 4];
    if (count == 0) {
        return 0;
    }

    uint32_t lock_count = u32[0x24 / 4] + 1;
    u32[0x24 / 4] = lock_count;
    if (lock_count > 1) {
        return 1;
    }

    void *ptr = (void *)(uintptr_t)u32[0x3c / 4];
    if (!ptr) {
        uint32_t stride = u32[0x30 / 4];
        size_t bytes = (size_t)count * (size_t)stride;
        if (bytes == 0) {
            bytes = 2 * 1024;
        }
        ptr = malloc_soloader(bytes);
        u32[0x3c / 4] = (uint32_t)(uintptr_t)ptr;
        if (!ptr) {
            l_error("Patch: T3IndexBuffer::PlatformLock fallback malloc failed (%u bytes).", (unsigned)bytes);
            return 0;
        }
        l_info("Patch: T3IndexBuffer::PlatformLock allocated CPU index buffer (%u bytes).", (unsigned)bytes);
    }

    return 1;
}

static int hook_vertexbuffer_platform_unlock(void *self) {
    if (!self) {
        return 0;
    }

    uint32_t *u32 = (uint32_t *)self;
    uint32_t mode = u32[0xd4 / 4];
    uint32_t lock_count = u32[0xe0 / 4];

    if (mode == 2 || lock_count != 1) {
        if (lock_count > 0) {
            lock_count -= 1;
            u32[0xe0 / 4] = lock_count;
        }
        return lock_count == 0;
    }

    glBindBuffer(GL_ARRAY_BUFFER, u32[0x20 / 4]);

    void *ptr = (void *)(uintptr_t)u32[0xd0 / 4];
    const size_t bytes = (size_t)u32[0xc0 / 4] * (size_t)u32[0xc4 / 4];
    if (!upload_cpu_buffer(u32[0x20 / 4], GL_ARRAY_BUFFER, bytes, ptr, "T3VertexBuffer")) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return 0;
    }

    if (ptr) {
        free_soloader(ptr);
    }

    u32[0xd0 / 4] = 0;
    u32[0xe0 / 4] = 0;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 1;
}

static int hook_indexbuffer_platform_unlock(void *self) {
    if (!self) {
        return 0;
    }

    uint32_t *u32 = (uint32_t *)self;
    if (u32[0x20 / 4] == 0) {
        return 0;
    }

    uint32_t lock_count = u32[0x24 / 4];
    if (lock_count > 0) {
        lock_count -= 1;
        u32[0x24 / 4] = lock_count;
    }

    if (lock_count > 0) {
        return 0;
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, u32[0x20 / 4]);

    const void *ptr = (const void *)(uintptr_t)u32[0x3c / 4];
    const size_t bytes = (size_t)u32[0x2c / 4] * (size_t)u32[0x30 / 4];
    if (!upload_cpu_buffer(u32[0x20 / 4], GL_ELEMENT_ARRAY_BUFFER, bytes, ptr, "T3IndexBuffer")) {
        return 0;
    }

    return 1;
}

static void patch_vertexbuffer_platform_lock(void) {
    so_hook hook;
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN14T3VertexBuffer12PlatformLockEb",
                              "T3VertexBuffer::PlatformLock",
                              (uintptr_t)&hook_vertexbuffer_platform_lock,
                              &hook);
}

static void patch_vertexbuffer_platform_unlock(void) {
    so_hook hook;
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN14T3VertexBuffer14PlatformUnlockEv",
                              "T3VertexBuffer::PlatformUnlock",
                              (uintptr_t)&hook_vertexbuffer_platform_unlock,
                              &hook);
}

static void patch_indexbuffer_platform_lock(void) {
    so_hook hook;
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN13T3IndexBuffer12PlatformLockEb",
                              "T3IndexBuffer::PlatformLock",
                              (uintptr_t)&hook_indexbuffer_platform_lock,
                              &hook);
}

static void patch_indexbuffer_platform_unlock(void) {
    so_hook hook;
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN13T3IndexBuffer14PlatformUnlockEv",
                              "T3IndexBuffer::PlatformUnlock",
                              (uintptr_t)&hook_indexbuffer_platform_unlock,
                              &hook);
}

static void patch_engine_diag_hooks(void) {
    init_metrics_diag();
    init_render_gate_diag();
    force_native_render_dimensions("patch");
    force_animation_runtime_flags("patch");
    patch_chore_full_update_path();

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN7Metrics8NewFrameEf",
                              "Metrics::NewFrame",
                              (uintptr_t)&hook_metrics_new_frame,
                              &g_hook_metrics_new_frame);

    g_renderframe_push_view_addr =
        so_symbol(&so_mod_gameengine, "_ZN11RenderFrame8PushViewER16RenderFrameSceneRK16RenderViewParams");
    g_renderframe_push_view_size =
        so_symbol_size(&so_mod_gameengine, "_ZN11RenderFrame8PushViewER16RenderFrameSceneRK16RenderViewParams");
    g_renderoverlay_update_render_thread_addr =
        so_symbol(&so_mod_gameengine, "_ZN13RenderOverlay18UpdateRenderThreadER11RenderFrameR21T3RenderTargetContextfbf");
    g_renderoverlay_update_render_thread_size =
        so_symbol_size(&so_mod_gameengine, "_ZN13RenderOverlay18UpdateRenderThreadER11RenderFrameR21T3RenderTargetContextfbf");

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "GameEngine_Start",
                              "GameEngine_Start",
                              (uintptr_t)&hook_gameengine_start,
                              &g_hook_gameengine_start);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "GameEngine_Loop",
                              "GameEngine_Loop",
                              (uintptr_t)&hook_gameengine_loop,
                              &g_hook_gameengine_loop);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameEngine6RenderEv",
                              "GameEngine::Render",
                              (uintptr_t)&hook_gameengine_render,
                              &g_hook_gameengine_render);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderDevice10BeginFrameEv",
                              "RenderDevice::BeginFrame",
                              (uintptr_t)&hook_renderdevice_begin_frame,
                              &g_hook_render_begin_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderDevice8EndFrameEv",
                              "RenderDevice::EndFrame",
                              (uintptr_t)&hook_renderdevice_end_frame,
                              &g_hook_render_end_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderDevice7PresentEv",
                              "RenderDevice::Present",
                              (uintptr_t)&hook_renderdevice_present,
                              &g_hook_render_present);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN15Application_SDL4SwapEv",
                              "Application_SDL::Swap",
                              (uintptr_t)&hook_application_sdl_swap,
                              &g_hook_app_swap);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameEngine18SetSuspendGameLoopEb",
                              "GameEngine::SetSuspendGameLoop",
                              (uintptr_t)&hook_suspend_game_loop,
                              &g_hook_suspend_game_loop);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN18PlaybackController25UpdatePlaybackControllersEff",
                              "PlaybackController::UpdatePlaybackControllers",
                              (uintptr_t)&hook_playback_controller_update,
                              &g_hook_playback_controller_update);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameRender11RenderFrameEv",
                              "GameRender::RenderFrame",
                              (uintptr_t)&hook_gamerender_render_frame,
                              &g_hook_gamerender_render_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameRender11RenderSceneER18RenderSceneContextRK16RenderParameters",
                              "GameRender::RenderScene",
                              (uintptr_t)&hook_gamerender_render_scene,
                              &g_hook_gamerender_render_scene);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderThread18SubmitCurrentFrameEv",
                              "RenderThread::SubmitCurrentFrame",
                              (uintptr_t)&hook_renderthread_submit_current_frame,
                              &g_hook_renderthread_submit_current_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderThread8EndFrameEv",
                              "RenderThread::EndFrame",
                              (uintptr_t)&hook_renderthread_end_frame,
                              &g_hook_renderthread_end_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderThread11FinishFrameEv",
                              "RenderThread::FinishFrame",
                              (uintptr_t)&hook_renderthread_finish_frame,
                              &g_hook_renderthread_finish_frame);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN11RenderFrame7ExecuteEPS_",
                              "RenderFrame::Execute",
                              (uintptr_t)&hook_renderframe_execute,
                              &g_hook_renderframe_execute);

#if ENABLE_HOT_RENDER_VIEW_DIAG_HOOKS
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN11RenderFrame8PushViewER16RenderFrameSceneRK16RenderViewParams",
                              "RenderFrame::PushView",
                              (uintptr_t)&hook_renderframe_push_view,
                              &g_hook_renderframe_push_view);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16RenderFrameScene12AllocateViewERK16RenderViewParams",
                              "RenderFrameScene::AllocateView",
                              (uintptr_t)&hook_renderframe_allocate_view,
                              &g_hook_renderframe_allocate_view);
#else
    l_info("Patch: skipped hot RenderFrame view diagnostics; latest core pointed at this render path.");
#endif

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN21RenderObject_Viewport11PrepareViewER16RenderFrameSceneR21T3RenderTargetContext",
                              "RenderObject_Viewport::PrepareView",
                              (uintptr_t)&hook_renderobject_viewport_prepare_view,
                              &g_hook_viewport_prepare_view);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN13RenderTexture11PrepareViewER16RenderFrameSceneR21T3RenderTargetContextP18RenderSceneContexti",
                              "RenderTexture::PrepareView",
                              (uintptr_t)&hook_rendertexture_prepare_view,
                              &g_hook_rendertexture_prepare_view);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16HandleObjectInfo9QuickSaveEv",
                              "HandleObjectInfo::QuickSave",
                              (uintptr_t)&hook_handleobjectinfo_quicksave,
                              &g_hook_handleobjectinfo_quicksave);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16Platform_Android20GetBaseUserDirectoryEv",
                              "Platform_Android::GetBaseUserDirectory",
                              (uintptr_t)&hook_platform_android_get_base_user_directory,
                              &g_hook_platform_android_get_base_user_directory);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN12RenderDevice25SwitchDefaultRenderTargetERK13T3RenderClear",
                              "RenderDevice::SwitchDefaultRenderTarget",
                              (uintptr_t)&hook_switch_default_render_target,
                              &g_hook_switch_default_render_target);

    /* RenderOverlay::UpdateRenderThread has float/bool/float tail args.
     * The engine .so was compiled with softfp (Android NDK armeabi-v7a
     * default: floats in integer regs), but the Vita loader compiles
     * with hard-float (floats in VFP regs).  Any hook here would receive
     * scrambled register content and corrupt the overlay pass.
     *
     * Instead we yield the render thread during boot inside
     * hook_renderframe_execute (which is int-returning, no floats)
     * and hook_gamerender_render_scene. */
    l_info("Patch: skipping RenderOverlay::UpdateRenderThread hook (softfp/hard-float ABI mismatch would corrupt args).");
}

/* TEXT INPUT -> VITA KEYBOARD (2026-07-18): the engine requests the on-screen
 * keyboard through SDL's Android_JNI_ShowTextInput (what SDL_StartTextInput calls
 * on Android). On Vita that's a JNI stub = nothing appears. Intercept it and raise
 * the Vita IME so ANY text field (save rename, etc.) gets a keyboard. */
static so_hook g_hook_sdl_show_text_input;
static void hook_sdl_show_text_input(void *inputRect) {
    extern void mcsm_ime_begin(const char *initial);
    l_info("KEYBOARD: Android_JNI_ShowTextInput -> Vita IME");
    mcsm_ime_begin("");
    (void)inputRect;
}

static void patch_sdl_android_runtime_hooks(void) {
    (void)hook_symbol_checked(&so_mod_sdl2,
                              "Android_JNI_PollInputDevices",
                              "Android_JNI_PollInputDevices",
                              (uintptr_t)&hook_android_jni_poll_input_devices,
                              &g_hook_android_jni_poll_input_devices);

    (void)hook_symbol_checked(&so_mod_sdl2,
                              "Android_PumpEvents",
                              "Android_PumpEvents",
                              (uintptr_t)&hook_android_pump_events,
                              &g_hook_android_pump_events);

    (void)hook_symbol_checked(&so_mod_sdl2,
                              "SDL_WaitEvent_REAL",
                              "SDL_WaitEvent_REAL stackless no-event",
                              (uintptr_t)&hook_sdl_wait_event_real,
                              &g_hook_sdl_wait_event_real);

    (void)hook_symbol_checked(&so_mod_sdl2,
                              "SDL_WaitEventTimeout_REAL",
                              "SDL_WaitEventTimeout_REAL stackless no-event",
                              (uintptr_t)&hook_sdl_wait_event_timeout_real,
                              &g_hook_sdl_wait_event_timeout_real);
    (void)hook_symbol_checked(&so_mod_sdl2,
                              "Android_JNI_ShowTextInput",
                              "Android_JNI_ShowTextInput -> Vita IME",
                              (uintptr_t)&hook_sdl_show_text_input,
                              &g_hook_sdl_show_text_input);
}

static void patch_input_diag_hooks(void) {
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN15Application_SDL11OnFingeringEN11InputMapper9EventTypeERK20SDL_TouchFingerEvent",
                              "Application_SDL::OnFingering",
                              (uintptr_t)&hook_application_sdl_on_fingering,
                              &g_hook_app_on_fingering);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN15Application_SDL12OnMouseEventEN11InputMapper9EventTypeERK20SDL_MouseButtonEvent",
                              "Application_SDL::OnMouseEvent",
                              (uintptr_t)&hook_application_sdl_on_mouse_event,
                              &g_hook_app_on_mouse_event);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN19GameWindow_PlayMode11OnMouseMoveEjiii",
                              "GameWindow_PlayMode::OnMouseMove",
                              (uintptr_t)&hook_gamewindow_playmode_mouse_move,
                              &g_hook_gamewindow_mouse_move);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN19GameWindow_PlayMode11OnMouseDownEjiii",
                              "GameWindow_PlayMode::OnMouseDown",
                              (uintptr_t)&hook_gamewindow_playmode_mouse_down,
                              &g_hook_gamewindow_mouse_down);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN19GameWindow_PlayMode9OnMouseUpEjiii",
                              "GameWindow_PlayMode::OnMouseUp",
                              (uintptr_t)&hook_gamewindow_playmode_mouse_up,
                              &g_hook_gamewindow_mouse_up);

    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16TouchScreenState24SetLegacyPointerPositionERK8Vector2I",
                              "TouchScreenState::SetLegacyPointerPosition",
                              (uintptr_t)&hook_touchscreenstate_set_legacy_pointer,
                              &g_hook_touch_set_legacy_pointer);
}

// ---- Boot / resource / job-system diagnostic hooks ----
// These trace the post-init boot path (Lua, resource descriptions, async job
// scheduler) to localize where the boot->menu transition stalls on Vita.
// All hooks uniformly return int (capturing r0), which is safe for both void
// and value-returning targets since the value fits in r0.
static so_hook g_hook_scriptmgr_init;
static so_hook g_hook_lua_register_resdesc;
static so_hook g_hook_lua_retry_resdesc;
static so_hook g_hook_job_init;
// NOTE: JobScheduler::_EnqueueJob / _ProcessJob run concurrently on ~15 worker
// threads. The SO_CONTINUE hook mechanism restores+repatches shared code bytes,
// which is NOT thread-safe and corrupts the function under concurrency (observed
// as a Data abort at _ProcessJob entry with 0xdeadbeef registers). Do NOT hook
// concurrent functions this way - those hooks were removed.

static int hook_scriptmgr_init(int a, int b) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    if (!first_ms) first_ms = now_ms();
    char extra[32];
    snprintf(extra, sizeof(extra), "a=%d b=%d", a, b);
    log_diag_counter("ScriptManager::Initialize", count, first_ms, extra);
    return SO_CONTINUE(int, g_hook_scriptmgr_init, a, b);
}

static int hook_lua_register_resdesc(void *L) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    if (!first_ms) first_ms = now_ms();
    launch_state_mark_progress();
    if (should_log_diag_count(count)) {
        log_diag_counter("luaRegisterResourceDescriptionWithEngine", count, first_ms, NULL);
    }
    return SO_CONTINUE(int, g_hook_lua_register_resdesc, L);
}

static int hook_lua_retry_resdesc(void *L) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    if (!first_ms) first_ms = now_ms();
    launch_state_mark_progress();
    // Always log: if this fires repeatedly, resource descriptions are failing to
    // load and the boot is stuck retrying them.
    log_diag_counter("luaResourceRetryFailedResDescs", count, first_ms, "RESDESC RETRY/FAIL");
    return SO_CONTINUE(int, g_hook_lua_retry_resdesc, L);
}

// Resource-set lifecycle tracers. These are Lua C functions invoked by the boot
// scripts on the main Lua thread (low frequency) so SO_CONTINUE is safe here.
// They reveal exactly where the boot->menu resource flow stops: sets created?
// the boot/menu set enabled (which triggers its load)? async loads requested?
#define BOOT_LUA_TRACER(fn_name, label, hook_var)                       \
    static so_hook hook_var;                                            \
    static int fn_name(void *L) {                                      \
        static uint32_t count = 0;                                     \
        static uint64_t first_ms = 0;                                 \
        count++;                                                       \
        if (!first_ms) first_ms = now_ms();                           \
        launch_state_mark_progress();                                 \
        log_diag_counter(label, count, first_ms, NULL);               \
        LS_TICK();                                                     \
        return SO_CONTINUE(int, hook_var, L);                         \
    }

BOOT_LUA_TRACER(hook_lua_loadasync, "lua_LoadAsync", g_hook_lua_loadasync)
BOOT_LUA_TRACER(hook_lua_preloadasync, "luaPreloadAsync", g_hook_lua_preloadasync)
BOOT_LUA_TRACER(hook_lua_rset_create, "luaResourceSetCreate", g_hook_lua_rset_create)
BOOT_LUA_TRACER(hook_lua_rset_enable, "luaResourceSetEnable", g_hook_lua_rset_enable)
BOOT_LUA_TRACER(hook_lua_rset_loadingcall, "luaResourceSetLoadingCall", g_hook_lua_rset_loadingcall)

// Forward declaration: LOGIN_BYPASS macros below use lua_push_forced_bool
// which is defined later (after the ChorePlay/scene tracers that need
// LuaToLStringFn etc.).
static int lua_push_forced_bool(void *L, int value, const char *label);
static int lua_push_forced_integer(void *L, int value, const char *label);

// Login/connect/cloud-sync bypass (2026-06-21). The Telltale authentication
// server (services.telltalegames.com) is permanently offline.  Every one of
// these primitives tries real network I/O through the un-replaced
// UNetworkAPI::* C++ methods, which block on dead sockets until their ~30s
// timeout.  With luaPlatformIsUserSignedIn already forced to true the engine
// does NOT need a password or a remote credential — it can proceed with a
// local-offline user.  Bypass the originals entirely: return 0 Lua values
// immediately.  All are low-frequency main-thread Lua calls.
#define LOGIN_BYPASS(fn_name, label, hook_var)                          \
    static so_hook hook_var;                                            \
    static int fn_name(void *L) {                                       \
        (void)L;                                                        \
        static uint32_t count = 0;                                      \
        static uint64_t first_ms = 0;                                   \
        count++;                                                         \
        if (!first_ms) first_ms = now_ms();                             \
        launch_state_mark_progress();                                   \
        if (count <= 8U || (count & 0x3FU) == 0U) {                     \
            l_info("Login bypass: %s -> 0 values (offline) count=%u",   \
                   label, count);                                       \
        }                                                               \
        return lua_push_forced_bool(L, 1, label) >= 0 ? 1 : 0;         \
    }

LOGIN_BYPASS(hook_lua_show_password_box, "luaShowPasswordBox", g_hook_lua_show_password_box)
LOGIN_BYPASS(hook_lua_is_password_box_finished, "luaIsPasswordBoxFinished", g_hook_lua_is_password_box_finished)
LOGIN_BYPASS(hook_lua_get_password_box_results, "luaGetPasswordBoxResults", g_hook_lua_get_password_box_results)
LOGIN_BYPASS(hook_lua_network_get_credential, "luaNetworkAPIGetCredential", g_hook_lua_network_get_credential)
LOGIN_BYPASS(hook_lua_session_log_process, "luaSessionLogProcess", g_hook_lua_session_log_process)
LOGIN_BYPASS(hook_lua_cloud_sync_userdata, "luaNetworkAPICloudSyncUserData", g_hook_lua_cloud_sync_userdata)
LOGIN_BYPASS(hook_lua_upload_cached, "luaUploadCachedObjectToServer", g_hook_lua_upload_cached)
LOGIN_BYPASS(hook_lua_upload_pending, "luaUploadPendingObjectsToServer", g_hook_lua_upload_pending)

/* Chore/scene-name tracers (2026-06-21). bootTitle stalls AFTER setting the
 * "Checking For DLC" text but BEFORE ConnectedContentManager_Update (its
 * luaPlatformIsConnectedToLicenseServer never fires) and before the login
 * primitives above. That window is driven by ChorePlay/ChorePlayAndWait/SceneOpen
 * on the ui_boot.scene UI. luaChorePlay and luaSceneOpen RETURN NORMALLY (safe to
 * SO_CONTINUE; unlike luaChorePlayAndWait which yields the coroutine -> never hook
 * that). Logging the chore/scene NAME arg: the last one before the watchdog
 * progress-freeze pinpoints where the UI coroutine stops. */
typedef const char *(*LuaToLStringFn)(void *L, int idx, size_t *len);
typedef int (*LuaGetTopFn)(void *L);
static LuaToLStringFn g_lua_tolstring_fast;
static LuaGetTopFn g_lua_gettop_fast;
/* 2026-07-02: a fixed READ-COUNT window is inherently racy. Menu_CharacterSelect
 * plays a "characterSelect" chore THREE distinct ways: (1) once immediately on
 * menu entry via Menu_CharacterSelect_Reset -> AppearanceSelect(kDefaultChoice)
 * -- long before the user does anything; (2) once per appearance thumbnail the
 * user browses while deciding; (3) via Complete()'s ChorePlayAndWait("..._hide.
 * chore") right before the ACTUAL Licensed read that must see false (not
 * hookable -- ChorePlayAndWait yields the coroutine). A budget of 1 read gets
 * consumed by whichever unrelated Licensed poll happens to fire first after
 * arm #1 (menu entry), so Complete()'s real read sees the TRUE (real) license
 * state -> takes the SubProject_Switch("Menu") branch instead of starting the
 * episode -> game never proceeds past character select. A budget of 16 (the
 * original) survived that noise but stayed armed long enough to also corrupt
 * the episode-start autosave (see [[mcsm-port-status]] 2026-07-02 save fix).
 *
 * Fix: use an ACTIVE FLAG, not a countdown -- every Licensed read sees false
 * for as long as we're inside the character-select flow (any number of
 * reads, matching the old 16-read robustness), but the flag is force-CLEARED
 * the moment SubProject_Switch is actually called (hook_lua_set_subproject),
 * which is the one Lua call BOTH exit branches of Complete() are guaranteed
 * to make (Menu_StartEpisode -> SubProject_StartEpisode, or the direct
 * SubProject_Switch("Menu",...) call) -- so the window can never leak into
 * whatever screen/save flow comes after. A frame-based safety cap covers the
 * case where the user backs out without confirming (Menu_Pop(), no subproject
 * switch at all). */
static volatile int g_character_select_license_active = 0;
static volatile uint32_t g_character_select_license_safety = 0;
#define CHARACTER_SELECT_LICENSE_SAFETY_MAX 4096u

static int string_contains_character_select(const char *name) {
    return name &&
        (strstr(name, "characterSelect") ||
         strstr(name, "CharacterSelect"));
}

static void mark_character_select_license_window(const char *source, const char *name) {
    if (!string_contains_character_select(name)) {
        return;
    }
    if (!g_character_select_license_active) {
        l_info("FULLGAME: scoped Licensed=false window ARMED from %s '%s'",
               source ? source : "?",
               name);
    }
    g_character_select_license_active = 1;
    g_character_select_license_safety = CHARACTER_SELECT_LICENSE_SAFETY_MAX;
}

/* Called from hook_lua_set_subproject: character select is done (either exit
 * branch calls a subproject switch) -- disarm so later screens/saves see the
 * real Licensed state again. */
static void clear_character_select_license_window(void) {
    if (g_character_select_license_active) {
        l_info("FULLGAME: scoped Licensed=false window CLEARED (subproject switch)");
    }
    g_character_select_license_active = 0;
    g_character_select_license_safety = 0;
}

static const char *trace_arg_str(void *L, int idx) {
    if (!g_lua_tolstring_fast)
        g_lua_tolstring_fast = (LuaToLStringFn)so_symbol(&so_mod_gameengine, "lua_tolstring");
    if (!g_lua_gettop_fast)
        g_lua_gettop_fast = (LuaGetTopFn)so_symbol(&so_mod_gameengine, "lua_gettop");
    if (!g_lua_tolstring_fast || !g_lua_gettop_fast) return NULL;
    if (g_lua_gettop_fast(L) < idx) return NULL;       /* no such arg */
    return g_lua_tolstring_fast(L, idx, NULL);          /* NULL if not a string */
}

static so_hook g_hook_lua_choreplay;
static int hook_lua_choreplay(void *L) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    if (count <= 24U || (count & 0x3FU) == 0U) {
        const char *nm = trace_arg_str(L, 1);
        l_info("Diag: luaChorePlay #%u chore='%s'", count, nm ? nm : "(non-string)");
        mark_character_select_license_window("chore", nm);
    } else {
        const char *nm = trace_arg_str(L, 1);
        mark_character_select_license_window("chore", nm);
    }
    return SO_CONTINUE(int, g_hook_lua_choreplay, L);
}

/* 2026-07-02 (5th pass) -- REAL FIX: redirect <User> saves to the PROVEN-
 * WORKING <Temp> resource location.
 *
 * Forcing GetBaseUserDirectory's return value (previous entry) did NOT fix
 * it: QuickSave's ResourceConcreteLocation pointer stayed NULL on every
 * call even with a valid, engine-constructed directory string spliced in.
 * That means <User>'s ResourceConcreteLocation binding does not go through
 * GetBaseUserDirectory at all (or not only through it) -- there is a
 * different/cached registration path we haven't found, and further blind
 * disassembly has diminishing returns.
 *
 * What IS proven to work: `logical:<Temp>/save.bundle` and
 * `logical:<Temp>/slot.bundle` (the demo-cache fallback path) DO write real
 * files to disk (confirmed via SAVEIO write logs in earlier sessions) --
 * <Temp> is a fully functional directory-backed resource location holding
 * multiple distinct filenames, not a single-file hack.
 *
 * SaveBundles.lua's `fix()` is the ONLY place that turns a bare save-bundle
 * name into a "logical:<User>/name" string, passed to exactly three native
 * calls: ResourceExists, Create, Load. Every other reference to the bundle
 * afterward (Save(bundle), BundleGetResource(bundle,...), etc.) uses either
 * the bare name or the already-resolved in-memory handle those three calls
 * returned -- so rewriting the string argument ONLY at these three
 * boundaries, from "logical:<User>/" to "logical:<Temp>/", should make the
 * entire save system transparently piggyback on the location we know works,
 * with no other code needing to change. */
static int redirect_logical_user_to_temp(void *L, int idx) {
    typedef int (*LuaTypeFn2)(void *L, int idx);
    typedef const char *(*LuaToLStringFn2)(void *L, int idx, size_t *len);
    typedef const char *(*LuaPushStringFn2)(void *L, const char *s);
    typedef void (*LuaReplaceFn2)(void *L, int idx);
    static LuaTypeFn2 s_type;
    static LuaToLStringFn2 s_tolstring;
    static LuaPushStringFn2 s_pushstring;
    static LuaReplaceFn2 s_replace;
    if (!s_type) s_type = (LuaTypeFn2)so_symbol(&so_mod_gameengine, "lua_type");
    if (!s_tolstring) s_tolstring = (LuaToLStringFn2)so_symbol(&so_mod_gameengine, "lua_tolstring");
    if (!s_pushstring) s_pushstring = (LuaPushStringFn2)so_symbol(&so_mod_gameengine, "lua_pushstring");
    if (!s_replace) s_replace = (LuaReplaceFn2)so_symbol(&so_mod_gameengine, "lua_replace");
    if (!s_type || !s_tolstring || !s_pushstring || !s_replace) {
        return 0;
    }
    if (s_type(L, idx) != 4 /* LUA_TSTRING */) {
        return 0;
    }
    const char *s = s_tolstring(L, idx, NULL);
    if (!s) {
        return 0;
    }
    static const char kPrefix[] = "logical:<User>/";
    const char *suffix = NULL; /* the part after any location prefix, or the whole bare name */
    if (strncmp(s, kPrefix, sizeof(kPrefix) - 1) == 0) {
        suffix = s + (sizeof(kPrefix) - 1);
    } else if (!strchr(s, '/') && !strchr(s, ':')) {
        /* Bare filename, no location qualifier at all. `Save(bundle)` /
         * `Save('_saveslot1_id.estore')` etc. reference the resource by its
         * bare name (from GetPreferences()[kBundle] or saveFileName()/
         * logFileName()'s output) -- these resolve through the engine's
         * resource-NAME map, which is apparently a SEPARATE entry from the
         * "logical:<Temp>/name"-qualified one our Create()/Load() redirect
         * creates (confirmed by device log: QuickSave on the bare-name
         * handle stayed NULL location even after the qualified name's
         * Create succeeded). Redirect these too, scoped tightly to the
         * save-system's own naming convention (saveslot* / _saveslot*) so
         * we don't touch unrelated bare-named resources elsewhere in the
         * game. */
        if (strncmp(s, "saveslot", 8) == 0 || strncmp(s, "saveSlot", 8) == 0 ||
            strncmp(s, "_saveslot", 9) == 0 || strncmp(s, "_saveSlot", 9) == 0 ||
            /* CROWD-CHOICE FIX (2026-07-17): the stats WRITE goes to
             * logical:<User>/choice.prop -> redirected to <Temp>, but ChoiceStats.lua
             * READS via bare ResourceExists('choice.prop')/PropertyGet, which was NOT
             * redirected -> read looked in the default location, never found the
             * written/shipped file -> stats never displayed. Redirect the bare read to
             * <Temp> too so read and write resolve to the SAME place. EXACT match
             * (strcmp) so unrelated names like module_dialog_choice.prop are untouched. */
            strcmp(s, "choice.prop") == 0 || strcmp(s, "choicestats.prop") == 0) {
            suffix = s;
        }
    }
    if (!suffix) {
        return 0;
    }
    char newpath[256];
    sceClibSnprintf(newpath, sizeof(newpath), "logical:<Temp>/%s", suffix);
    static uint32_t redirect_count = 0;
    redirect_count++;
    if (redirect_count <= 48U) {
        l_info("SAVEFIX2: redirecting '%s' -> '%s' (#%u)", s, newpath, redirect_count);
    }
    s_pushstring(L, newpath);
    s_replace(L, idx);
    return 1;
}

/* 2026-07-02 (3rd pass): with the license-window fix, character select and
 * ResetGame now behave correctly, but a full session log showed ZERO
 * filesystem writes of ANY kind (no fopen in 'w' mode, no write() syscall)
 * even though the player triggered an in-game autosave and the "saving"
 * icon stayed stuck on screen. That icon is a visual chore independent of
 * the actual save; the real question is where SaveMe()'s native call chain
 * (SaveLoadPreSave -> SaveBundle_Create/Save -> SaveGameToBundle -> Save ->
 * SaveLoadPostSave, all in SaveBundles.lua/SaveLoad.lua) stops. All of these
 * are low-frequency main-thread Lua C calls -- safe to SO_CONTINUE trace. */
#define SAVE_TRACE_HOOK(fn_name, label, hook_var)                       \
    static so_hook hook_var;                                           \
    static int fn_name(void *L) {                                      \
        static uint32_t count = 0;                                     \
        count++;                                                       \
        l_info("SAVETRACE: %s ENTER #%u", label, count);                \
        int ret = SO_CONTINUE(int, hook_var, L);                       \
        l_info("SAVETRACE: %s RETURN #%u", label, count);               \
        return ret;                                                    \
    }

SAVE_TRACE_HOOK(hook_lua_saveload_presave, "SaveLoadPreSave", g_hook_lua_saveload_presave)
SAVE_TRACE_HOOK(hook_lua_saveload_postsave, "SaveLoadPostSave", g_hook_lua_saveload_postsave)
SAVE_TRACE_HOOK(hook_lua_save_game_to_bundle, "SaveGameToBundle", g_hook_lua_save_game_to_bundle)
SAVE_TRACE_HOOK(hook_lua_set_save_finished_cb, "SetSaveFinishedCallback", g_hook_lua_set_save_finished_cb)

/* Save()/Create() are also called constantly for non-save resources (props,
 * configs) at boot -- trace only the ones whose arg string looks save-shaped
 * so this doesn't flood the log. */
static so_hook g_hook_lua_save;
static int hook_lua_save(void *L) {
    static uint32_t count = 0;
    redirect_logical_user_to_temp(L, 1);
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: Save('%s') ENTER #%u", nm ? nm : "?", count);
    }
    int ret = SO_CONTINUE(int, g_hook_lua_save, L);
    if (is_save_shaped) {
        l_info("SAVETRACE: Save('%s') RETURN #%u", nm ? nm : "?", count);
    }
    return ret;
}

static so_hook g_hook_lua_create;
static int hook_lua_create(void *L) {
    static uint32_t count = 0;
    redirect_logical_user_to_temp(L, 1);
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: Create('%s') ENTER #%u", nm ? nm : "?", count);
    }
    int ret = SO_CONTINUE(int, g_hook_lua_create, L);
    if (is_save_shaped) {
        l_info("SAVETRACE: Create('%s') RETURN #%u", nm ? nm : "?", count);
    }
    return ret;
}

/* SaveLoad_Copy / copyLog build "logical:<User>/<dst>" destination strings
 * for ResourceCopy (save-slot copy UI, event-log copy). Redirect both the
 * source (arg1, may be bare or qualified) and destination (arg2). */
static so_hook g_hook_lua_resource_copy;
static int hook_lua_resource_copy(void *L) {
    redirect_logical_user_to_temp(L, 1);
    redirect_logical_user_to_temp(L, 2);
    return SO_CONTINUE(int, g_hook_lua_resource_copy, L);
}

/* 2026-07-02 (production pass 2) -- SPLIT-BRAIN metadata fix. Device log
 * proof: the per-save data bundles write fine (checkpoint = 21685 bytes)
 * but saveslot1.bundle ALWAYS serializes as an empty 40-byte header, so
 * the slot metadata ("Latest Save" / "Latest Serial" / "Episode in
 * Progress", written via SaveLoad.lua getSlotMetadata ->
 * BundleGetResource("saveslot1.bundle", "metadata_slot.prop") with the
 * BARE prefs name) never reaches disk -> on relaunch GetLatestSave finds
 * no "Latest Save" -> Play starts from zero even though the save data
 * exists. Root: the bare name resolves to a DIFFERENT resource map entry
 * than the redirected "logical:<Temp>/saveslot1.bundle" one that Save()
 * serializes -- metadata goes into an orphan object, the real bundle
 * stays empty. Fix: apply the same name redirect to EVERY remaining
 * native call that takes a save-system resource NAME as its first arg
 * (the Bundle* family + Unload/ResourceDelete/QueryEventLog), so every
 * reference -- create, load, save, metadata access, delete, event-log
 * query -- resolves to the single <Temp>-backed entry. */
#define REDIRECT_ARG1_HOOK(fn_name, hook_var)                          \
    static so_hook hook_var;                                           \
    static int fn_name(void *L) {                                      \
        redirect_logical_user_to_temp(L, 1);                           \
        return SO_CONTINUE(int, hook_var, L);                          \
    }

REDIRECT_ARG1_HOOK(hook_lua_bundle_get_resource, g_hook_lua_bundle_get_resource)
REDIRECT_ARG1_HOOK(hook_lua_bundle_create_resource, g_hook_lua_bundle_create_resource)
REDIRECT_ARG1_HOOK(hook_lua_bundle_get_resources, g_hook_lua_bundle_get_resources)
REDIRECT_ARG1_HOOK(hook_lua_bundle_remove_resource, g_hook_lua_bundle_remove_resource)
REDIRECT_ARG1_HOOK(hook_lua_unload, g_hook_lua_unload)
REDIRECT_ARG1_HOOK(hook_lua_resource_delete, g_hook_lua_resource_delete)
REDIRECT_ARG1_HOOK(hook_lua_query_event_log, g_hook_lua_query_event_log)
/* CHOICES FIX (2026-07-20): the crowd choice.prop was redirected to <Temp> for
 * ResourceExists but NOT for PropertyExists/PropertyGet, so the file was FOUND
 * (=1) yet PARSED under the bare name (NULL location) -> empty stats screen after
 * a chapter. Redirect the bare read for PropertyExists too (whitelist-scoped to
 * choice.prop/choicestats.prop/saveslot*, so other keys are untouched). */
REDIRECT_ARG1_HOOK(hook_lua_property_exists, g_hook_lua_property_exists)

/* 2026-07-03 CHOICES FIX. Decompiled EventLog.lua: player choices are
 * recorded into an EVENT LOG (.estore) whose backing resource is created &
 * addressed as "logical:<User>/_saveslot1_id.estore" -- a location that does
 * not bind on this port (same root cause as the save bundles). The save-
 * bundle redirects didn't cover the EventLog native family, so choices went
 * to a dead <User> log and never persisted. Redirect these too:
 *   ResourceSetNonPurgable("logical:<User>/..._id.estore", v)  -> arg1
 *   DeleteAllEventsAfterEvent("_saveslot1_id.estore", ...)     -> arg1 (bare)
 *   EventLogCreate(logName, tags, "logical:<User>/..._id.estore", 32768) -> arg3
 * (CreateEventLogEvent/EventLogActive/EventLogRemove take the bare log NAME
 * with no location, so they resolve against whatever EventLogCreate bound --
 * fixing EventLogCreate's resource arg fixes the whole chain.) */
REDIRECT_ARG1_HOOK(hook_lua_resource_set_nonpurgable, g_hook_lua_resource_set_nonpurgable)
REDIRECT_ARG1_HOOK(hook_lua_delete_all_events_after, g_hook_lua_delete_all_events_after)

static so_hook g_hook_lua_event_log_create;
static int hook_lua_event_log_create(void *L) {
    redirect_logical_user_to_temp(L, 3); /* the backing-resource path arg */
    return SO_CONTINUE(int, g_hook_lua_event_log_create, L);
}

/* 2026-07-16 CROWD-CHOICE / CROSS-CHAPTER FIX. StatChoicesHandler.lua persists
 * the server "choice_tracker" document (the "other players chose X%" stats + the
 * cross-chapter carryover) via SaveDownloadedDocumentAsPropertySet(docName,
 * "logical:<User>/choice.prop"); ChoiceStats.lua reads it back. This native
 * writer builds its OWN ResourceConcreteLocation internally, so none of the
 * Create/Save/ResourceExists redirects intercept it -> the <User> address never
 * binds (same dead-location root cause as the save bundles) and the write is
 * dropped -> the "next chapter" choice presenter is empty/inconsistent. Redirect
 * the resource-path arg to logical:<Temp>/ like every other save resource. The
 * real crowd-stats data is also shipped as a pre-baked choice.prop in the data
 * folder so the offline read has content even before any write. (The doc-name
 * arg won't match the logical:<User>/ prefix, so redirecting both is safe.) */
static so_hook g_hook_lua_save_downloaded_doc_as_propset;
static int hook_lua_save_downloaded_doc_as_propset(void *L) {
    const char *a1 = trace_arg_str(L, 1);
    const char *a2 = trace_arg_str(L, 2);
    l_info("CHOICEIO: SaveDownloadedDoc arg1='%s' arg2='%s'", a1 ? a1 : "?", a2 ? a2 : "?");
    redirect_logical_user_to_temp(L, 1);
    redirect_logical_user_to_temp(L, 2); /* "logical:<User>/choice.prop" output path */
    return SO_CONTINUE(int, g_hook_lua_save_downloaded_doc_as_propset, L);
}

/* 2026-07-16 CHOICE-STATS READ DIAGNOSIS. ChoiceStats.lua reads the crowd stats
 * via ResourceExists("choice.prop") + PropertyGet; StatChoicesHandler may also
 * retrieve the downloaded doc via these. Offline the server fetch never lands, so
 * LOG the whole download/retrieve flow (and redirect any <User> path so a pre-
 * placed choice.prop is served) to see EXACTLY where the offline read stops. */
static so_hook g_hook_lua_download_doc_retrieve;
static int hook_lua_download_doc_retrieve(void *L) {
    const char *a1 = trace_arg_str(L, 1);
    l_info("CHOICEIO: DownloadDocumentRetrieve arg1='%s'", a1 ? a1 : "?");
    redirect_logical_user_to_temp(L, 1);
    redirect_logical_user_to_temp(L, 2);
    return SO_CONTINUE(int, g_hook_lua_download_doc_retrieve, L);
}
static so_hook g_hook_lua_download_docs_from_server;
static int hook_lua_download_docs_from_server(void *L) {
    const char *a1 = trace_arg_str(L, 1);
    l_info("CHOICEIO: DownloadDocumentsFromServer arg1='%s'", a1 ? a1 : "?");
    return SO_CONTINUE(int, g_hook_lua_download_docs_from_server, L);
}
static so_hook g_hook_lua_set_download_completed_cb;
static int hook_lua_set_download_completed_cb(void *L) {
    l_info("CHOICEIO: SetDownloadCompletedCallback registered");
    return SO_CONTINUE(int, g_hook_lua_set_download_completed_cb, L);
}

/* Diagnostic: confirm SavePrefs actually runs + bracket it so the SAVEIO
 * lines (io.c) for the prefs.prop write land between these markers. */
static so_hook g_hook_lua_save_prefs;
static int hook_lua_save_prefs(void *L) {
    static uint32_t count = 0;
    count++;
    l_info("SAVETRACE: SavePrefs ENTER #%u", count);
    int ret = SO_CONTINUE(int, g_hook_lua_save_prefs, L);
    l_info("SAVETRACE: SavePrefs RETURN #%u", count);
    return ret;
}

static so_hook g_hook_lua_resource_exists_redirect;
static int hook_lua_resource_exists_redirect(void *L) {
    static uint32_t count = 0;
    redirect_logical_user_to_temp(L, 1);
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: ResourceExists('%s') ENTER #%u", nm ? nm : "?", count);
    }
    int ret = SO_CONTINUE(int, g_hook_lua_resource_exists_redirect, L);
    if (is_save_shaped) {
        l_info("SAVETRACE: ResourceExists('%s') RETURN #%u", nm ? nm : "?", count);
    }
    return ret;
}

static so_hook g_hook_lua_sceneopen;
static int hook_lua_sceneopen(void *L) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    const char *nm = trace_arg_str(L, 1);
    l_info("Diag: luaSceneOpen #%u scene='%s'", count, nm ? nm : "(non-string)");
#ifndef USE_PVR_PSP2
    /* SceneOpen loads the scene's textures synchronously (multi-second freeze).
     * Flag it so glutil's texture-upload path animates the loading screen, and
     * reset the per-load timer. Cleared after the load completes. */
    loading_screen_begin();
    g_scene_loading = 1;
    int rc = SO_CONTINUE(int, g_hook_lua_sceneopen, L);
    g_scene_loading = 0;
    return rc;
#else
    return SO_CONTINUE(int, g_hook_lua_sceneopen, L);
#endif
}

/* DIAGNOSTIC (2026-06-21): the menu reached its scripts but parks in
 * Menu_Main's startup BEFORE opening its scene, while Load()-ing the menu
 * CHARACTER MESHES (skM1_jesse/olivia/lukas/axel/petra/ellie/gabriel_*.d3dmesh
 * + portal blocks). Trace luaLoad ENTER+RETURN with the resource name: the
 * resource that logs ENTER and never RETURN is the one whose load hangs.
 * Also trace luaResourceIsLoaded (throttled) in case it polls "is it loaded?"
 * forever on an async load that never completes. Main-thread Lua -> safe. */
static so_hook g_hook_lua_load;
static int hook_lua_load(void *L) {
    static uint32_t count = 0;
    count++;
    redirect_logical_user_to_temp(L, 1);
    const char *nm = trace_arg_str(L, 1);
    if (mcsm_mega_diag_enabled()) {
        l_info("Diag: luaLoad #%u ENTER name='%s'", count, nm ? nm : "(?)");
    }
    int ret = SO_CONTINUE(int, g_hook_lua_load, L);
    if (mcsm_mega_diag_enabled()) {
        l_info("Diag: luaLoad #%u RETURN name='%s'", count, nm ? nm : "(?)");
    }
    return ret;
}
static so_hook g_hook_lua_resource_is_loaded;
static int hook_lua_resource_is_loaded(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 40U || (count & 0xFFU) == 0U) {
        const char *nm = trace_arg_str(L, 1);
        l_info("Diag: luaResourceIsLoaded #%u name='%s'", count, nm ? nm : "(?)");
    }
    return SO_CONTINUE(int, g_hook_lua_resource_is_loaded, L);
}

/* STREAMING FIX (2026-06-21): On Android ScenePreload returns immediately
 * after queuing async work; the main game loop pumps AdvancePreloadBatch
 * each frame until the scene is ready.  On Vita the main loop stops pumping
 * right after ScenePreload, so the async preload never completes and the
 * earlier hack returned 0 (skip) — forcing ALL textures to upload at
 * SceneOpen time synchronously → 2076 uploads for ~308 unique textures →
 * GPU OOM at ~7200 frames with free_cdram dropping to ~12MB.
 *
 * The engine-level fix: let ScenePreload run natively, capture the Lua
 * state, then drive AdvancePreloadBatch from our GameEngine_Loop hook until
 * the preload is complete.  This keeps only the current scene's textures
 * resident in VRAM, exactly as the Android build does. */
static void    *g_preload_lua_state = NULL;
static volatile int g_preload_pending   = 0;
static volatile int g_preload_log_once  = 0;
/* Direct function pointer to luaResourceAdvancePreloadBatch so we can
 * pump it from the C++ game loop without round-tripping through Lua. */
typedef int (*AdvancePreloadBatchFn)(void *L);
static AdvancePreloadBatchFn g_advance_preload_fn = NULL;

static int stream_resolve_preload_api(void) {
    if (!g_advance_preload_fn) {
        uintptr_t addr = so_symbol(&so_mod_gameengine,
                          "_Z30luaResourceAdvancePreloadBatchP9lua_State");
        if (!addr) {
            if (!g_preload_log_once) {
                l_warn("STREAM: cannot resolve AdvancePreloadBatch symbol");
                g_preload_log_once = 1;
            }
            return 0;
        }
        g_advance_preload_fn = (AdvancePreloadBatchFn)addr;
    }
    return 1;
}

static void stream_pump_preload(void) {
    if (!g_preload_pending || !g_preload_lua_state)
        return;
    if (!stream_resolve_preload_api()) {
        g_preload_pending = 0;
        return;
    }

    int pumped = 0;
    const uint64_t pump_t0 = sceKernelGetSystemTimeWide();
#ifndef USE_PVR_PSP2
    const int old_scene_loading = g_scene_loading;
    g_scene_loading = 1;
#endif
    for (int i = 0; i < 2 && g_preload_pending; ++i) {
        int ret = g_advance_preload_fn(g_preload_lua_state);
        pumped++;
        if (ret <= 0) {
            g_preload_pending = 0;
            l_info("STREAM: preload pump complete after %d calls", pumped);
            break;
        }
        const uint64_t elapsed_us = sceKernelGetSystemTimeWide() - pump_t0;
        if (elapsed_us >= 4000ULL) {
            break;
        }
    }
#ifndef USE_PVR_PSP2
    g_scene_loading = old_scene_loading;
#endif
    if (pumped && g_preload_pending && !g_preload_log_once) {
        g_preload_log_once = 1;
        l_info("STREAM: preload pump still pending after %d calls", pumped);
    }
    const uint32_t pump_ms = (uint32_t)((sceKernelGetSystemTimeWide() - pump_t0) / 1000ULL);
    if (pump_ms > 25U) {
        l_info("STREAM: preload pump budget overrun calls=%d work=%ums", pumped, pump_ms);
    }
}

static so_hook g_hook_lua_scene_preload;
static int hook_lua_scene_preload(void *L) {
    static uint32_t count = 0;
    count++;
    const char *nm = trace_arg_str(L, 1);
    /* 2026-06-21 (revised): SKIPPING ScenePreload makes the PRESENT hang — across
     * runs the render thread's eglSwapBuffers wedges only when ScenePreload is
     * skipped (logs 185908, 233044), whereas with the real ScenePreload running
     * the present stays alive for 25k+ frames (log 215037). There the game only
     * stalled in ScenePreload's RenderThread::FinishFrame because its texture
     * uploads OOM'd and never finished — which the GL-layer LRU eviction cap
     * (glutil.c) now prevents. So run the REAL ScenePreload (keeps present alive)
     * and let the cap bound its uploads so FinishFrame completes and it returns. */
    l_info("STREAM: luaScenePreload #%u scene='%s' (real preload; LRU cap bounds uploads)",
           count, nm ? nm : "(?)");
    g_preload_lua_state = L;
    g_preload_pending   = 1;
    g_preload_log_once  = 0;
#ifndef USE_PVR_PSP2
    const int old_scene_loading = g_scene_loading;
    loading_screen_begin();
    g_scene_loading = 1;
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_scene_preload, L);
#ifndef USE_PVR_PSP2
    g_scene_loading = old_scene_loading;
#endif
    l_info("STREAM: luaScenePreload #%u returned %d", count, ret);
    if (ret == 0) {
        g_preload_pending = 0;
    }
    return ret;
}

static so_hook g_hook_lua_advance_preload;
static int hook_lua_advance_preload(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 16U || (count & 0xFFU) == 0U) {
        l_info("Diag: luaResourceAdvancePreloadBatch #%u (preload pump)", count);
    }
    return SO_CONTINUE(int, g_hook_lua_advance_preload, L);
}
static so_hook g_hook_lua_wait_for_callbacks;
static int hook_lua_wait_for_callbacks(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 16U || (count & 0x3FU) == 0U) {
        l_info("Diag: luaWaitForCallbacks #%u ENTER", count);
    }
    return SO_CONTINUE(int, g_hook_lua_wait_for_callbacks, L);
}

/* (Reverted 2026-06-21) The ui_boot_* ChorePlayAndWait boot-skip was net-
 * negative: those chores ALSO present the studio logos / legal / status text,
 * so skipping them just removed the logos and the "Checking For DLC" text
 * WITHOUT clearing the real hang (login). Removed -> chores run natively again
 * (logos restored). The real blocker is UserManager_LogIn; see the IsToolBuild
 * bypass below. */

typedef void (*LuaSetTopFn)(void *L, int idx);
typedef void (*LuaPushBooleanFn)(void *L, int value);
typedef void (*LuaPushIntegerFn)(void *L, int value);
typedef const char *(*LuaPushStringFn)(void *L, const char *s);

static LuaSetTopFn g_lua_settop_fast;
static LuaPushBooleanFn g_lua_pushboolean_fast;
static LuaPushIntegerFn g_lua_pushinteger_fast;
static LuaPushStringFn g_lua_pushstring_fast;

static int lua_push_forced_bool(void *L, int value, const char *label) {
    if (!g_lua_settop_fast) {
        g_lua_settop_fast = (LuaSetTopFn)so_symbol(&so_mod_gameengine, "lua_settop");
    }
    if (!g_lua_pushboolean_fast) {
        g_lua_pushboolean_fast = (LuaPushBooleanFn)so_symbol(&so_mod_gameengine, "lua_pushboolean");
    }
    if (!g_lua_settop_fast || !g_lua_pushboolean_fast) {
        l_warn("DLC fastpath: Lua API missing while forcing %s.", label);
        return -1;
    }

    g_lua_settop_fast(L, 0);
    g_lua_pushboolean_fast(L, value ? 1 : 0);
    return 1;
}

static int lua_push_forced_integer(void *L, int value, const char *label) {
    if (!g_lua_settop_fast) {
        g_lua_settop_fast = (LuaSetTopFn)so_symbol(&so_mod_gameengine, "lua_settop");
    }
    if (!g_lua_pushinteger_fast) {
        g_lua_pushinteger_fast = (LuaPushIntegerFn)so_symbol(&so_mod_gameengine, "lua_pushinteger");
    }
    if (!g_lua_settop_fast || !g_lua_pushinteger_fast) {
        l_warn("DLC fastpath: Lua API missing while forcing integer %s.", label);
        return -1;
    }

    g_lua_settop_fast(L, 0);
    g_lua_pushinteger_fast(L, value);
    return 1;
}

static int lua_push_forced_string(void *L, const char *value, const char *label) {
    if (!g_lua_settop_fast) {
        g_lua_settop_fast = (LuaSetTopFn)so_symbol(&so_mod_gameengine, "lua_settop");
    }
    if (!g_lua_pushstring_fast) {
        g_lua_pushstring_fast = (LuaPushStringFn)so_symbol(&so_mod_gameengine, "lua_pushstring");
    }
    if (!g_lua_settop_fast || !g_lua_pushstring_fast) {
        l_warn("LANG: Lua API missing while forcing string %s.", label);
        return -1;
    }
    g_lua_settop_fast(L, 0);
    g_lua_pushstring_fast(L, value);
    return 1;
}

static int hook_forced_lua_bool(void *L,
                                int value,
                                const char *label,
                                uint32_t *count) {
    (*count)++;
    launch_state_mark_progress();
    if (*count <= 12U || ((*count) & 0x7fU) == 0U) {
        l_info("DLC fastpath: %s -> %s count=%u L=%p",
               label,
               value ? "true" : "false",
               *count,
               L);
    }
    return lua_push_forced_bool(L, value, label);
}

static so_hook g_hook_lua_platform_is_connected_to_internet;
static so_hook g_hook_lua_platform_is_connected_to_license_server;
static so_hook g_hook_lua_platform_is_age_restricted;
static so_hook g_hook_lua_begin_offers_enumeration;
static so_hook g_hook_lua_offers_enumeration_ready;
static so_hook g_hook_lua_is_episode_available;
static so_hook g_hook_lua_is_episode_purchased;
static so_hook g_hook_lua_is_episode_downloaded;
static so_hook g_hook_lua_is_episode_unlicensed;
static so_hook g_hook_lua_get_demo_mode;
static so_hook g_hook_lua_get_demo_timeout;
static so_hook g_hook_lua_platform_get_trial_timeout;
static so_hook g_hook_lua_platform_can_user_make_purchases;
static so_hook g_hook_lua_platform_is_user_space_available;
static so_hook g_hook_lua_is_storage_selected;
static so_hook g_hook_lua_save_load_has_available_space;
static so_hook g_hook_lua_platform_get_free_disk_space;
static so_hook g_hook_lua_storage_device_error_on_removal;
static so_hook g_hook_lua_is_save_game_corrupt;
static so_hook g_hook_lua_file_is_last_error_corrupt_save_file;
static so_hook g_hook_gameengine_get_trial_version;
static so_hook g_hook_gameengine_get_trial_version_secure;
static so_hook g_hook_ttplatform_is_trial_version;
static so_hook g_hook_ttplatform_is_user_space_available;
static so_hook g_hook_platform_android_is_user_space_available;

static int hook_lua_platform_is_connected_to_internet(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "PlatformIsConnectedToInternet", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_connected_to_internet, L);
}

static int hook_lua_platform_is_connected_to_license_server(void *L) {
    static uint32_t count = 0;
    /* Must be false when internet is also false. Returning true while
     * luaPlatformIsConnectedToInternet returns false creates an impossible
     * state (license server reachable without internet) that traps the boot
     * flow in a DLC verification retry loop (DLCStatus/PurchaseManager/
     * DownloadManager loaded 8+ times as timestamp evidence). */
    int ret = hook_forced_lua_bool(L, 0, "PlatformIsConnectedToLicenseServer", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_connected_to_license_server, L);
}

static int hook_lua_platform_is_age_restricted(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "PlatformIsAgeRestricted", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_age_restricted, L);
}

static int hook_lua_begin_offers_enumeration(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "BeginOffersEnumeration", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_begin_offers_enumeration, L);
}

static int hook_lua_offers_enumeration_ready(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "OffersEnumerationReady", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_offers_enumeration_ready, L);
}

/* EPISODE VISIBILITY via chapters.txt  (2026-07-10)
 * -----------------------------------------------------------------------------
 * The engine asks luaIsEpisodeAvailable/Downloaded once per episode. We answer
 * per chapter so ONLY the chapters you want appear in Episode Select -- and you
 * change which ones show WITHOUT ever rebuilding again: just edit a text file on
 * the Vita:
 *
 *     ux0:data/mcsm/chapters.txt        (one line per chapter)
 *         1=true
 *         2=true
 *         3=false
 *         4=false ... 8=false
 *
 * Resolution order for a given chapter N:
 *   1. Chapter 1 is ALWAYS available (never break the base game / a CH1 test).
 *   2. If chapters.txt lists N -> obey it exactly (true shows it, false hides it).
 *   3. Otherwise fall back to "is its data archive actually on disk?"
 *      (assets/MCSM_android_Minecraft10N_data.ttarch2).
 *   4. If we cannot identify the episode at all -> available (old safe default).
 * chapters.txt is read once and cached; accepted values: true/1/yes or
 * false/0/no. Delete the file to fall back to pure on-disk auto-detection. */
static int mcsm_file_present(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}
static signed char s_ch_txt[9];        /* [1..8]: 1=force-show, 0=force-hide, -1=unspecified */
static int         s_ch_txt_loaded = 0;
static void mcsm_load_chapters_txt(void) {
    const McsmGame *g = mcsm_game();
    s_ch_txt[0] = -1;
    for (int i = 0; i < 8; i++) s_ch_txt[i + 1] = (signed char)g->chapters[i];
    s_ch_txt_loaded = 1;
}
/* Which chapter (1..8) is this availability query about, or 0 if we can't tell.
 * Handles both a string set-name ("...Minecraft10N...") and a numeric index. */
static int mcsm_episode_chapter(void *L) {
    typedef int         (*LuaTypeFn3)(void *, int);
    typedef const char *(*LuaToLStrFn3)(void *, int, size_t *);
    typedef double      (*LuaToNumFn3)(void *, int);
    static LuaTypeFn3 s_type; static LuaToLStrFn3 s_tolstr; static LuaToNumFn3 s_tonum;
    if (!s_type)   s_type   = (LuaTypeFn3)so_symbol(&so_mod_gameengine, "lua_type");
    if (!s_tolstr) s_tolstr = (LuaToLStrFn3)so_symbol(&so_mod_gameengine, "lua_tolstring");
    if (!s_tonum)  s_tonum  = (LuaToNumFn3)so_symbol(&so_mod_gameengine, "lua_tonumber");
    if (!s_type) return 0;
    int t = s_type(L, 1);
    if (t == 3 /*LUA_TNUMBER*/ && s_tonum) {
        int nch = (int)(s_tonum(L, 1) + 0.5);
        return (nch >= 1 && nch <= 8) ? nch : 0;
    }
    if (t == 4 /*LUA_TSTRING*/ && s_tolstr) {
        const char *s = s_tolstr(L, 1, NULL);
        if (!s) return 0;
        const char *m = s;
        while ((m = strstr(m, "10")) != NULL) {
            char c = m[2];
            if (c >= '1' && c <= '8') return c - '0';
            m += 2;
        }
    }
    return 0;
}
static int mcsm_episode_available(void *L) {
    static signed char s_logged[9];                    /* log each decision once (diag) */
    int ch = mcsm_episode_chapter(L);
    if (ch == 1) return 1;                              /* CH1 = base game, always available */
    if (!s_ch_txt_loaded) mcsm_load_chapters_txt();
    if (ch >= 2 && ch <= 8) {
        int vis; const char *src;
        if (s_ch_txt[ch] == 1)      { vis = 1; src = "chapters.txt"; }   /* force-show */
        else if (s_ch_txt[ch] == 0) { vis = 0; src = "chapters.txt"; }   /* force-hide */
        else {                                         /* unlisted -> require its data on disk (memoized) */
            /* The three IsEpisode* menu hooks call this on every refresh while
             * Episode-Select is open; the chapter archives don't appear/disappear
             * mid-session (copied in before boot), so probe each at most once
             * instead of fopen'ing a large .ttarch2 path per query. */
            static signed char s_present[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
            if (s_present[ch] < 0) {
                char path[256];
                snprintf(path, sizeof(path),
                         DATA_PATH "assets/MCSM_android_Minecraft10%d_data.ttarch2", ch);
                s_present[ch] = (signed char)(mcsm_file_present(path) ? 1 : 0);
            }
            vis = s_present[ch]; src = "data-present";
        }
        if (!s_logged[ch]) { s_logged[ch] = 1;
            l_info("EPVIS: ch=%d -> %s (%s)", ch, vis ? "SHOW" : "hide", src); }
        return vis;
    }
    /* ch <= 0: an episode we cannot map to chapter 1..8. This previously returned
     * 1 (show), which presented chapters with no data behind them. Default to HIDE
     * so nothing unrecognized leaks into Episode Select. CH1 is recognized above,
     * so the base game is never affected. */
    if (!s_logged[0]) { s_logged[0] = 1; l_info("EPVIS: unidentified episode -> hide"); }
    return 0;
}

static int hook_lua_is_episode_available(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, mcsm_episode_available(L), "IsEpisodeAvailable", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_episode_available, L);
}

static int hook_lua_is_episode_purchased(void *L) {
    static uint32_t count = 0;
    /* Gate "purchased/owned" by the same per-chapter availability as Available/
     * Downloaded. Previously hardcoded 1 (every episode owned), which made the
     * menu present CH2-8 as "installed" (and offer "restart chapter") even with
     * no data. CH1 stays owned (mcsm_episode_available==1); empty chapters ->0. */
    int ret = hook_forced_lua_bool(L, mcsm_episode_available(L), "IsEpisodePurchased", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_episode_purchased, L);
}

static int hook_lua_is_episode_downloaded(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, mcsm_episode_available(L), "IsEpisodeDownloaded", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_episode_downloaded, L);
}

static int hook_lua_is_episode_unlicensed(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "IsEpisodeUnlicensed", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_episode_unlicensed, L);
}

/* LANGUAGE. The engine loads ALL language packs as "constant" resource sets, so
 * getLocale doesn't pick the text language — luaGetUserSystemLanguage does (its
 * result feeds LangSetCurLanguage). Force it from settings/language.txt. The
 * value may be a locale (ru_RU/fr_FR/...) or a bare game language name; both map
 * to the engine's names: English/Russian/French/German/Spanish/Chinese/Portuguese. */
static void mcsm_forced_language_name(char *out, int outsz) {
    /* Resolve ONCE and cache (this hook fires repeatedly as UI/scripts set up
     * localized text; language.txt never changes mid-session). Mirrors the
     * already-cached locale twin GetLocale() in java.c — avoids a memory-card
     * open (up to 2 fopen syscalls via mcsm_open_setting) on every call. */
    static char s_name[32];
    static int s_resolved = 0;
    if (!s_resolved) {
        s_resolved = 1;
        s_name[0] = '\0';
        {
            char v[32] = "";
            strncpy(v, mcsm_game()->language, sizeof(v) - 1);
            if (v[0]) {
                static const struct { const char *code; const char *name; } m[] = {
                    { "en", "English" }, { "ru", "Russian" }, { "fr", "French" },
                    { "de", "German" },  { "es", "Spanish" }, { "zh", "Chinese" },
                    { "pt", "Portuguese" },
                };
                int matched = 0;
                for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
                    if (strncmp(v, m[i].code, 2) == 0) { snprintf(s_name, sizeof(s_name), "%s", m[i].name); matched = 1; break; }
                }
                if (!matched) snprintf(s_name, sizeof(s_name), "%s", v);  /* exact name (e.g. "Russian") passes through */
            }
        }
    }
    snprintf(out, outsz, "%s", s_name);
}

static so_hook g_hook_lua_get_user_system_language;
static int hook_lua_get_user_system_language(void *L) {
    int orig = SO_CONTINUE(int, g_hook_lua_get_user_system_language, L);
    char forced[32]; mcsm_forced_language_name(forced, sizeof(forced));
    static int logged = 0;
    if (!logged) {
        logged = 1;
        if (!g_lua_tolstring_fast) {
            g_lua_tolstring_fast = (LuaToLStringFn)so_symbol(&so_mod_gameengine, "lua_tolstring");
        }
        const char *nat = g_lua_tolstring_fast ? g_lua_tolstring_fast(L, -1, NULL) : NULL;
        l_info("LANG: GetUserSystemLanguage native=\"%s\" forced=\"%s\"",
               nat ? nat : "(null)", forced[0] ? forced : "(none)");
    }
    if (forced[0]) {
        int r = lua_push_forced_string(L, forced, "GetUserSystemLanguage");
        if (r >= 0) return r;
    }
    return orig;
}

static int hook_lua_get_demo_mode(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "GetDemoMode(full game)", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_get_demo_mode, L);
}

static int hook_lua_get_demo_timeout(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GetDemoTimeout -> 0 count=%u L=%p", count, L);
    }
    int ret = lua_push_forced_integer(L, 0, "GetDemoTimeout");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_get_demo_timeout, L);
}

static int hook_lua_platform_get_trial_timeout(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: PlatformGetTrialTimeout -> 0 count=%u L=%p", count, L);
    }
    int ret = lua_push_forced_integer(L, 0, "PlatformGetTrialTimeout");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_get_trial_timeout, L);
}

static int hook_lua_platform_can_user_make_purchases(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "PlatformCanUserMakePurchases", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_can_user_make_purchases, L);
}

static int hook_lua_platform_is_user_space_available(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "PlatformIsUserSpaceAvailable", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_user_space_available, L);
}

static int hook_lua_is_storage_selected(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "IsStorageSelected", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_storage_selected, L);
}

static int hook_lua_save_load_has_available_space(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "SaveLoadHasAvailableSpace", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_save_load_has_available_space, L);
}

static int hook_lua_platform_get_free_disk_space(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("SAVEFIX: PlatformGetFreeDiskSpace -> 536870912 count=%u L=%p", count, L);
    }
    int ret = lua_push_forced_integer(L, 536870912, "PlatformGetFreeDiskSpace");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_get_free_disk_space, L);
}

static int hook_lua_storage_device_error_on_removal(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "StorageDeviceErrorOnRemoval", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_storage_device_error_on_removal, L);
}

static int hook_lua_is_save_game_corrupt(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "IsSaveGameCorrupt", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_save_game_corrupt, L);
}

static int hook_lua_file_is_last_error_corrupt_save_file(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "FileIsLastErrorCorruptSaveFile", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_file_is_last_error_corrupt_save_file, L);
}

static int hook_gameengine_get_trial_version(void) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GameEngine::GetTrialVersion -> false (#%u)", count);
    }
    return 0;
}

static int hook_gameengine_get_trial_version_secure(void) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GameEngine::GetTrialVersionSecure -> false (#%u)", count);
    }
    return 0;
}

static int hook_ttplatform_is_trial_version(void *self) {
    static uint32_t count = 0;
    (void)self;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: TTPlatform::IsTrialVersion -> false (#%u)", count);
    }
    return 0;
}

static int hook_ttplatform_is_user_space_available(void *self) {
    static uint32_t count = 0;
    (void)self;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("SAVEFIX: TTPlatform::IsUserSpaceAvailable -> true (#%u)", count);
    }
    return 1;
}

static int hook_platform_android_is_user_space_available(void *self) {
    static uint32_t count = 0;
    (void)self;
    count++;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("SAVEFIX: Platform_Android::IsUserSpaceAvailable -> true (#%u)", count);
    }
    return 1;
}

/* ---- New Game / episode-start gate (2026-06-23) ---------------------------
 * Symptom: confirming the character in Menu_CharacterSelect briefly loads, then
 * returns to the main menu. The log shows only ui_menuMain ever (re)opening and
 * the Menu subproject re-running ~130 scripts each time; no adv_env* gameplay
 * scene is ever requested. So the confirm re-enters the Menu subproject instead
 * of switching to the episode subproject.
 *
 * Root cause (from the game's own Lua): Menu_CharacterSelect_Complete reads a
 * "Licensed" user property; when true it calls Menu_StartEpisode, otherwise it
 * does SubProject_Switch("Menu_Main") -- exactly the bounce we see. DRM::
 * IsLicensed and GameEngine::ValidateDRM are already hardcoded true in this
 * build, so the demo/unlicensed state is decided at the Lua/property layer.
 *
 * Fix: intercept luaPropertyGet and force any "Licensed" key read to true so the
 * full game unlocks and the episode subproject loads. Also trace luaSetSubProject
 * (the C function behind Lua SubProject_Switch) so the log shows the exact target
 * the confirm switches to (Menu_Main vs the episode). */
typedef int (*LuaTypeFn)(void *L, int idx);
typedef const char *(*LuaToLStringFn)(void *L, int idx, size_t *len);
typedef int (*LuaGetTopFn)(void *L);
static LuaTypeFn g_lua_type_fast;
static LuaToLStringFn g_lua_tolstring_fast;
static LuaGetTopFn g_lua_gettop_fast;

#define LUA_TSTRING_TAG 4 /* stable across all Lua versions */

static void resolve_lua_str_api(void) {
    if (!g_lua_type_fast)
        g_lua_type_fast = (LuaTypeFn)so_symbol(&so_mod_gameengine, "lua_type");
    if (!g_lua_tolstring_fast)
        g_lua_tolstring_fast = (LuaToLStringFn)so_symbol(&so_mod_gameengine, "lua_tolstring");
    if (!g_lua_gettop_fast)
        g_lua_gettop_fast = (LuaGetTopFn)so_symbol(&so_mod_gameengine, "lua_gettop");
}

static const char *lua_arg_string_checked(void *L, int idx) {
    resolve_lua_str_api();
    if (!g_lua_type_fast || !g_lua_tolstring_fast || !g_lua_gettop_fast ||
        g_lua_gettop_fast(L) < idx ||
        g_lua_type_fast(L, idx) != LUA_TSTRING_TAG) {
        return NULL;
    }
    return g_lua_tolstring_fast(L, idx, NULL);
}

static so_hook g_hook_lua_set_subproject;
static int hook_lua_set_subproject(void *L) {
    static uint32_t count = 0;
    count++;
    resolve_lua_str_api();
    const char *name = NULL;
    if (g_lua_type_fast && g_lua_tolstring_fast &&
        g_lua_type_fast(L, 1) == LUA_TSTRING_TAG) {
        name = g_lua_tolstring_fast(L, 1, NULL);
    }
    l_info("TRACE: SubProject_Switch -> '%s' (#%u)", name ? name : "(non-string)", count);
    return SO_CONTINUE(int, g_hook_lua_set_subproject, L);
}

/* 2026-07-02 (2nd pass): luaSetSubProject (native SubProject_Switch) is a
 * DEAD END for this window-clear -- decompiled SubProject.lua shows
 * SubProject_Switch() itself never calls SetSubProject; both the "Menu"
 * return branch AND SubProject_StartEpisode -> SubProject_Switch end in
 * `ResetGame(firstScript, resourceSets)`, the actual native call. Clearing
 * on the wrong hook left the character-select window armed straight into
 * gameplay -> the treehouse autosave still saw fake Licensed=false -> demo
 * cache again. Hook the REAL funnel instead. */
static so_hook g_hook_lua_reset_game;
static int hook_lua_reset_game(void *L) {
    static uint32_t count = 0;
    count++;
    resolve_lua_str_api();
    const char *name = NULL;
    if (g_lua_type_fast && g_lua_tolstring_fast &&
        g_lua_type_fast(L, 1) == LUA_TSTRING_TAG) {
        name = g_lua_tolstring_fast(L, 1, NULL);
    }
    l_info("TRACE: ResetGame -> '%s' (#%u)", name ? name : "(non-string)", count);
    clear_character_select_license_window();
    return SO_CONTINUE(int, g_hook_lua_reset_game, L);
}

/* Trace which resource sets get enabled -- on a successful episode launch the
 * episode's set (e.g. Android_KP_101 / Minecraft101 / JC101) is enabled here.
 * Confirms whether the chapter load is actually reached after the Licensed flip. */
static so_hook g_hook_lua_resource_set_enable;
static int hook_lua_resource_set_enable(void *L) {
    static uint32_t count = 0;
    count++;
    resolve_lua_str_api();
    const char *name = NULL;
    if (g_lua_type_fast && g_lua_tolstring_fast &&
        g_lua_type_fast(L, 1) == LUA_TSTRING_TAG) {
        name = g_lua_tolstring_fast(L, 1, NULL);
    }
    l_info("TRACE: ResourceSetEnable('%s') #%u", name ? name : "(non-string)", count);
    return SO_CONTINUE(int, g_hook_lua_resource_set_enable, L);
}

/* 2026-07-03 STUTTER FIX — the "impossible" 4-5 SECOND freezes. Log proof:
 * MCSM_android-pvr_JesseMale101_all.ttarch2 (30MB) was re-OPENED 25 times and
 * ResourceSetEnable('JesseMale101') fired 12 times in ONE session. The game's
 * PlayerChoice_Set (Lua) disables THEN re-enables the character resource set on
 * every scene transition, forcing a full ~30MB re-decompress + mesh/skeleton
 * rebuild each time = the multi-second sim freeze (NOT shaders, NOT the memory-
 * alloc black-texture issue — a THIRD, script-driven cause). Fix: keep the
 * frequently-thrashed CHARACTER sets RESIDENT by skipping their
 * ResourceSetDisable. The script's own EnableResourceSet wrapper then sees the
 * set still-enabled and skips the reload -> loads ONCE, never re-decodes.
 * Engine state stays consistent (we simply never call the real disable, so
 * "enabled" and the loaded resources both remain true). Bounded memory (~30-48MB
 * for the current episode's Jesse; the 320MB heap has room). Escape hatch:
 * ux0:data/mcsm/no_keep_resident.txt (read once). */
static const char *resource_set_arg_name(void *L); /* fwd decl (defined below) */
static int g_keep_resident_off = -1;
static int keep_resident_enabled(void) {
    if (g_keep_resident_off < 0) {
        FILE *f = fopen("ux0:data/mcsm/no_keep_resident.txt", "r");
        g_keep_resident_off = f ? 1 : 0;
        if (f) fclose(f);
    }
    return g_keep_resident_off == 0;
}
static int resource_set_should_stay_resident(const char *name) {
    /* "JesseMale" is a prefix of both "JesseMale101" (30MB model) and
     * "JesseMaleChores101" (9.5MB) -> one match covers the model + its chores.
     * "JesseFemale" mirror for female Jesse. */
    return name && keep_resident_enabled() &&
        (strstr(name, "JesseMale") != NULL ||
         strstr(name, "JesseFemale") != NULL);
}
static so_hook g_hook_lua_resource_set_disable;
static int hook_lua_resource_set_disable(void *L) {
    const char *name = resource_set_arg_name(L);
    if (resource_set_should_stay_resident(name)) {
        static uint32_t skipped = 0;
        skipped++;
        if (skipped <= 24U || (skipped & 0x3FU) == 0U) {
            l_info("PERF: kept '%s' resident (skipped disable -> avoids 30MB re-decode freeze) #%u",
                   name, skipped);
        }
        return 0; /* push no Lua return values; EnableResourceSet ignores it */
    }
    return SO_CONTINUE(int, g_hook_lua_resource_set_disable, L);
}

static int resource_set_name_is_vita_ui(const char *name) {
    return name &&
        (strcmp(name, "Vita") == 0 ||
         strcmp(name, "PSVita") == 0);
}

static int resource_set_name_is_other_playstation_ui(const char *name) {
    return name &&
        (strcmp(name, "PS3") == 0 ||
         strcmp(name, "PS4") == 0);
}

static int resource_set_name_is_xbox_ui(const char *name) {
    return name &&
        (strcmp(name, "Xbox360") == 0 ||
         strcmp(name, "XBox360") == 0 ||
         strcmp(name, "XBOne") == 0 ||
         strcmp(name, "XboxOne") == 0);
}

/* Is chapter 2 meant to be shown? chapters.txt is authoritative (2=true force-on,
 * 2=false force-off); if unlisted, only when its data archive is present. */
static int mcsm_ch2_forced_visible(void) {
    if (!s_ch_txt_loaded) mcsm_load_chapters_txt();
    if (s_ch_txt[2] == 1) return 1;
    if (s_ch_txt[2] == 0) return 0;
    return mcsm_file_present(DATA_PATH "assets/MCSM_android_Minecraft102_data.ttarch2");
}

static int resource_set_name_is_episode2_local(const char *name) {
    if (!(name &&
        (strstr(name, "101-102_Available") ||
         strstr(name, "Minecraft102") ||
         strstr(name, "Android102") ||
         strstr(name, "Android_KP_M102") ||
         strstr(name, "Android_KP_J102") ||
         strstr(name, "Android_KP_JC102") ||
         strstr(name, "Shaders102")))) {
        return 0;
    }
    /* The whole Episode-2 spoof (ResourceSetExists/Enabled -> true, cloud-mount,
     * fallback descriptor) exists ONLY to force Episode 2 present. For a CH1 tester
     * (chapters.txt 2=false / no CH2 data) return 0 so none of it runs: Episode 2
     * reads as genuinely absent, so the menu neither shows it installed nor lets you
     * "restart" it — the pre-CH2-subsystem behaviour. Set chapters.txt 2=true to
     * re-enable the spoof. */
    return mcsm_ch2_forced_visible();
}

static const char *resource_set_arg_name(void *L) {
    resolve_lua_str_api();
    if (!g_lua_type_fast || !g_lua_tolstring_fast ||
        g_lua_type_fast(L, 1) != LUA_TSTRING_TAG) {
        return NULL;
    }
    return g_lua_tolstring_fast(L, 1, NULL);
}

/* ---- CH2 REAL MOUNT (2026-07-02) -------------------------------------
 * Forcing ResourceSetExists('Minecraft102') -> true only lied to the menu;
 * the episode's resource description (Net/_resdesc_50_Minecraft102_
 * android-pvr.lua) was never registered, so actually LAUNCHING Episode 2
 * had no content. On Android the engine mounts downloaded SyncFS content
 * (Temp/IN_* manifests + Net/ archives) via NetworkAPICloudMountAllContent
 * (see PurchaseManager_InstallAndReset). Do the same here, lazily, on the
 * first Minecraft102 resource-set query (main Lua thread, resource system
 * up). Fallback: register the descriptor directly via DoString +
 * RegisterSetDescription with absolute Net/ archive paths. */
#define LUA_TFUNCTION_TAG 6
typedef void (*LuaGetGlobalFn)(void *L, const char *name);
typedef int (*LuaPCallKFn)(void *L, int nargs, int nresults, int errfunc,
                           int ctx, void *k);
typedef int (*LuaToBooleanFn)(void *L, int idx);
static LuaGetGlobalFn g_lua_getglobal_fast;
static LuaPCallKFn g_lua_pcallk_fast;
static LuaToBooleanFn g_lua_toboolean_fast;
static volatile int g_ch2_probe_active = 0;
static int g_ch2_mounted_ok = 0;

static int ch2_resolve_call_api(void) {
    resolve_lua_str_api();
    if (!g_lua_getglobal_fast) {
        g_lua_getglobal_fast = (LuaGetGlobalFn)so_symbol(&so_mod_gameengine, "lua_getglobal");
    }
    if (!g_lua_pcallk_fast) {
        g_lua_pcallk_fast = (LuaPCallKFn)so_symbol(&so_mod_gameengine, "lua_pcallk");
    }
    if (!g_lua_toboolean_fast) {
        g_lua_toboolean_fast = (LuaToBooleanFn)so_symbol(&so_mod_gameengine, "lua_toboolean");
    }
    if (!g_lua_settop_fast) {
        g_lua_settop_fast = (LuaSetTopFn)so_symbol(&so_mod_gameengine, "lua_settop");
    }
    if (!g_lua_pushstring_fast) {
        g_lua_pushstring_fast = (LuaPushStringFn)so_symbol(&so_mod_gameengine, "lua_pushstring");
    }
    return g_lua_getglobal_fast && g_lua_pcallk_fast && g_lua_toboolean_fast &&
           g_lua_settop_fast && g_lua_pushstring_fast &&
           g_lua_gettop_fast && g_lua_type_fast;
}

/* Call a no-arg global Lua function; returns pcall rc (0 = ok), -1 if the
 * global is not a function. Stack is restored either way. */
static int ch2_call_global_noargs(void *L, const char *fn) {
    int top = g_lua_gettop_fast(L);
    g_lua_getglobal_fast(L, fn);
    if (g_lua_type_fast(L, -1) != LUA_TFUNCTION_TAG) {
        g_lua_settop_fast(L, top);
        l_warn("CH2: global '%s' is not a function", fn);
        return -1;
    }
    int rc = g_lua_pcallk_fast(L, 0, 0, 0, 0, NULL);
    if (rc != 0 && g_lua_type_fast(L, -1) == LUA_TSTRING_TAG) {
        l_warn("CH2: pcall %s error: %s", fn, g_lua_tolstring_fast(L, -1, NULL));
    }
    g_lua_settop_fast(L, top);
    return rc;
}

static int ch2_call_global_1str(void *L, const char *fn, const char *arg) {
    int top = g_lua_gettop_fast(L);
    g_lua_getglobal_fast(L, fn);
    if (g_lua_type_fast(L, -1) != LUA_TFUNCTION_TAG) {
        g_lua_settop_fast(L, top);
        l_warn("CH2: global '%s' is not a function", fn);
        return -1;
    }
    g_lua_pushstring_fast(L, arg);
    int rc = g_lua_pcallk_fast(L, 1, 0, 0, 0, NULL);
    if (rc != 0 && g_lua_type_fast(L, -1) == LUA_TSTRING_TAG) {
        l_warn("CH2: pcall %s error: %s", fn, g_lua_tolstring_fast(L, -1, NULL));
    }
    g_lua_settop_fast(L, top);
    return rc;
}

/* Ask the ENGINE (not our forced answer) whether a resource set exists.
 * Re-enters hook_lua_resource_set_exists; g_ch2_probe_active makes the hook
 * pass straight through to the original. Returns 1/0, or -1 on error. */
static int ch2_resource_set_exists_probe(void *L, const char *name) {
    int top = g_lua_gettop_fast(L);
    g_lua_getglobal_fast(L, "ResourceSetExists");
    if (g_lua_type_fast(L, -1) != LUA_TFUNCTION_TAG) {
        g_lua_settop_fast(L, top);
        return -1;
    }
    g_lua_pushstring_fast(L, name);
    g_ch2_probe_active = 1;
    int rc = g_lua_pcallk_fast(L, 1, 1, 0, 0, NULL);
    g_ch2_probe_active = 0;
    int result = (rc == 0) ? (g_lua_toboolean_fast(L, -1) ? 1 : 0) : -1;
    g_lua_settop_fast(L, top);
    return result;
}

/* Fallback registration script: descriptor contents of
 * Net/_resdesc_50_Minecraft102_android-pvr.lua with _currentDirectory
 * resolved to the absolute Net/ path. */
static const char k_ch2_register_script[] =
    "local d = \"ux0:data/mcsm/Net/\"\n"
    "local set = {}\n"
    "set.name = \"Minecraft102\"\n"
    "set.setName = \"Minecraft102\"\n"
    "set.descriptionFilenameOverride = \"_resdesc_50_Minecraft102_android-pvr.lua\"\n"
    "set.logicalName = \"<Minecraft102>\"\n"
    "set.logicalDestination = \"<>\"\n"
    "set.priority = 102\n"
    "set.localDir = d\n"
    "set.enableMode = \"bootable\"\n"
    "set.version = \"trunk\"\n"
    "set.descriptionPriority = 0\n"
    "set.gameDataName = \"Minecraft102 Game Data\"\n"
    "set.gameDataPriority = 0\n"
    "set.gameDataEnableMode = \"constant\"\n"
    "set.localDirIncludeBase = true\n"
    "set.localDirRecurse = false\n"
    "set.localDirIncludeOnly = nil\n"
    "set.localDirExclude = { \"_dev/\" }\n"
    "set.gameDataArchives = {\n"
    "  d .. \"MCSM_android-pvr_Minecraft102_txmesh.ttarch2\",\n"
    "  d .. \"MCSM_android_Minecraft102_anichore.ttarch2\",\n"
    "  d .. \"MCSM_android_Minecraft102_data.ttarch2\",\n"
    "  d .. \"MCSM_android_Minecraft102_ms.ttarch2\",\n"
    "  d .. \"MCSM_android_Minecraft102_voice.ttarch2\"\n"
    "}\n"
    "RegisterSetDescription(set)\n";

static void ch2_try_mount_episode2(void *L) {
    static int attempted = 0;
    if (attempted) {
        return;
    }
    attempted = 1;

    if (!ch2_resolve_call_api()) {
        l_warn("CH2: Lua call API unavailable; cannot mount Episode 2 content.");
        return;
    }

    /* 1) The Android path: mount every synced SyncFS content package
     * (reads Temp/IN_* manifests, registers Net/ archives). */
    int rc = ch2_call_global_noargs(L, "NetworkAPICloudMountAllContent");
    int have = ch2_resource_set_exists_probe(L, "Minecraft102");
    l_info("CH2: NetworkAPICloudMountAllContent rc=%d -> Minecraft102 exists=%d", rc, have);
    if (have == 1) {
        g_ch2_mounted_ok = 1;
        return;
    }

    /* 1b) Try the specific content package name. */
    rc = ch2_call_global_1str(L, "NetworkAPICloudMountContent", "Minecraft102_pvr");
    have = ch2_resource_set_exists_probe(L, "Minecraft102");
    l_info("CH2: NetworkAPICloudMountContent('Minecraft102_pvr') rc=%d -> exists=%d", rc, have);
    if (have == 1) {
        g_ch2_mounted_ok = 1;
        return;
    }

    /* 2) Fallback: register the resource-set description directly. */
    rc = ch2_call_global_1str(L, "DoString", k_ch2_register_script);
    have = ch2_resource_set_exists_probe(L, "Minecraft102");
    l_info("CH2: RegisterSetDescription fallback rc=%d -> exists=%d", rc, have);
    if (have == 1) {
        g_ch2_mounted_ok = 1;
    } else {
        l_warn("CH2: Episode 2 content could not be mounted; menu will still show it installed (forced).");
    }
}

/* 2026-07-02 (7th pass): the underlying save DATA path (Create/Load/Save/
 * ResourceExists) is fixed and proven working (SAVEFIXED), but
 * SaveLoad.lua's `getSaveNames()` -- what Menu_Saves.lua's save-slot LIST UI
 * actually calls to enumerate which saves exist -- goes through a DIFFERENT
 * native call: `ResourceLocationGetNames("<User>", pattern)`. This takes the
 * bare LOCATION NAME "<User>" as arg1 (not a "logical:<User>/name" resource
 * path), so redirect_logical_user_to_temp()'s prefix match never catches
 * it. Without this, the save-slot list UI would still show empty even
 * though the files genuinely exist under <Temp> now. Redirect just this one
 * arg the same way: "<User>" -> "<Temp>". */
static void redirect_user_location_arg1(void *L, const char *label) {
    const char *location = lua_arg_string_checked(L, 1);
    if (!location || strcmp(location, "<User>") != 0) {
        return;
    }
    typedef const char *(*LuaPushStringFn3)(void *L, const char *s);
    typedef void (*LuaReplaceFn3)(void *L, int idx);
    static LuaPushStringFn3 s_pushstring;
    static LuaReplaceFn3 s_replace;
    if (!s_pushstring) s_pushstring = (LuaPushStringFn3)so_symbol(&so_mod_gameengine, "lua_pushstring");
    if (!s_replace) s_replace = (LuaReplaceFn3)so_symbol(&so_mod_gameengine, "lua_replace");
    if (s_pushstring && s_replace) {
        static uint32_t count = 0;
        count++;
        if (count <= 32U) {
            l_info("SAVEFIX2: redirecting %s location '<User>' -> '<Temp>' (#%u)", label, count);
        }
        s_pushstring(L, "<Temp>");
        s_replace(L, 1);
    }
}

static so_hook g_hook_lua_resource_location_get_names;
static int hook_lua_resource_location_get_names(void *L) {
    redirect_user_location_arg1(L, "ResourceLocationGetNames");
    return SO_CONTINUE(int, g_hook_lua_resource_location_get_names, L);
}

/* bootTitle.lua's Startup() preloads every <User> save bundle/estore via
 * ResourceLocationGetSymbols("<User>", "*.bundle"/"*.estore") + PreloadAsync
 * -- this is what makes slot bundles RESIDENT so the menu's metadata reads
 * (BundleGetResource on a bundle referenced by name) work. Same redirect. */
static so_hook g_hook_lua_resource_location_get_symbols;
static int hook_lua_resource_location_get_symbols(void *L) {
    redirect_user_location_arg1(L, "ResourceLocationGetSymbols");
    return SO_CONTINUE(int, g_hook_lua_resource_location_get_symbols, L);
}

static so_hook g_hook_lua_resource_set_exists;
static int hook_lua_resource_set_exists(void *L) {
    static uint32_t count = 0;

    /* CH2 mount probe: answer with the engine's REAL state. */
    if (g_ch2_probe_active) {
        return SO_CONTINUE(int, g_hook_lua_resource_set_exists, L);
    }

    const char *name = resource_set_arg_name(L);

    if (resource_set_name_is_vita_ui(name) ||
        resource_set_name_is_other_playstation_ui(name) ||
        resource_set_name_is_xbox_ui(name)) {
        const int value = resource_set_name_is_vita_ui(name) ? 1 : 0;
        count++;
        if (count <= 16U) {
            l_info("FIX: ResourceSetExists('%s') -> %s for controller glyph selection (#%u)",
                   name,
                   value ? "true" : "false",
                   count);
        }
        int ret = lua_push_forced_bool(L, value, "ResourceSetExists");
        if (ret >= 0) {
            return ret;
        }
    }

    if (resource_set_name_is_episode2_local(name)) {
        /* Mount the real Episode 2 content first (one-shot). Once mounted,
         * pass every Episode 2 query through so the engine answers with the
         * real registered state instead of a spoof. */
        if (name && strstr(name, "Minecraft102")) {
            ch2_try_mount_episode2(L);
        }
        if (g_ch2_mounted_ok) {
            return SO_CONTINUE(int, g_hook_lua_resource_set_exists, L);
        }
        count++;
        if (count <= 32U) {
            l_info("CH2: ResourceSetExists('%s') -> true for local Episode 2 payload (#%u)",
                   name,
                   count);
        }
        int ret = lua_push_forced_bool(L, 1, "ResourceSetExists");
        if (ret >= 0) {
            return ret;
        }
    }

    return SO_CONTINUE(int, g_hook_lua_resource_set_exists, L);
}

static so_hook g_hook_lua_resource_set_enabled;
static int hook_lua_resource_set_enabled(void *L) {
    static uint32_t count = 0;
    const char *name = resource_set_arg_name(L);

    if (resource_set_name_is_vita_ui(name) ||
        resource_set_name_is_other_playstation_ui(name) ||
        resource_set_name_is_xbox_ui(name)) {
        const int value = resource_set_name_is_vita_ui(name) ? 1 : 0;
        count++;
        if (count <= 16U) {
            l_info("FIX: ResourceSetEnabled('%s') -> %s for controller glyph selection (#%u)",
                   name,
                   value ? "true" : "false",
                   count);
        }
        int ret = lua_push_forced_bool(L, value, "ResourceSetEnabled");
        if (ret >= 0) {
            return ret;
        }
    }

    if (resource_set_name_is_episode2_local(name)) {
        if (g_ch2_mounted_ok) {
            /* Content really mounted: let the engine answer. */
            return SO_CONTINUE(int, g_hook_lua_resource_set_enabled, L);
        }
        count++;
        if (count <= 32U) {
            l_info("CH2: ResourceSetEnabled('%s') -> true for local Episode 2 payload (#%u)",
                   name,
                   count);
        }
        int ret = lua_push_forced_bool(L, 1, "ResourceSetEnabled");
        if (ret >= 0) {
            return ret;
        }
    }

    return SO_CONTINUE(int, g_hook_lua_resource_set_enabled, L);
}

static so_hook g_hook_lua_property_get;
static int hook_lua_property_get(void *L) {
    /* CHOICES FIX (2026-07-20): redirect bare choice.prop/choicestats.prop reads to
     * <Temp> (same whitelist as ResourceExists) so PropertyGet parses the real 114KB
     * crowd file that ResourceExists already resolves — without this the stats screen
     * is empty after a chapter. Whitelist-scoped: other keys (user.prop, the Licensed
     * gate below, etc.) are unaffected. */
    redirect_logical_user_to_temp(L, 1);
    resolve_lua_str_api();
    if (g_lua_type_fast && g_lua_tolstring_fast && g_lua_gettop_fast) {
        const int top = g_lua_gettop_fast(L);
        for (int i = 1; i <= top; ++i) {
            /* Only inspect real strings; calling lua_tolstring on a number
             * would convert it in place and corrupt the args we pass through. */
            if (g_lua_type_fast(L, i) != LUA_TSTRING_TAG) {
                continue;
            }
            const char *s = g_lua_tolstring_fast(L, i, NULL);
            if (s && strcmp(s, "Licensed") == 0) {
                static uint32_t count = 0;
                count++;
                /* 2026-07-03 CHARACTER-CHOICE FIX. Two DIFFERENT Lua calls read
                 * a "Licensed" key:
                 *   Menu_CharacterSelect_Complete: PropertyGet("user.prop","Licensed")
                 *       -- the INVERTED gate we must force FALSE so it starts the episode.
                 *   IsLicensed() (Utilities.lua):   PropertyGet("user","Licensed")
                 *       -- gates SaveLoad_SetSlotValue/GetSlotValue + EventLog_Start.
                 * Forcing BOTH false during character select made PlayerChoice_Set
                 * write the chosen appearance to the DEMO slot (<Temp>/slot.bundle)
                 * while the episode reads it back from the REAL slot
                 * (saveslot1.bundle) -> choice lost -> selector defaults. Also broke
                 * EventLog_Start (choices). FIX: only force false for the
                 * "user.prop" gate; leave "user" (IsLicensed) genuinely TRUE so
                 * slot values + the event log always use the real, persistent slot.
                 * The property-set name is arg 1; the "Licensed" key is arg 2. */
                const char *pset = (g_lua_type_fast(L, 1) == LUA_TSTRING_TAG)
                                       ? g_lua_tolstring_fast(L, 1, NULL)
                                       : NULL;
                const int is_userprop_gate = pset && strcmp(pset, "user.prop") == 0;
                int force_demo_path = (g_character_select_license_active != 0) && is_userprop_gate;
                if (g_character_select_license_active && is_userprop_gate) {
                    if (g_character_select_license_safety > 0) {
                        g_character_select_license_safety--;
                    }
                    if (g_character_select_license_safety == 0) {
                        /* Safety cap hit (user backed out without a subproject
                         * switch, or something is wrong) -- disarm so we never
                         * lie to Licensed forever. */
                        clear_character_select_license_window();
                    }
                }
                const int value = force_demo_path ? 0 : 1;
                if (count <= 24U || (count & 0x7fU) == 0U) {
                    l_info("FULLGAME: PropertyGet(\"Licensed\") -> %s%s (#%u active=%d safety=%u)",
                           value ? "true" : "false",
                           force_demo_path ? " (character-select window)" : "",
                           count,
                           g_character_select_license_active,
                           g_character_select_license_safety);
                }
                /* Bytecode of Menu_CharacterSelect_Complete (Lua 5.2 disasm):
                 *   r1 = PropertyGet("user.prop","Licensed")
                 *   TEST r1, 1 ; JMP ->menu   -- if r1 TRUE  -> SubProject_Switch("Menu_Main")
                 *   Menu_StartEpisode(1.0)     -- if r1 FALSE -> START THE EPISODE
                 * i.e. the gate is INVERTED vs intuition: the licensed/full build
                 * returns to the main menu after character select, the "demo" path
                 * jumps straight into Episode 1. Keep the main menu licensed/full,
                 * but force FALSE only for the character-select confirmation window
                 * so Menu_StartEpisode still reaches the episode resource set. */
                int ret = lua_push_forced_bool(L, value, "Licensed");
                if (ret >= 0) {
                    return ret;
                }
                break;
            }
        }
    }
    return SO_CONTINUE(int, g_hook_lua_property_get, L);
}

/* FIX (2026-06-21): on Android the platform (Google Play Games) silently
 * reports the user as SIGNED IN, so the Telltale account login auto-succeeds and
 * there is NO login prompt. On Vita our platform stubs report "not signed in",
 * so the boot login flow (bootTitle -> UserManager_LogIn) tries to sign the user
 * in (PlatformRequestSignIn / ShowSignInUI) and hangs forever on "Checking For
 * DLC", never reaching SubProject_Switch("Menu"). Emulate the phone: report the
 * platform user as already signed in so login completes and boot proceeds.
 * (Replaces the earlier IsToolBuild->true bypass, which was too broad and
 * derailed boot to a black loading screen.) */
static so_hook g_hook_lua_platform_is_user_signed_in;
static int hook_lua_platform_is_user_signed_in(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 8U) l_info("FIX: luaPlatformIsUserSignedIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "PlatformIsUserSignedIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_user_signed_in, L);
}
static so_hook g_hook_lua_is_user_signed_in;
static int hook_lua_is_user_signed_in(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 8U) l_info("FIX: luaIsUserSignedIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsUserSignedIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_user_signed_in, L);
}
/* If the flow still requests a sign-in despite the above, report success
 * immediately instead of waiting for a sign-in UI that never completes. */
static so_hook g_hook_lua_platform_request_sign_in;
static int hook_lua_platform_request_sign_in(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 8U) l_info("FIX: luaPlatformRequestSignIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "PlatformRequestSignIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_request_sign_in, L);
}

/* Prompt/platform identity: the Android binary contains the official Vita Lua
 * platform probe, but it returns false by default. Make scripts that branch on
 * Vita see the native handheld identity without changing TTPlatform's platform
 * enum, which still controls Android resource loading. */
static so_hook g_hook_lua_is_engine_vita;
static int hook_lua_is_engine_vita(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsEngineVita -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsEngineVita");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_vita, L);
}

static so_hook g_hook_lua_is_joystick_xbox;
static int hook_lua_is_joystick_xbox(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsJoystickXbox -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsJoystickXbox");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_joystick_xbox, L);
}

static so_hook g_hook_lua_is_engine_xbox360;
static int hook_lua_is_engine_xbox360(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsEngineXbox360 -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEngineXbox360");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_xbox360, L);
}

static so_hook g_hook_lua_is_engine_xbone;
static int hook_lua_is_engine_xbone(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsEngineXBOne -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEngineXBOne");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_xbone, L);
}

static so_hook g_hook_lua_is_engine_ps3;
static int hook_lua_is_engine_ps3(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsEnginePS3 -> false (Vita-only device identity) (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEnginePS3");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_ps3, L);
}

static so_hook g_hook_lua_is_engine_ps4;
static int hook_lua_is_engine_ps4(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaIsEnginePS4 -> false (Vita-only device identity) (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEnginePS4");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_ps4, L);
}

static so_hook g_hook_lua_input_has_joystick;
static int hook_lua_input_has_joystick(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaInputHasJoystick -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputHasJoystick");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_has_joystick, L);
}

static so_hook g_hook_lua_input_supports_joystick;
static int hook_lua_input_supports_joystick(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaInputSupportsJoystick -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputSupportsJoystick");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_supports_joystick, L);
}

static so_hook g_hook_lua_input_is_joystick_enabled;
static int hook_lua_input_is_joystick_enabled(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 12U) l_info("FIX: luaInputIsJoystickEnabled -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputIsJoystickEnabled");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_is_joystick_enabled, L);
}

static so_hook g_hook_lua_input_has_touch;
static int hook_lua_input_has_touch(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 8U) l_info("FIX: luaInputHasTouch -> true while controller is active (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputHasTouch");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_has_touch, L);
}

static so_hook g_hook_lua_input_supports_touch;
static int hook_lua_input_supports_touch(void *L) {
    static uint32_t count = 0;
    count++;
    if (count <= 8U) l_info("FIX: luaInputSupportsTouch -> true while controller is active (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputSupportsTouch");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_supports_touch, L);
}

/* CONNECT-FLOW INSTRUMENTATION (2026-06-22): after sign-in the boot script runs
 * the "Connecting"/"Checking for DLC" sequence (ConnectedContentManager_Update ->
 * Update*AndWait -> Upload*ToServer -> SubProject_Switch(Menu)). These *AndWait
 * functions block on Telltale server responses that never come on Vita. Trace
 * ENTER/RETURN on each so the log shows EXACTLY which one never returns (the
 * blocker). Pure logging via SO_CONTINUE — no behaviour change. ret is logged so
 * polled status queries reveal what value they keep returning. */
#define CONNECT_TRACE_HOOK(fn)                                                  \
    static so_hook g_hook_##fn;                                                 \
    static int hook_##fn(void *L) {                                             \
        static uint32_t c = 0; c++;                                            \
        l_info("CONNECT-TRACE: " #fn " ENTER #%u", c);                          \
        int r = SO_CONTINUE(int, g_hook_##fn, L);                              \
        l_info("CONNECT-TRACE: " #fn " RETURN #%u ret=%d", c, r);               \
        return r;                                                              \
    }
CONNECT_TRACE_HOOK(luaUpdateProfileAndWait)
CONNECT_TRACE_HOOK(luaUpdateStatsAndWait)
CONNECT_TRACE_HOOK(luaUpdateAchievementsAndWait)
CONNECT_TRACE_HOOK(luaUpdateFriendsAndWait)
CONNECT_TRACE_HOOK(luaGetConnectionStatus)
CONNECT_TRACE_HOOK(luaBeginUpdateEpisodes)
CONNECT_TRACE_HOOK(luaIsEpisodesUpdateAvailable)
CONNECT_TRACE_HOOK(luaLocalContentEnumerationReady)
CONNECT_TRACE_HOOK(luaUploadPendingObjectsToServer)
CONNECT_TRACE_HOOK(luaUploadCachedObjectToServer)
/* Episode-launch path (the real "start episode" C bindings). Menu_StartEpisode ->
 * SubProject_StartEpisode -> these. Tracing ENTER/RETURN pinpoints exactly which
 * step fails when confirming a character bounces back to the menu. */
CONNECT_TRACE_HOOK(luaNewGame)
CONNECT_TRACE_HOOK(luaMountEpisode)
CONNECT_TRACE_HOOK(luaInstallEpisode)
CONNECT_TRACE_HOOK(luaMountAllEpisodes)

// The boot gate: GameEngine_Start only loads _boot.lua if GameEngine::Initialize
// (-> Initialize2) returns non-zero. Initialize2's success hinges on
// ScriptManager::DoLoad. These three hooks capture exactly where the gate fails.
// All run once on the main thread during boot -> safe.
static so_hook g_hook_ge_init2;
static so_hook g_hook_sm_load;
static so_hook g_hook_sm_doload;

static int hook_ge_init2(void *arg) {
    static uint64_t first_ms = 0;
    if (!first_ms) first_ms = now_ms();
    l_info("Diag: GameEngine::Initialize2 ENTER arg=%p", arg);
    int ret = SO_CONTINUE(int, g_hook_ge_init2, arg);
    l_info("Diag: GameEngine::Initialize2 RETURNED %d (0 => _boot.lua will NOT load)", ret);
    return ret;
}

static int hook_sm_load(void *str, int b) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    if (!first_ms) first_ms = now_ms();
    l_info("Diag: ScriptManager::Load ENTER #%u (boot script load reached!) b=%d", count, b);
    int ret = SO_CONTINUE(int, g_hook_sm_load, str, b);
    l_info("Diag: ScriptManager::Load RETURNED %d", ret);
    return ret;
}

static int hook_sm_doload(void *str) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    if (!first_ms) first_ms = now_ms();
    l_info("Diag: ScriptManager::DoLoad ENTER #%u", count);
    int ret = SO_CONTINUE(int, g_hook_sm_doload, str);
    l_info("Diag: ScriptManager::DoLoad RETURNED %d", ret);
    return ret;
}

// Logs the script NAME each load requests and the raw return value. The return
// value is engine-private; resource success/failure must be judged with the
// surrounding ALOG resource messages and OBB I/O, not this value alone.
static so_hook g_hook_sm_loadresource;

static int dlc_resource_index(const char *name) {
    if (!name) {
        return -1;
    }
    if (strstr(name, "DLCStatus.lua")) {
        return 0;
    }
    if (strstr(name, "DownloadManager.lua")) {
        return 1;
    }
    if (strstr(name, "PurchaseManager.lua")) {
        return 2;
    }
    return -1;
}

static const char *dlc_resource_name_for_index(int index) {
    switch (index) {
        case 0: return "DLCStatus.lua";
        case 1: return "DownloadManager.lua";
        case 2: return "PurchaseManager.lua";
        default: return "(unknown)";
    }
}

static int hook_sm_loadresource(void *L, const char *name) {
    static uint32_t count = 0;
    static uint32_t dlc_load_counts[3];
    count++;

    const int dlc_index = dlc_resource_index(name);

    /* Show the asset on the loading screen BEFORE the (blocking) load. */
    if (name) LS_SET_ASSET(name);
    launch_state_mark_progress();
    LS_TICK();
    int ret = SO_CONTINUE(int, g_hook_sm_loadresource, L, name);
    launch_state_mark_progress();
    if (dlc_index >= 0) {
        dlc_load_counts[dlc_index]++;
        if (mcsm_mega_diag_enabled() &&
            (dlc_load_counts[dlc_index] <= 16U || (dlc_load_counts[dlc_index] & 0x3fU) == 0U)) {
            l_info("Diag: ScriptManager::LoadResource DLC script load key=%s load=%u L=%p ret=%d",
                   dlc_resource_name_for_index(dlc_index),
                   dlc_load_counts[dlc_index],
                   L,
                   ret);
        }
    }
    if (mcsm_mega_diag_enabled()) {
        l_info("Diag: ScriptManager::LoadResource #%u L=%p name='%s' ret=%d (raw engine result; not decoded)",
               count, L, name ? name : "(null)", ret);
    }
    return ret;
}

static int hook_job_init(void) {
    static uint32_t count = 0;
    static uint64_t first_ms = 0;
    count++;
    launch_state_mark_progress();
    if (!first_ms) first_ms = now_ms();
    log_diag_counter("JobScheduler::Initialize", count, first_ms, NULL);
    return SO_CONTINUE(int, g_hook_job_init);
}


/* ---- SHADOW DISABLE (perf) --------------------------------------------------
 * Shadows render the scene geometry a SECOND time (a depth pass) + cost CPU in
 * Scene::PrepareToRenderShadows — a big chunk of the 488k-vert/100ms-CPU heavy
 * frames. Hooking LightInstance::IsShadowLight / IsContributingShadowLight to
 * report FALSE makes the engine treat no light as a shadow caster, so it skips
 * the whole shadow setup + pass. Opt-in via ux0:data/mcsm/no_shadows.txt (visual
 * trade: objects lose cast shadows). */
static int shadows_disabled(void) { return !mcsm_cfg()->shadows; }
static so_hook g_hook_is_shadow_light;
static int hook_is_shadow_light(void *self) {
    if (shadows_disabled()) return 0;
    return SO_CONTINUE(int, g_hook_is_shadow_light, self);
}
static so_hook g_hook_is_contributing_shadow;
static int hook_is_contributing_shadow(void *self) {
    if (shadows_disabled()) return 0;
    return SO_CONTINUE(int, g_hook_is_contributing_shadow, self);
}

static void patch_boot_diag_hooks(void) {
    if (shadows_disabled()) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13LightInstance13IsShadowLightEv",
                                  "LightInstance::IsShadowLight",
                                  (uintptr_t)&hook_is_shadow_light, &g_hook_is_shadow_light);
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13LightInstance25IsContributingShadowLightEv",
                                  "LightInstance::IsContributingShadowLight",
                                  (uintptr_t)&hook_is_contributing_shadow, &g_hook_is_contributing_shadow);
        l_info("PERF: shadows DISABLED (no_shadows.txt) — shadow geometry pass skipped.");
    }
#if ENABLE_UNSAFE_ARCHIVE_DIAG_HOOKS
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN23ResourceDirectory_Posix12OpenResourceERK6Symbol18ResourceAccessType16ResourceOpenType",
                              "ResourceDirectory_Posix::OpenResource",
                              (uintptr_t)&hook_posix_open, &g_hook_posix_open);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN22ResourceDynamicArchive12OpenResourceERK6Symbol18ResourceAccessType16ResourceOpenType",
                              "ResourceDynamicArchive::OpenResource",
                              (uintptr_t)&hook_dynarch_open, &g_hook_dynarch_open);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTArchive28ActivateE3PtrI10DataStreamE",
                              "TTArchive2::Activate",
                              (uintptr_t)&hook_ttarch_activate, &g_hook_ttarch_activate);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTArchive24LoadERK3PtrI10DataStreamE",
                              "TTArchive2::Load",
                              (uintptr_t)&hook_ttarch_load, &g_hook_ttarch_load);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTArchive211HasResourceERK6Symbol",
                              "TTArchive2::HasResource",
                              (uintptr_t)&hook_ttarch_has, &g_hook_ttarch_has);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTArchive213_FindResourceERK6Symbol",
                              "TTArchive2::_FindResource",
                              (uintptr_t)&hook_ttarch_findres, &g_hook_ttarch_findres);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN24ResourceConcreteLocation29FindLocationByResourceAddressERK15ResourceAddress",
                              "ResourceConcreteLocation::FindLocationByResourceAddress",
                              (uintptr_t)&hook_findloc, &g_hook_findloc);
#else
    l_info("Patch: skipped hot archive diag hooks (unsafe with concurrent resource/render threads).");
#endif
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13ScriptManager10InitializeEbb",
                              "ScriptManager::Initialize",
                              (uintptr_t)&hook_scriptmgr_init, &g_hook_scriptmgr_init);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z40luaRegisterResourceDescriptionWithEngineP9lua_State",
                              "luaRegisterResourceDescriptionWithEngine",
                              (uintptr_t)&hook_lua_register_resdesc, &g_hook_lua_register_resdesc);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z30luaResourceRetryFailedResDescsP9lua_State",
                              "luaResourceRetryFailedResDescs",
                              (uintptr_t)&hook_lua_retry_resdesc, &g_hook_lua_retry_resdesc);
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN12JobScheduler10InitializeEv",
                              "JobScheduler::Initialize",
                              (uintptr_t)&hook_job_init, &g_hook_job_init);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z13lua_LoadAsyncP9lua_State",
                              "lua_LoadAsync",
                              (uintptr_t)&hook_lua_loadasync, &g_hook_lua_loadasync);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z15luaPreloadAsyncP9lua_State",
                              "luaPreloadAsync",
                              (uintptr_t)&hook_lua_preloadasync, &g_hook_lua_preloadasync);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z20luaResourceSetCreateP9lua_State",
                              "luaResourceSetCreate",
                              (uintptr_t)&hook_lua_rset_create, &g_hook_lua_rset_create);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z20luaResourceSetEnableP9lua_State",
                              "luaResourceSetEnable",
                              (uintptr_t)&hook_lua_rset_enable, &g_hook_lua_rset_enable);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z21luaResourceSetDisableP9lua_State",
                              "luaResourceSetDisable",
                              (uintptr_t)&hook_lua_resource_set_disable,
                              &g_hook_lua_resource_set_disable);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z25luaResourceSetLoadingCallP9lua_State",
                              "luaResourceSetLoadingCall",
                              (uintptr_t)&hook_lua_rset_loadingcall, &g_hook_lua_rset_loadingcall);
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN10GameEngine11Initialize2EPKc",
                              "GameEngine::Initialize2",
                              (uintptr_t)&hook_ge_init2, &g_hook_ge_init2);
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13ScriptManager4LoadERK6Stringb",
                              "ScriptManager::Load",
                              (uintptr_t)&hook_sm_load, &g_hook_sm_load);
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13ScriptManager6DoLoadERK6String",
                              "ScriptManager::DoLoad",
                              (uintptr_t)&hook_sm_doload, &g_hook_sm_doload);
    (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13ScriptManager12LoadResourceEP9lua_StatePKc",
                              "ScriptManager::LoadResource",
                              (uintptr_t)&hook_sm_loadresource, &g_hook_sm_loadresource);
}

/* SAVE-RENAME KEYBOARD (2026-07-18): the game calls luaPlatformShowKeyboard when
 * it wants text entry (renaming a save). On Android that pops the OS soft keyboard;
 * on Vita it did nothing. Hook it to raise the Vita IME (dialog.c mcsm_ime_begin);
 * gl_swap pumps it and feeds the typed name back to the engine. */
static so_hook g_hook_lua_platform_show_keyboard;
static int hook_lua_platform_show_keyboard(void *L) {
    extern void mcsm_ime_begin(const char *initial);
    l_info("KEYBOARD: luaPlatformShowKeyboard -> Vita IME");
    mcsm_ime_begin("");
    return SO_CONTINUE(int, g_hook_lua_platform_show_keyboard, L);
}

/* ENGINE VIRTUAL-KEYBOARD BRIDGE (2026-07-20). TTPlatform::{Open,IsFinished,GetResult}
 * VirtualKeyboard are vtable STUBS (Open=nop, IsFinished=return 1, GetResult=nop —
 * confirmed via the TTPlatform vtables in libGameEngine .data.rel.ro), so the engine's
 * rename flow instantly "finished" with an empty name. Replace them with a bridge to
 * the real Vita IME (dialog.c). We DON'T call the originals (they do nothing). */
extern void mcsm_ime_begin_vkbd(const char *initial);
extern int mcsm_vkbd_finished(void);
extern const char *mcsm_vkbd_result(int *cancelled);
extern void mcsm_vkbd_reset(void);
static so_hook g_hook_open_vkbd, g_hook_is_vkbd_finished, g_hook_get_vkbd_result;

/* OpenVirtualKeyboard(String const& title, String const& initial, bool, int, bool).
 * We ignore the engine args (no need to read the Telltale String layout) and raise
 * an empty IME. Return 1 in case the caller checks a success bool. */
static int hook_open_virtual_keyboard(void) {
    l_info("KEYBOARD: TTPlatform::OpenVirtualKeyboard -> Vita IME");
    mcsm_ime_begin_vkbd("");
    return 1;
}
/* IsVirtualKeyboardFinished() -> 0 while the IME is up, 1 when the user confirms/
 * cancels (the stub always returned 1 so the engine never waited for input). */
static int hook_is_virtual_keyboard_finished(void) {
    return mcsm_vkbd_finished();
}
/* GetVirtualKeyboardResult(String& out, bool& cancelled). Write the typed name into
 * `out` via placement destruct + String(const char*) construct (== what operator=
 * does internally), and set `cancelled`. Runs on the engine/sim thread that called it. */
typedef void  (*mcsm_string_dtor_fn)(void *self);
typedef void *(*mcsm_string_ctor_cstr_fn)(void *self, const char *s);
static void hook_get_virtual_keyboard_result(void *self, void *out_string, unsigned char *cancelled) {
    (void)self;
    static mcsm_string_dtor_fn s_dtor; static mcsm_string_ctor_cstr_fn s_ctor;
    if (!s_dtor) s_dtor = (mcsm_string_dtor_fn)so_symbol(&so_mod_gameengine, "_ZN6StringD1Ev");
    if (!s_ctor) s_ctor = (mcsm_string_ctor_cstr_fn)so_symbol(&so_mod_gameengine, "_ZN6StringC1EPKc");
    int cancel = 0;
    const char *name = mcsm_vkbd_result(&cancel);
    if (out_string && s_dtor && s_ctor) {
        s_dtor(out_string);                     /* free the out String's current buffer */
        s_ctor(out_string, name ? name : "");   /* reconstruct it from the typed name */
    }
    if (cancelled) *cancelled = (unsigned char)(cancel ? 1 : 0);
    l_info("KEYBOARD: GetVirtualKeyboardResult -> '%s' cancelled=%d", name ? name : "", cancel);
    mcsm_vkbd_reset();
}
/* The Vita has no HW keyboard, so luaInputSupportsKeyboard/HasKeyboard return
 * FALSE and the engine never offers text entry (rename) -> ShowKeyboard is never
 * called. Force them TRUE so the engine routes text entry through the keyboard,
 * which our ShowKeyboard hook then turns into the Vita IME. Also write a diag so
 * we can SEE which of these the rename path actually hits. */
static so_hook g_hook_lua_input_supports_keyboard;
static int hook_lua_input_supports_keyboard(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "InputSupportsKeyboard", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_supports_keyboard, L);
}
static so_hook g_hook_lua_input_has_keyboard;
static int hook_lua_input_has_keyboard(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 1, "InputHasKeyboard", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_has_keyboard, L);
}
/* RENAME TRACE (2026-07-18): none of the OS/SDL keyboard triggers fire on rename,
 * so trace the CONFIRMED rename setter to see if the rename flow is even reached.
 * If this fires when you rename, the flow works up to the setter (so text got in
 * somehow) — and I can hook whatever raised the text field. Log-only. */
static so_hook g_hook_lua_saveload_set_display_name;
static int hook_lua_saveload_set_display_name(void *L) {
    return SO_CONTINUE(int, g_hook_lua_saveload_set_display_name, L);
}

static void patch_dlc_fast_path_hooks(void) {
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z29luaSaveLoadSetSaveDisplayNameP9lua_State",
                              "luaSaveLoadSetSaveDisplayName",
                              (uintptr_t)&hook_lua_saveload_set_display_name,
                              &g_hook_lua_saveload_set_display_name);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z24luaInputSupportsKeyboardP9lua_State",
                              "luaInputSupportsKeyboard",
                              (uintptr_t)&hook_lua_input_supports_keyboard,
                              &g_hook_lua_input_supports_keyboard);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z19luaInputHasKeyboardP9lua_State",
                              "luaInputHasKeyboard",
                              (uintptr_t)&hook_lua_input_has_keyboard,
                              &g_hook_lua_input_has_keyboard);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z23luaPlatformShowKeyboardP9lua_State",
                              "luaPlatformShowKeyboard",
                              (uintptr_t)&hook_lua_platform_show_keyboard,
                              &g_hook_lua_platform_show_keyboard);
    /* ENGINE VIRTUAL-KEYBOARD BRIDGE: the vtable-dispatched rename path (the stubs
     * are what actually run on rename; the Lua ShowKeyboard above may not fire). */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTPlatform19OpenVirtualKeyboardERK6StringS2_bib",
                              "TTPlatform::OpenVirtualKeyboard",
                              (uintptr_t)&hook_open_virtual_keyboard, &g_hook_open_vkbd);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTPlatform25IsVirtualKeyboardFinishedEv",
                              "TTPlatform::IsVirtualKeyboardFinished",
                              (uintptr_t)&hook_is_virtual_keyboard_finished, &g_hook_is_vkbd_finished);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTPlatform24GetVirtualKeyboardResultER6StringRb",
                              "TTPlatform::GetVirtualKeyboardResult",
                              (uintptr_t)&hook_get_virtual_keyboard_result, &g_hook_get_vkbd_result);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z32luaPlatformIsConnectedToInternetP9lua_State",
                              "luaPlatformIsConnectedToInternet",
                              (uintptr_t)&hook_lua_platform_is_connected_to_internet,
                              &g_hook_lua_platform_is_connected_to_internet);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z37luaPlatformIsConnectedToLicenseServerP9lua_State",
                              "luaPlatformIsConnectedToLicenseServer",
                              (uintptr_t)&hook_lua_platform_is_connected_to_license_server,
                              &g_hook_lua_platform_is_connected_to_license_server);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z26luaPlatformIsAgeRestrictedP9lua_State",
                              "luaPlatformIsAgeRestricted",
                              (uintptr_t)&hook_lua_platform_is_age_restricted,
                              &g_hook_lua_platform_is_age_restricted);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z25luaBeginOffersEnumerationP9lua_State",
                              "luaBeginOffersEnumeration",
                              (uintptr_t)&hook_lua_begin_offers_enumeration,
                              &g_hook_lua_begin_offers_enumeration);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z25luaOffersEnumerationReadyP9lua_State",
                              "luaOffersEnumerationReady",
                              (uintptr_t)&hook_lua_offers_enumeration_ready,
                              &g_hook_lua_offers_enumeration_ready);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z21luaIsEpisodeAvailableP9lua_State",
                              "luaIsEpisodeAvailable",
                              (uintptr_t)&hook_lua_is_episode_available,
                              &g_hook_lua_is_episode_available);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z21luaIsEpisodePurchasedP9lua_State",
                              "luaIsEpisodePurchased",
                              (uintptr_t)&hook_lua_is_episode_purchased,
                              &g_hook_lua_is_episode_purchased);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z22luaIsEpisodeDownloadedP9lua_State",
                              "luaIsEpisodeDownloaded",
                              (uintptr_t)&hook_lua_is_episode_downloaded,
                              &g_hook_lua_is_episode_downloaded);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z22luaIsEpisodeUnlicensedP9lua_State",
                              "luaIsEpisodeUnlicensed",
                              (uintptr_t)&hook_lua_is_episode_unlicensed,
                              &g_hook_lua_is_episode_unlicensed);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z24luaGetUserSystemLanguageP9lua_State",
                              "luaGetUserSystemLanguage",
                              (uintptr_t)&hook_lua_get_user_system_language,
                              &g_hook_lua_get_user_system_language);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z14luaGetDemoModeP9lua_State",
                              "luaGetDemoMode",
                              (uintptr_t)&hook_lua_get_demo_mode,
                              &g_hook_lua_get_demo_mode);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaGetDemoTimeoutP9lua_State",
                              "luaGetDemoTimeout",
                              (uintptr_t)&hook_lua_get_demo_timeout,
                              &g_hook_lua_get_demo_timeout);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z26luaPlatformGetTrialTimeoutP9lua_State",
                              "luaPlatformGetTrialTimeout",
                              (uintptr_t)&hook_lua_platform_get_trial_timeout,
                              &g_hook_lua_platform_get_trial_timeout);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z31luaPlatformCanUserMakePurchasesP9lua_State",
                              "luaPlatformCanUserMakePurchases",
                              (uintptr_t)&hook_lua_platform_can_user_make_purchases,
                              &g_hook_lua_platform_can_user_make_purchases);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z31luaPlatformIsUserSpaceAvailableP9lua_State",
                              "luaPlatformIsUserSpaceAvailable",
                              (uintptr_t)&hook_lua_platform_is_user_space_available,
                              &g_hook_lua_platform_is_user_space_available);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z20luaIsStorageSelectedP9lua_State",
                              "luaIsStorageSelected",
                              (uintptr_t)&hook_lua_is_storage_selected,
                              &g_hook_lua_is_storage_selected);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z28luaSaveLoadHasAvailableSpaceP9lua_State",
                              "luaSaveLoadHasAvailableSpace",
                              (uintptr_t)&hook_lua_save_load_has_available_space,
                              &g_hook_lua_save_load_has_available_space);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z27luaPlatformGetFreeDiskSpaceP9lua_State",
                              "luaPlatformGetFreeDiskSpace",
                              (uintptr_t)&hook_lua_platform_get_free_disk_space,
                              &g_hook_lua_platform_get_free_disk_space);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z30luaStorageDeviceErrorOnRemovalP9lua_State",
                              "luaStorageDeviceErrorOnRemoval",
                              (uintptr_t)&hook_lua_storage_device_error_on_removal,
                              &g_hook_lua_storage_device_error_on_removal);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z20luaIsSaveGameCorruptP9lua_State",
                              "luaIsSaveGameCorrupt",
                              (uintptr_t)&hook_lua_is_save_game_corrupt,
                              &g_hook_lua_is_save_game_corrupt);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z33luaFileIsLastErrorCorruptSaveFileP9lua_State",
                              "luaFileIsLastErrorCorruptSaveFile",
                              (uintptr_t)&hook_lua_file_is_last_error_corrupt_save_file,
                              &g_hook_lua_file_is_last_error_corrupt_save_file);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameEngine15GetTrialVersionEv",
                              "GameEngine::GetTrialVersion",
                              (uintptr_t)&hook_gameengine_get_trial_version,
                              &g_hook_gameengine_get_trial_version);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10GameEngine21GetTrialVersionSecureEv",
                              "GameEngine::GetTrialVersionSecure",
                              (uintptr_t)&hook_gameengine_get_trial_version_secure,
                              &g_hook_gameengine_get_trial_version_secure);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTPlatform14IsTrialVersionEv",
                              "TTPlatform::IsTrialVersion",
                              (uintptr_t)&hook_ttplatform_is_trial_version,
                              &g_hook_ttplatform_is_trial_version);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN10TTPlatform20IsUserSpaceAvailableEv",
                              "TTPlatform::IsUserSpaceAvailable",
                              (uintptr_t)&hook_ttplatform_is_user_space_available,
                              &g_hook_ttplatform_is_user_space_available);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16Platform_Android20IsUserSpaceAvailableEv",
                              "Platform_Android::IsUserSpaceAvailable",
                              (uintptr_t)&hook_platform_android_is_user_space_available,
                              &g_hook_platform_android_is_user_space_available);
    /* New Game / episode-start gate: keep menu licensing full-game, but scope
     * the known inverted Licensed=false branch to character-select confirmation.
     * Also trace the SubProject_Switch target. */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z14luaPropertyGetP9lua_State",
                              "luaPropertyGet",
                              (uintptr_t)&hook_lua_property_get,
                              &g_hook_lua_property_get);
    /* CHOICES FIX: PropertyExists("choice.prop","Options") must resolve to <Temp>
     * too, or the stats screen's existence gate fails even after PropertyGet works. */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaPropertyExistsP9lua_State",
                              "luaPropertyExists",
                              (uintptr_t)&hook_lua_property_exists,
                              &g_hook_lua_property_exists);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z16luaSetSubProjectP9lua_State",
                              "luaSetSubProject",
                              (uintptr_t)&hook_lua_set_subproject,
                              &g_hook_lua_set_subproject);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z12luaResetGameP9lua_State",
                              "luaResetGame",
                              (uintptr_t)&hook_lua_reset_game,
                              &g_hook_lua_reset_game);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z18luaSaveLoadPreSaveP9lua_State",
                              "luaSaveLoadPreSave",
                              (uintptr_t)&hook_lua_saveload_presave,
                              &g_hook_lua_saveload_presave);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z19luaSaveLoadPostSaveP9lua_State",
                              "luaSaveLoadPostSave",
                              (uintptr_t)&hook_lua_saveload_postsave,
                              &g_hook_lua_saveload_postsave);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z19luaSaveGameToBundleP9lua_State",
                              "luaSaveGameToBundle",
                              (uintptr_t)&hook_lua_save_game_to_bundle,
                              &g_hook_lua_save_game_to_bundle);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z26luaSetSaveFinishedCallbackP9lua_State",
                              "luaSetSaveFinishedCallback",
                              (uintptr_t)&hook_lua_set_save_finished_cb,
                              &g_hook_lua_set_save_finished_cb);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z7luaSaveP9lua_State",
                              "luaSave",
                              (uintptr_t)&hook_lua_save,
                              &g_hook_lua_save);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z9luaCreateP9lua_State",
                              "luaCreate",
                              (uintptr_t)&hook_lua_create,
                              &g_hook_lua_create);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaResourceExistsP9lua_State",
                              "luaResourceExists",
                              (uintptr_t)&hook_lua_resource_exists_redirect,
                              &g_hook_lua_resource_exists_redirect);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z27luaResourceLocationGetNamesP9lua_State",
                              "luaResourceLocationGetNames",
                              (uintptr_t)&hook_lua_resource_location_get_names,
                              &g_hook_lua_resource_location_get_names);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z29luaResourceLocationGetSymbolsP9lua_State",
                              "luaResourceLocationGetSymbols",
                              (uintptr_t)&hook_lua_resource_location_get_symbols,
                              &g_hook_lua_resource_location_get_symbols);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z15luaResourceCopyP9lua_State",
                              "luaResourceCopy",
                              (uintptr_t)&hook_lua_resource_copy,
                              &g_hook_lua_resource_copy);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z20luaBundleGetResourceP9lua_State",
                              "luaBundleGetResource",
                              (uintptr_t)&hook_lua_bundle_get_resource,
                              &g_hook_lua_bundle_get_resource);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z23luaBundleCreateResourceP9lua_State",
                              "luaBundleCreateResource",
                              (uintptr_t)&hook_lua_bundle_create_resource,
                              &g_hook_lua_bundle_create_resource);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z21luaBundleGetResourcesP9lua_State",
                              "luaBundleGetResources",
                              (uintptr_t)&hook_lua_bundle_get_resources,
                              &g_hook_lua_bundle_get_resources);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z23luaBundleRemoveResourceP9lua_State",
                              "luaBundleRemoveResource",
                              (uintptr_t)&hook_lua_bundle_remove_resource,
                              &g_hook_lua_bundle_remove_resource);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z9luaUnloadP9lua_State",
                              "luaUnload",
                              (uintptr_t)&hook_lua_unload,
                              &g_hook_lua_unload);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaResourceDeleteP9lua_State",
                              "luaResourceDelete",
                              (uintptr_t)&hook_lua_resource_delete,
                              &g_hook_lua_resource_delete);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z16luaQueryEventLogP9lua_State",
                              "luaQueryEventLog",
                              (uintptr_t)&hook_lua_query_event_log,
                              &g_hook_lua_query_event_log);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z25luaResourceSetNonPurgableP9lua_State",
                              "luaResourceSetNonPurgable",
                              (uintptr_t)&hook_lua_resource_set_nonpurgable,
                              &g_hook_lua_resource_set_nonpurgable);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z28luaDeleteAllEventsAfterEventP9lua_State",
                              "luaDeleteAllEventsAfterEvent",
                              (uintptr_t)&hook_lua_delete_all_events_after,
                              &g_hook_lua_delete_all_events_after);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaEventLogCreateP9lua_State",
                              "luaEventLogCreate",
                              (uintptr_t)&hook_lua_event_log_create,
                              &g_hook_lua_event_log_create);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z38luaSaveDownloadedDocumentAsPropertySetP9lua_State",
                              "luaSaveDownloadedDocumentAsPropertySet",
                              (uintptr_t)&hook_lua_save_downloaded_doc_as_propset,
                              &g_hook_lua_save_downloaded_doc_as_propset);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z27luaDownloadDocumentRetrieveP9lua_State",
                              "luaDownloadDocumentRetrieve",
                              (uintptr_t)&hook_lua_download_doc_retrieve,
                              &g_hook_lua_download_doc_retrieve);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z30luaDownloadDocumentsFromServerP9lua_State",
                              "luaDownloadDocumentsFromServer",
                              (uintptr_t)&hook_lua_download_docs_from_server,
                              &g_hook_lua_download_docs_from_server);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z31luaSetDownloadCompletedCallbackP9lua_State",
                              "luaSetDownloadCompletedCallback",
                              (uintptr_t)&hook_lua_set_download_completed_cb,
                              &g_hook_lua_set_download_completed_cb);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z12luaSavePrefsP9lua_State",
                              "luaSavePrefs",
                              (uintptr_t)&hook_lua_save_prefs,
                              &g_hook_lua_save_prefs);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z20luaResourceSetEnableP9lua_State",
                              "luaResourceSetEnable",
                              (uintptr_t)&hook_lua_resource_set_enable,
                              &g_hook_lua_resource_set_enable);
    /* SAVEFIX spoof hooks (ResourceLocationGetNames/ResourceGetNames/
     * ResourceExists forcing fake "saveSlot1.bundle" answers) are DISARMED
     * (2026-07-02). With the character-select Licensed window narrowed to a
     * single read, SaveLoad.lua takes its real licensed path and creates
     * <User>/saveSlot1.bundle + per-save sub-bundles + .estore choice logs
     * itself; lying about slot existence made SaveMe skip SaveBundle_Create
     * and corrupted slot enumeration. */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z20luaResourceSetExistsP9lua_State",
                              "luaResourceSetExists",
                              (uintptr_t)&hook_lua_resource_set_exists,
                              &g_hook_lua_resource_set_exists);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z21luaResourceSetEnabledP9lua_State",
                              "luaResourceSetEnabled",
                              (uintptr_t)&hook_lua_resource_set_enabled,
                              &g_hook_lua_resource_set_enabled);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z25luaPlatformIsUserSignedInP9lua_State",
                              "luaPlatformIsUserSignedIn",
                              (uintptr_t)&hook_lua_platform_is_user_signed_in,
                              &g_hook_lua_platform_is_user_signed_in);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaIsUserSignedInP9lua_State",
                              "luaIsUserSignedIn",
                              (uintptr_t)&hook_lua_is_user_signed_in,
                              &g_hook_lua_is_user_signed_in);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z24luaPlatformRequestSignInP9lua_State",
                              "luaPlatformRequestSignIn",
                              (uintptr_t)&hook_lua_platform_request_sign_in,
                              &g_hook_lua_platform_request_sign_in);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z15luaIsEngineVitaP9lua_State",
                              "luaIsEngineVita",
                              (uintptr_t)&hook_lua_is_engine_vita,
                              &g_hook_lua_is_engine_vita);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaIsJoystickXboxP9lua_State",
                              "luaIsJoystickXbox",
                              (uintptr_t)&hook_lua_is_joystick_xbox,
                              &g_hook_lua_is_joystick_xbox);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z18luaIsEngineXbox360P9lua_State",
                              "luaIsEngineXbox360",
                              (uintptr_t)&hook_lua_is_engine_xbox360,
                              &g_hook_lua_is_engine_xbox360);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z16luaIsEngineXBOneP9lua_State",
                              "luaIsEngineXBOne",
                              (uintptr_t)&hook_lua_is_engine_xbone,
                              &g_hook_lua_is_engine_xbone);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z14luaIsEnginePS3P9lua_State",
                              "luaIsEnginePS3",
                              (uintptr_t)&hook_lua_is_engine_ps3,
                              &g_hook_lua_is_engine_ps3);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z14luaIsEnginePS4P9lua_State",
                              "luaIsEnginePS4",
                              (uintptr_t)&hook_lua_is_engine_ps4,
                              &g_hook_lua_is_engine_ps4);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z19luaInputHasJoystickP9lua_State",
                              "luaInputHasJoystick",
                              (uintptr_t)&hook_lua_input_has_joystick,
                              &g_hook_lua_input_has_joystick);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z24luaInputSupportsJoystickP9lua_State",
                              "luaInputSupportsJoystick",
                              (uintptr_t)&hook_lua_input_supports_joystick,
                              &g_hook_lua_input_supports_joystick);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z25luaInputIsJoystickEnabledP9lua_State",
                              "luaInputIsJoystickEnabled",
                              (uintptr_t)&hook_lua_input_is_joystick_enabled,
                              &g_hook_lua_input_is_joystick_enabled);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z16luaInputHasTouchP9lua_State",
                              "luaInputHasTouch",
                              (uintptr_t)&hook_lua_input_has_touch,
                              &g_hook_lua_input_has_touch);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z21luaInputSupportsTouchP9lua_State",
                              "luaInputSupportsTouch",
                              (uintptr_t)&hook_lua_input_supports_touch,
                              &g_hook_lua_input_supports_touch);

    /* CONNECT-FLOW trace hooks (find the "Connecting" blocker). */
    #define REG_CONNECT_TRACE(fn, mangled)                                      \
        (void)hook_symbol_checked(&so_mod_gameengine, mangled, #fn,             \
                                  (uintptr_t)&hook_##fn, &g_hook_##fn)
    REG_CONNECT_TRACE(luaUpdateProfileAndWait,        "_Z23luaUpdateProfileAndWaitP9lua_State");
    REG_CONNECT_TRACE(luaUpdateStatsAndWait,          "_Z21luaUpdateStatsAndWaitP9lua_State");
    REG_CONNECT_TRACE(luaUpdateAchievementsAndWait,   "_Z28luaUpdateAchievementsAndWaitP9lua_State");
    REG_CONNECT_TRACE(luaUpdateFriendsAndWait,        "_Z23luaUpdateFriendsAndWaitP9lua_State");
    REG_CONNECT_TRACE(luaGetConnectionStatus,         "_Z22luaGetConnectionStatusP9lua_State");
    REG_CONNECT_TRACE(luaBeginUpdateEpisodes,         "_Z22luaBeginUpdateEpisodesP9lua_State");
    REG_CONNECT_TRACE(luaIsEpisodesUpdateAvailable,   "_Z28luaIsEpisodesUpdateAvailableP9lua_State");
    REG_CONNECT_TRACE(luaLocalContentEnumerationReady,"_Z31luaLocalContentEnumerationReadyP9lua_State");
    REG_CONNECT_TRACE(luaUploadPendingObjectsToServer,"_Z31luaUploadPendingObjectsToServerP9lua_State");
    REG_CONNECT_TRACE(luaUploadCachedObjectToServer,  "_Z29luaUploadCachedObjectToServerP9lua_State");
    REG_CONNECT_TRACE(luaNewGame,                      "_Z10luaNewGameP9lua_State");
    REG_CONNECT_TRACE(luaMountEpisode,                 "_Z15luaMountEpisodeP9lua_State");
    REG_CONNECT_TRACE(luaInstallEpisode,               "_Z17luaInstallEpisodeP9lua_State");
    REG_CONNECT_TRACE(luaMountAllEpisodes,             "_Z19luaMountAllEpisodesP9lua_State");
    #undef REG_CONNECT_TRACE
}

// DIAGNOSTIC: trace the bootTitle login/connect/cloud-sync native primitives so
// the next HW log pinpoints exactly where the "Checking For DLC" stall happens.
static void patch_login_diag_hooks(void) {
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z18luaShowPasswordBoxP9lua_State",
                              "luaShowPasswordBox",
                              (uintptr_t)&hook_lua_show_password_box, &g_hook_lua_show_password_box);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z24luaIsPasswordBoxFinishedP9lua_State",
                              "luaIsPasswordBoxFinished",
                              (uintptr_t)&hook_lua_is_password_box_finished, &g_hook_lua_is_password_box_finished);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z24luaGetPasswordBoxResultsP9lua_State",
                              "luaGetPasswordBoxResults",
                              (uintptr_t)&hook_lua_get_password_box_results, &g_hook_lua_get_password_box_results);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z26luaNetworkAPIGetCredentialP9lua_State",
                              "luaNetworkAPIGetCredential",
                              (uintptr_t)&hook_lua_network_get_credential, &g_hook_lua_network_get_credential);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z20luaSessionLogProcessP9lua_State",
                              "luaSessionLogProcess",
                              (uintptr_t)&hook_lua_session_log_process, &g_hook_lua_session_log_process);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z30luaNetworkAPICloudSyncUserDataP9lua_State",
                              "luaNetworkAPICloudSyncUserData",
                              (uintptr_t)&hook_lua_cloud_sync_userdata, &g_hook_lua_cloud_sync_userdata);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z29luaUploadCachedObjectToServerP9lua_State",
                              "luaUploadCachedObjectToServer",
                              (uintptr_t)&hook_lua_upload_cached, &g_hook_lua_upload_cached);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z31luaUploadPendingObjectsToServerP9lua_State",
                              "luaUploadPendingObjectsToServer",
                              (uintptr_t)&hook_lua_upload_pending, &g_hook_lua_upload_pending);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z12luaChorePlayP9lua_State",
                              "luaChorePlay",
                              (uintptr_t)&hook_lua_choreplay, &g_hook_lua_choreplay);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z12luaSceneOpenP9lua_State",
                              "luaSceneOpen",
                              (uintptr_t)&hook_lua_sceneopen, &g_hook_lua_sceneopen);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z7luaLoadP9lua_State",
                              "luaLoad",
                              (uintptr_t)&hook_lua_load, &g_hook_lua_load);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z19luaResourceIsLoadedP9lua_State",
                              "luaResourceIsLoaded",
                              (uintptr_t)&hook_lua_resource_is_loaded, &g_hook_lua_resource_is_loaded);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z15luaScenePreloadP9lua_State",
                              "luaScenePreload",
                              (uintptr_t)&hook_lua_scene_preload, &g_hook_lua_scene_preload);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z30luaResourceAdvancePreloadBatchP9lua_State",
                              "luaResourceAdvancePreloadBatch",
                              (uintptr_t)&hook_lua_advance_preload, &g_hook_lua_advance_preload);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z19luaWaitForCallbacksP9lua_State",
                              "luaWaitForCallbacks",
                              (uintptr_t)&hook_lua_wait_for_callbacks, &g_hook_lua_wait_for_callbacks);
}

/* RENDER-QUALITY + OUTLINE draw-reduction levers (2026-07-20, opt-in) ---------
 * The Telltale engine bakes ~900 draws/frame (one per material x mesh-batch x bone
 * palette, doubled by the shadow pass) and CANNOT be batched at the loader level —
 * confirmed by the D3DMesh format + vitaGL docs. The only way to cut the sustained
 * draw cost (the "no stable 30" that shadows-off didn't fully solve) is to render
 * LESS, via the engine's OWN scalability levers:
 *   (1) render QUALITY tier — RenderConfiguration::SetQuality(RenderQualityType).
 *       A lower tier selects the vlow shader permutations + cheaper/omitted effects
 *       (fewer runtime shader compiles AND less per-frame CPU/GPU). The hook ALWAYS
 *       logs the natural values the game sets (so a diagnostic run reveals the enum
 *       in loader.log) and only OVERRIDES when settings/render_quality.txt = <int>.
 *   (2) toon OUTLINE pass — RenderObject_Mesh::SetRenderToonOutline(bool). Forcing
 *       it off skips the per-mesh outline submit. Opt-in: settings/no_outlines.txt.
 * Both alter visuals, so both are OFF by default — zero effect on the shader-key-fix
 * validation; enable to A/B toward reliable 30fps. */
static int render_quality_override(void) {
    static int v = -2;   /* -2 unread, -1 no override, >=0 forced level */
    if (v == -2) {
        v = -1;
        /* MCSM boots at q=15 (logged) — outside the 0-4 desktop enum, so its mobile
         * build uses a wider scale. Accept 0..15; the engine already runs 15 without
         * crashing, so any value in range is safe to probe (revert = delete the file). */
        FILE *f = mcsm_open_setting("render_quality.txt", "r");
        if (f) { int q = -1; if (fscanf(f, "%d", &q) == 1 && q >= 0 && q <= 15) v = q; fclose(f); }
    }
    return v;
}
static so_hook g_hook_set_render_quality;
static void hook_set_render_quality(void *self, int quality) {
    static unsigned n = 0;
    const int ov = render_quality_override();
    if (n++ < 8U) l_info("RQUAL: RenderConfiguration::SetQuality(q=%d) override=%d", quality, ov);
    if (ov >= 0) quality = ov;
    SO_CONTINUE_VOID(g_hook_set_render_quality, self, quality);
}

static int outlines_disabled(void) { return !mcsm_cfg()->outlines; }
static so_hook g_hook_set_toon_outline;
static void hook_set_toon_outline(void *self, int on) {
    if (outlines_disabled()) on = 0;
    SO_CONTINUE_VOID(g_hook_set_toon_outline, self, on);
}

/* GEOMETRY/LOD lever (2026-07-20): Scene::SetBrushNear/FarDetail(float) is the
 * only exposed control over the engine's ~435k-vert brush geometry — the wall
 * that caps heavy-scene fps (verts are resolution-independent, so lowering the
 * framebuffer can't touch it; device log: draws=~900 verts=~435k render=41-58ms).
 * Scaling the detail DOWN biases toward coarser baked LODs = fewer verts = faster
 * render. FLOAT arg, so we CANNOT use SO_CONTINUE (it casts to an unprototyped fn
 * pointer -> float promotes to double -> value corrupted; that is exactly why the
 * old RenderOverlay float hook was abandoned). Use a TYPED trampoline instead.
 * OPT-IN + gated on settings/detail_scale.txt (0.1..1.0): the hooks are not even
 * installed without it, so the default build is zero-risk. Logs the engine's
 * natural detail values so the scale can be tuned from a device run. */
static float detail_scale(void) {
    static float v = -2.0f;               /* < -1 = unread */
    if (v < -1.0f) {
        v = -1.0f;                        /* -1 = no file / no override */
        FILE *f = mcsm_open_setting("detail_scale.txt", "r");
        if (f) { float x = 0.0f; if (fscanf(f, "%f", &x) == 1 && x >= 0.1f && x <= 1.0f) v = x; fclose(f); }
    }
    return v;
}
static so_hook g_hook_scene_far_detail, g_hook_scene_near_detail;
#define MCSM_DETAIL_TRAMPOLINE(H, SELF, V) do { \
    kuKernelCpuUnrestrictedMemcpy((void *)(H).addr, (H).orig_instr, sizeof((H).orig_instr)); \
    kuKernelFlushCaches((void *)(H).addr, sizeof((H).orig_instr)); \
    void (*fn_)(void *, float) = (H).thumb_addr ? (void (*)(void *, float))(H).thumb_addr \
                                                : (void (*)(void *, float))(H).addr; \
    fn_((SELF), (V)); \
    kuKernelCpuUnrestrictedMemcpy((void *)(H).addr, (H).patch_instr, sizeof((H).patch_instr)); \
    kuKernelFlushCaches((void *)(H).addr, sizeof((H).patch_instr)); \
} while (0)
static void hook_scene_far_detail(void *self, float v) {
    static unsigned n = 0; const float s = detail_scale();
    if (n++ < 8U) l_info("DETAIL: Scene::SetBrushFarDetail=%d/1000 scale=%d/1000", (int)(v * 1000.0f), (int)(s * 1000.0f));
    if (s > 0.0f) v *= s;
    MCSM_DETAIL_TRAMPOLINE(g_hook_scene_far_detail, self, v);
}
static void hook_scene_near_detail(void *self, float v) {
    static unsigned n = 0; const float s = detail_scale();
    if (n++ < 8U) l_info("DETAIL: Scene::SetBrushNearDetail=%d/1000 scale=%d/1000", (int)(v * 1000.0f), (int)(s * 1000.0f));
    if (s > 0.0f) v *= s;
    MCSM_DETAIL_TRAMPOLINE(g_hook_scene_near_detail, self, v);
}

/* FAR-CLIP CULLING (2026-07-20): the ONLY runtime lever that cuts the 439k-vert
 * wall — the engine has no runtime mesh LOD. Pulling the camera's far plane IN
 * frustum-culls distant in-view geometry, so verts AND draws fall together.
 * Camera::SetFarClip(float) -> TYPED trampoline (SO_CONTINUE corrupts the float).
 * Opt-in: settings/far_clip.txt = far-plane cap in world units (try ~4000). HIGH
 * visual pop at the boundary, scene-dependent (only helps scenes with distant
 * geometry), so default-off + per-scene tuned. Logs the natural far value. */
static float far_clip_cap(void) {
    int d = mcsm_cfg()->draw_distance;   /* 0 = engine default (no clamp) */
    return d > 0 ? (float)d : -1.0f;
}
static so_hook g_hook_camera_far_clip;
static void hook_camera_far_clip(void *self, float v) {
    static unsigned n = 0; const float cap = far_clip_cap();
    if (n++ < 8U) l_info("FARCLIP: Camera::SetFarClip=%d cap=%d", (int)v, (int)cap);
    if (cap > 0.0f && v > cap) v = cap;   /* clamp the far plane IN; never push it out */
    MCSM_DETAIL_TRAMPOLINE(g_hook_camera_far_clip, self, v);
}

static void patch_render_perf_hooks(void) {
    /* Install the quality hook ALWAYS: it logs the engine's natural quality values
     * (so we can learn the enum from a diagnostic loader.log) and forwards unchanged
     * unless render_quality.txt is set, so it is behaviour-neutral by default. */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN19RenderConfiguration10SetQualityE17RenderQualityType",
                              "RenderConfiguration::SetQuality",
                              (uintptr_t)&hook_set_render_quality, &g_hook_set_render_quality);
    if (render_quality_override() >= 0) {
        l_info("PERF: render quality OVERRIDE -> %d (render_quality.txt)", render_quality_override());
    }
    if (outlines_disabled()) {
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_ZN17RenderObject_Mesh20SetRenderToonOutlineEb",
                                  "RenderObject_Mesh::SetRenderToonOutline",
                                  (uintptr_t)&hook_set_toon_outline, &g_hook_set_toon_outline);
        l_info("PERF: toon outlines DISABLED (no_outlines.txt) — outline submit skipped.");
    }
    if (detail_scale() > 0.0f) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN5Scene17SetBrushFarDetailEf",
                                  "Scene::SetBrushFarDetail",
                                  (uintptr_t)&hook_scene_far_detail, &g_hook_scene_far_detail);
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN5Scene18SetBrushNearDetailEf",
                                  "Scene::SetBrushNearDetail",
                                  (uintptr_t)&hook_scene_near_detail, &g_hook_scene_near_detail);
        l_info("PERF: brush detail scaled to %d/1000 (detail_scale.txt) — NOTE: verified post-effect, NOT a vert cut.",
               (int)(detail_scale() * 1000.0f));
    }
    if (far_clip_cap() > 0.0f) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN6Camera10SetFarClipEf",
                                  "Camera::SetFarClip",
                                  (uintptr_t)&hook_camera_far_clip, &g_hook_camera_far_clip);
        l_info("PERF: far-clip capped at %d (far_clip.txt) — culls distant geometry (verts+draws).",
               (int)far_clip_cap());
    }
}

void so_patch(void) {
    patch_fmod_audio_hooks();
    patch_engine_diag_hooks();
    patch_sdl_android_runtime_hooks();
    patch_input_diag_hooks();
    patch_dlc_fast_path_hooks();
    patch_login_diag_hooks();
    patch_boot_diag_hooks();
    patch_render_perf_hooks();
    patch_vertexbuffer_platform_lock();
    patch_vertexbuffer_platform_unlock();
    patch_indexbuffer_platform_lock();
    patch_indexbuffer_platform_unlock();
}
