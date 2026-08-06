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
#include "utils/init.h"
#include "utils/launch_state.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/config.h"


extern so_module so_mod_gameengine;
extern so_module so_mod_fmod;
extern so_module so_mod_fmodstudio;
extern so_module so_mod_sdl2;
extern void mcsm_register_virtual_controller(void);

#define ENABLE_UNSAFE_ARCHIVE_DIAG_HOOKS 0
#define ENABLE_HOT_RENDER_VIEW_DIAG_HOOKS 0

/* Keep format-checked diagnostic throttles in telemetry builds without paying a
 * load/add/store in production handlers whose result is otherwise constant. */
#ifdef DEBUG_SOLOADER
#define MCSM_DIAG_COUNTER(name) static uint32_t name = 0; ++name
#else
#define MCSM_DIAG_COUNTER(name) enum { name = 0 }
#endif

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
static so_hook g_hook_renderframe_execute;
static so_hook g_hook_renderframe_allocate_view;
static so_hook g_hook_renderframe_push_view;
static so_hook g_hook_gamerender_render_frame;
static so_hook g_hook_gamerender_render_scene;
static so_hook g_hook_playback_controller_update;
static uintptr_t g_renderframe_execute_tramp;
static uintptr_t g_gamerender_render_frame_tramp;
static uintptr_t g_gamerender_render_scene_tramp;
static uintptr_t g_playback_controller_update_tramp;
static uintptr_t g_metrics_new_frame_tramp;
static so_hook g_hook_android_pump_events;
static so_hook g_hook_android_jni_poll_input_devices;
static so_hook g_hook_sdl_wait_event_real;
static so_hook g_hook_sdl_wait_event_timeout_real;
#ifdef DEBUG_SOLOADER
static so_hook g_hook_app_on_fingering;
static so_hook g_hook_app_on_mouse_event;
static so_hook g_hook_gamewindow_mouse_move;
static so_hook g_hook_gamewindow_mouse_down;
static so_hook g_hook_gamewindow_mouse_up;
static so_hook g_hook_touch_set_legacy_pointer;
#endif
static so_hook g_hook_fmod_studio_initialize;

/* Defined with the render-lever trampoline implementation below. Hot hooks use
 * the same validated cave builder and retain SO_CONTINUE as a fail-safe fallback. */
static uintptr_t mcsm_build_tramp(so_hook *h);
static uintptr_t mcsm_build_metrics_new_frame_tramp(so_hook *h);


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
/* PER-CHARACTER UPLOAD PROBE (2026-07-30) -----------------------------------
 * These count what the T3VertexBuffer / T3IndexBuffer hooks actually do each
 * frame. They exist because the existing logs CANNOT answer it: the lock log is
 * capped at 16 lines/run and the AllocateGLBuffer log at 32, so both go quiet
 * long before gameplay, and every conclusion about per-frame upload cost so far
 * has been inference rather than measurement.
 *
 * Why this is the number that matters: T3VertexBuffer::PlatformLock/Unlock are
 * reached from RenderObject_Mesh::DoSoftwareSkinning <- _RenderMeshInstance, so
 * they run once per SKINNED MESH per FRAME -- they scale with character count,
 * which is exactly the reported failure shape. Plain counters, no syscalls and
 * no formatting on the hot path; mcsm_skin_report turns them into per-frame
 * averages.
 *
 * CUMULATIVE, not per-frame, deliberately: the writer is the render thread and
 * the reader is the watchdog thread, so a per-frame reset would race the read.
 * Deltas over the report window give the same answer with no shared reset. Bytes
 * accumulate in KILOBYTES so a u32 cannot wrap in a session (a skinned vertex
 * buffer is always far more than 1KB, so the truncation is noise). */
/* ☠ NOT volatile. These are updated 5-7 times per buffer lock/upload, ~790 times a
 * frame, and `volatile` forces every one to a memory round trip and blocks the
 * compiler from keeping or coalescing them in registers across the hottest non-GL
 * loader code. One thread writes them; the watchdog reads them once per 10s and only
 * needs a monotonically-increasing approximation -- exactly the reasoning
 * launch_state.c already applies to g_draw_serial. */
#ifdef DEBUG_SOLOADER
static uint32_t g_vb_locks = 0;     /* T3VertexBuffer::PlatformLock calls   */
static uint32_t g_vb_respec = 0;    /* full glBufferData respecifies        */
static uint32_t g_vb_kb = 0;        /* KB pushed through those respecifies  */
static uint32_t g_vb_mallocs = 0;   /* CPU staging buffers malloc'd + freed */
/* Size distribution: the per-frame COST is bytes, not call count, and the sizes
 * span three orders of magnitude (the boot log alone shows 32 B up to 262140 B),
 * so an average alone would be misleading. Largest respecify seen tells us
 * whether the traffic is a few big shared pools or many per-character buffers. */
static uint32_t g_vb_max_bytes = 0;
/* Which thread pays. This decides whether the cost lands inside the measured
 * sim budget (hook_gameengine_loop's sim_us) or on the render/present side, and
 * therefore whether removing it moves the number we actually care about.
 * Sampled 1-in-256 so the syscall is not on the per-mesh path. */
static uint32_t g_vb_tid = 0;
/* Effectiveness counters for the two overhead fixes: how many uploads took the
 * no-allocation glBufferSubData path, and how many locks reused a staging buffer
 * instead of allocating. Both should dominate their totals once warm. */
static uint32_t g_vb_subdata = 0;
static uint32_t g_vb_reuse = 0;
static uint32_t g_vb_evict = 0;   /* table evictions -- if this tracks locks/f, the keys are transient */
#define MCSM_VB_DIAG(code) do { code; } while (0)
#else
#define MCSM_VB_DIAG(code) do { } while (0)
#endif
/* SIM BREAKDOWN (2026-07-30). DIP-SIM says the engine sim is a sustained 25-27ms
 * against a 16.7ms budget, and frames land at 41ms -- so ~26ms sim + ~15ms other.
 * Knowing WHICH of the two once-per-frame animation entry points owns that 26ms is
 * the difference between cutting it and guessing at it. Both functions are already
 * hooked and both are called exactly once per frame from one thread, so timing them
 * costs 4 syscalls/frame and adds no new hook risk. Accumulated in us; the reporter
 * divides by frames. */
/* ★ THE SIM IS NOT PURE SIMULATION. GameEngine::Loop calls GameEngine::Render at
 * +0x1ec (0xcb304c), so the DIP-SIM "work=26-40ms" figure this project has been
 * optimising against for weeks INCLUDES render submission -- the ~890 draw calls.
 * Both functions were already hooked; timing the inner one splits the number.
 * Scene::UpdateScenes and ScriptManager::Update are the two largest remaining
 * phases by inspection of Loop's call order, and both are once-per-frame and
 * single-threaded, so SO_CONTINUE on them is safe (~15us each). */
static volatile uint32_t g_render_us = 0, g_render_n = 0;
static volatile uint32_t g_scene_us = 0, g_scene_n = 0;
static volatile uint32_t g_script_us = 0, g_script_n = 0;

/* ★ PHASE TIMERS ARE NOW GATED (2026-07-30), like the per-draw diagnostics already
 * were. Each instrumented phase brackets its target with TWO
 * sceKernelGetSystemTimeWide() calls; across render/chore/playback/scenes/script
 * that is at least ten syscalls a frame, and more wherever a phase runs more than
 * once. They exist to answer "where does the frame go", which the r82 device run
 * answered: render ~75%, animation ~11%, scenes+script ~0. Unlike the glDrawElements
 * diagnostics these were NOT behind MCSM_FAST_FINAL_RUNTIME, so they shipped.
 * Build with -DMCSM_FAST_FINAL_RUNTIME=0 to measure again; SIMSPLIT then reports as
 * before and reads 0.000ms everywhere in a normal build, which is the intended tell
 * that instrumentation is off rather than that the phases are free. */
#ifndef MCSM_FAST_FINAL_RUNTIME
#define MCSM_FAST_FINAL_RUNTIME 1
#endif

/* ★ 2026-08-06 — THE DIAGNOSTIC BUILD COULD NOT DIAGNOSE THE SIM.
 *
 * MCSM_FAST_FINAL_RUNTIME is never set by CMakeLists or build_vpk.ps1, so it is 1
 * in BOTH configurations -- including the logging VPK that exists purely to be
 * sent back for analysis. In a 25-minute device log that meant: every phase
 * accumulator read 0.000ms, `SIMSPLIT` printed only "phase timers COMPILED OUT",
 * the chore-tick counter never incremented, and `ANIM: chore ticks` was logged
 * ZERO times. So the one build we ask players to run could not answer "where does
 * the frame go" or "are chores even advancing" -- which is exactly what the
 * outstanding animation and framerate reports need.
 *
 * Split the two concerns. MCSM_FAST_FINAL_RUNTIME keeps governing the PER-DRAW GL
 * diagnostics, which really are too expensive to ship and would distort any
 * measurement they appear in. MCSM_SIM_PROBES governs the per-FRAME sim phase
 * timers and the chore tick, and follows the logging switch: on in the logging
 * build, off in the release build. That is ~10 syscalls a frame in a build that is
 * already slower on purpose and already labelled as such. */
#ifndef MCSM_SIM_PROBES
#  ifdef DEBUG_SOLOADER
#    define MCSM_SIM_PROBES 1
#  else
#    define MCSM_SIM_PROBES 0
#  endif
#endif

#if !MCSM_SIM_PROBES
#define MCSM_PHASE_T0()             (0ULL)
#define MCSM_PHASE_ADD(acc, n, t0)  ((void)(t0))
#else
#define MCSM_PHASE_T0()             sceKernelGetSystemTimeWide()
#define MCSM_PHASE_ADD(acc, n, t0)  \
    do { (acc) += (uint32_t)(sceKernelGetSystemTimeWide() - (t0)); (n)++; } while (0)
#endif
/* [X] ARM PMU COUNTERS: ATTEMPTED AND REMOVED (2026-07-30).
 * scePerfArmPmon* crashed before the first frame. Cause, from the VitaSDK headers:
 *     psp2/power.h :  usergroup{ScePower}    <- user mode, works
 *     psp2/perf.h  :  kernelgroup{ScePerf}   <- KERNEL ONLY
 * They are kernel functions. ScePerf_stub links them, which is why the build was
 * clean, but a user-mode call is not valid. Reading the ARM PMU needs a taihen
 * kernel plugin (.skprx) -- a separate project, not a loader change.
 * Razor is out too: librazorcapture_es4.suprx is not on retail firmware (searched
 * vs0:/sys/external and every other partition). Wall-clock phase timing is the
 * profiling we actually have. */
static volatile uint32_t g_chore_us = 0;      /* ChoreInst::UpdateChoreInstances   */
static volatile uint32_t g_pbc_us = 0;       /* PlaybackController::Update...      */
static volatile uint32_t g_chore_n = 0, g_pbc_n = 0;
static const uint32_t *g_render_caps_ptr = NULL;
void mcsm_skin_report(void);                 /* defined next to mcsm_anim_report     */
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
typedef int (*fmod_system_get_version_fn)(void *system, unsigned int *version);
static fmod_system_get_version_fn g_fmod_system_get_version = NULL;

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
    if (!g_fmod_system_get_version) {
        g_fmod_system_get_version =
            (fmod_system_get_version_fn)so_symbol(&so_mod_fmod, "FMOD_System_GetVersion");
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
    /* OPENSL REMOVED (2026-07-29). The loader no longer impersonates Android
     * OpenSL ES: FMOD is driven by our own registered output plugin
     * (fmod_output_vita.c) writing straight to sceAudioOut, which the device
     * proved works -- 4142 mixer pulls, readfail=0, outfail=0.
     *
     * Forcing FMOD_OUTPUTTYPE_OPENSL here is therefore not just unnecessary but
     * harmful: the OpenSL backend would try to dlopen libOpenSLES.so, which no
     * longer exists, and could fail init before SetOutputByPlugin ever runs.
     * AUTODETECT leaves FMOD's own choice alone for the brief window before the
     * plugin takes over. */
    const int requested_output = FMOD_OUTPUTTYPE_AUTODETECT;

    if (g_fmod_studio_get_low_level_system && g_fmod_system_set_output) {
        get_low_result = g_fmod_studio_get_low_level_system(studio_system, &low_level);
        if (get_low_result == 0 && low_level) {
            if (g_fmod_system_get_output) {
                (void)g_fmod_system_get_output(low_level, &output_before);
            }
            if (requested_output != FMOD_OUTPUTTYPE_AUTODETECT) {
            } else {
            }
            /* MIX RATE (2026-07-30). FMOD was initialising its software mixer at
             * 24000 Hz -- the Android build's mobile setting -- while the game's
             * own audio is 44100. Decoding the FSB5 headers in
             * MCSM_android_Minecraft101_voice.ttarch2: 200 of 200 banks are
             * 44100 Hz MPEG mono. So the port was low-passing every line of
             * dialogue and every music cue at 12 kHz (Nyquist) for no reason --
             * the quality is already sitting in the shipped assets.
             *
             * Matching the mixer to the source is not merely free, it is likely
             * cheaper: MP3 decode happens at 44100 regardless, and FMOD is
             * currently resampling 44100 -> 24000 on every sample it mixes.
             * Setting the format equal to the source removes that conversion.
             * sceAudioOut takes 44100 natively (see clamp_audio_rate in java.c).
             *
             * Must be called before FMOD's init, which is exactly where we are --
             * the same window that makes registerOutput legal. Override with
             * settings/audio_rate.txt (e.g. 24000) if it ever misbehaves. */
            if (low_level) {
                /* graphics.txt `audio_rate`; 0 = keep the 44100 the assets use. */
                const int cfg_rate = mcsm_cfg()->audio_rate;
                int want_rate = cfg_rate > 0 ? cfg_rate : 44100;
                typedef int (*setfmt_fn)(void *, int, int, int);
                setfmt_fn setfmt = (setfmt_fn)so_symbol(&so_mod_fmod, "FMOD_System_SetSoftwareFormat");
                if (setfmt) {
                    int rc = setfmt(low_level, want_rate, 0 /* SPEAKERMODE_DEFAULT */, 0);
                    l_info("FMODRATE: software mix rate -> %d Hz rc=%d (source assets are 44100; "
                           "was 24000, which discarded everything above 12kHz)", want_rate, rc);
                } else {
                    l_warn("FMODRATE: FMOD_System_SetSoftwareFormat not found — staying at the engine default");
                }
            }

            /* REAL FMOD OUTPUT PLUGIN. Registered here because this is the one
             * place the low-level System exists before init, which is the only
             * point registerOutput is legal. Unconditional: there is no OpenSL
             * path to fall back to any more, so a failure here means no audio and
             * says so in the log. */
            {
                extern int mcsm_fmod_output_install(void *, int (*)(void *, const void *, unsigned int *),
                                                    int (*)(void *, unsigned int));
                if (low_level) {   /* unconditional: the plugin IS the audio path now */
                    typedef int (*reg_fn)(void *, const void *, unsigned int *);
                    typedef int (*setp_fn)(void *, unsigned int);
                    reg_fn  reg  = (reg_fn)so_symbol(&so_mod_fmod,  "FMOD_System_RegisterOutput");
                    setp_fn setp = (setp_fn)so_symbol(&so_mod_fmod, "FMOD_System_SetOutputByPlugin");
                    if (mcsm_fmod_output_install(low_level, reg, setp)) {
                        l_info("FMODOUT: opensl_audio.c is now BYPASSED for this session");
                    }
                }
            }
            if (g_fmod_system_get_output) {
                (void)g_fmod_system_get_output(low_level, &output_after);
            }
            /* FMOD VERSION PROBE (2026-07-29). We currently force FMOD to its OPENSL
             * backend and then impersonate Android's OpenSL ES in ~900 lines of
             * opensl_audio.c -- including a feed thread that spins without sleeping
             * whenever the mixer accepts data. The correct design is an FMOD output
             * plugin driving sceAudioOut directly (FMOD_System_RegisterOutput +
             * SetOutputByPlugin, both exported here), which removes the impersonation
             * and the thread outright.
             *
             * The blocker is ABI: FMOD_OUTPUT_DESCRIPTION's layout and callback set
             * changed across the 1.x line, and this binary carries no version string.
             * Fingerprinting narrows it to FMOD Studio 1.x (FMOD_Thread_SetAttributes
             * absent = pre-2.01; Studio::System::create takes a headerversion) but not
             * to a specific release, and guessing the struct crashes at boot. Ask the
             * library instead -- one call, logged unconditionally so a single run
             * settles it. FMOD encodes the version as 0x000AABBCC = AA.BB.CC. */
            if (g_fmod_system_get_version && low_level) {
                unsigned int fmod_ver = 0;
                int vr = g_fmod_system_get_version(low_level, &fmod_ver);
                l_info("FMODVER: GetVersion rc=%d raw=0x%08X -> FMOD %u.%02u.%02u",
                       vr, fmod_ver, (fmod_ver >> 16) & 0xFFFFu,
                       (fmod_ver >> 8) & 0xFFu, fmod_ver & 0xFFu);
            } else {
                l_warn("FMODVER: cannot probe (getVersion=%p low_level=%p)",
                       (void *)g_fmod_system_get_version, low_level);
            }
        }
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
    return result;
}

static void patch_fmod_audio_hooks(void) {
    resolve_fmod_audio_symbols();
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

/* FRAME PACING (2026-06-23): the animation judder isn't low fps (avg ~50-60 in
 * menus) -- it's VARIANCE. Plain vsync makes the per-frame delta beat 17ms<->34ms
 * (60<->30) because the GPU keeps missing vblank by a hair, so the engine advances
 * chores/animation by an uneven amount each frame = stutter. Pacing the game loop
 * to a fixed period makes the delta the engine sees constant -> smooth motion.
 * Default 30fps (the cadence the original console builds used). Tunable at runtime
 * with no rebuild via settings/graphics.txt `fps_cap` (an integer; <=0 = uncapped).
 * This must honor the user's configured cap directly; forcing 60 down to 30
 * made character/menu animation visibly regress in heavy scenes. */
/* The EXACT frame period in us for the configured cap (0 = uncapped). This is the
 * real per-frame budget and the number the clock governor must scale against; the
 * sim pace below is this minus a small undershoot. */
static uint32_t mcsm_frame_period_us(void) {
    const int fps = mcsm_cfg()->fps_cap;
    if (fps <= 0 || fps > 120) return 0;
    return (uint32_t)(1000000 / fps);
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
 * the step-up. Budget follows the configured pace (graphics.txt (fps_cap)), default 30.
 * RENDER-BOUND GUARD: sim_us can't see the render thread's ~900-draw submission
 * cost, so the step-down is ALSO gated on the TRUE present cadence
 * (g_mcsm_present_dt_us from gl_swap) — it never downclocks a scene whose frame
 * is already dropping, and escalates when it is. Biased to the ceiling (safe for
 * the stable-30 goal; a wrong present read only costs battery, never fps).
 * Config: graphics.txt `clock`. "adaptive"/"battery" enables this governor with a
 * 266 floor and a clock_mhz ceiling; a bare <MHz> pins the ARM and disables it.
 * (An earlier comment here advertised a settings/clock.txt with off/min/max/gpu
 * tokens. NO SUCH FILE IS EVER PARSED — see the McsmClockCfg note in utils.h.) */
/* Is the governor actually driving the ARM clock right now? 0 until its first tick
 * has run and decided, which is the safe answer: before then main.c's re-assert owns
 * the ARM, so the clock is held up rather than left to drift. main.c MUST use this
 * instead of inferring ownership from graphics.txt, because the governor can also be
 * disabled by a non-adaptive graphics.txt clock, or by there being only one usable
 * clock step. */
static int g_clock_gov_active = 0;
int mcsm_clock_governor_active(void) { return g_clock_gov_active; }

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
        /* ★ graphics.txt decides WHO OWNS the ARM (it is the only clock config).
         * These were two independent switches in two different files, and one legal
         * combination left the ARM owned by NOBODY: graphics.txt `clock = adaptive`
         * made main.c's re-assert stand down ("the governor has it"), while
         * clock.txt `off` made this function return before ever calling
         * scePowerSetArmClockFrequency. The power manager then walked the clock down
         * to 333MHz unopposed and nothing logged it -- a silent 25% CPU underclock,
         * and the battery profile sets clock_adaptive unconditionally so it was
         * reachable by config alone. Now a single predicate decides, and
         * mcsm_clock_governor_active() publishes the ANSWER rather than each side
         * re-deriving it from its own file. */
        mcsm_read_clock_cfg(&cfg);        /* derives arm_min/arm_max from graphics.txt */
        /* The `cfg.governor_off` term that used to be here was DEAD: nothing ever
         * sets that field to 1 (mcsm_read_clock_cfg assigns 0 unconditionally), so
         * the branch and its "clock.txt 'off'" message were unreachable -- and a
         * reader grepping a log for that string would have wrongly concluded the
         * governor had run. graphics.txt's `clock` setting is the real switch. */
        if (!mcsm_cfg()->clock_adaptive) {
            l_info("clock-gov: DISABLED (graphics.txt clock is not adaptive) — "
                   "ARM pinned; the main.c re-assert owns it");
            return; /* enabled and g_clock_gov_active both stay 0 */
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
        /* The budget is the EXACT frame period, not the sim pace. The pace is the
         * period minus a 2500us undershoot, so using it sized every threshold ~6-8%
         * low; and before 2026-07-30 the pace was whole-vblank quantised, which for
         * fps_cap=24 reported 30834 against a real 41666 budget -- a 26% error that
         * made the governor hold a high clock on frames that had ample headroom. */
        budget_us = mcsm_frame_period_us();
        if (!budget_us) budget_us = 33333; /* uncapped -> target 30fps power budget */
        up_us   = (uint32_t)((uint64_t)budget_us * 80 / 100);
        down_us = (uint32_t)((uint64_t)budget_us * 55 / 100);
        enabled = (n_levels > 1);         /* nothing to govern with a single level */
        g_clock_gov_active = enabled;     /* published so main.c never re-derives it */
        l_info("clock-gov: %s levels=%d [%d..%dMHz] budget=%uus up=%uus down=%uus (graphics.txt clock)",
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

#ifdef DEBUG_SOLOADER
    /* Diagnostic heartbeat: confirm the governor's settled clock in logging builds. */
    { static uint32_t hb = 0;
      if (((++hb) & 0xFFu) == 0u)
          l_info("clock-gov: heartbeat ARM=%dMHz sim=%uus present=%uus (down<%u up>%u)",
                  levels[cur_idx], sim_us, pdt, down_us, up_us); }
#endif

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
    /* Metrics::mSoftwareSkinningTime — the engine's OWN per-frame software-skinning
     * timer. Every other Metrics:: field was already resolved here; this one never
     * was, which is why "is the collapse actually skinning?" has only ever been
     * argued from inference. Read as raw bits and reported both ways (float seconds
     * and u32) because the type is not provable from the symbol table alone — it is
     * 4 bytes in .bss like mFrameTime, so float seconds is the likely convention.
     * READ-ONLY: nothing writes through this pointer. */
    uint32_t *software_skinning_time_bits;
    /* Metrics::mShadowFrameTime — a TWO-float ring (8 bytes + its own index),
     * written by GameRender::RenderFrame. Confirmed live by cross-referencing the
     * GOT: both GameRender::RenderFrame (the producer) and Metrics::NewFrame (the
     * per-frame reset) reference it. This is what finally puts a number on the
     * shadows lever, which profiles currently toggle on feel alone. */
    uint32_t *shadow_frame_time_bits;   /* [2] ring */
    uint32_t *shadow_frame_time_index;
    /* Metrics::mTotalScriptGCTime + mScriptGCNum — Lua GC cost, written by
     * Metrics::ScriptGarbageCollect. A classic source of periodic hitches that
     * nothing in this loader has ever looked at. */
    uint32_t *script_gc_time_bits;
    uint32_t *script_gc_num;
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
    g_metrics_diag.software_skinning_time_bits =
        metrics_u32_symbol("_ZN7Metrics21mSoftwareSkinningTimeE");
    g_metrics_diag.shadow_frame_time_bits =
        metrics_u32_symbol("_ZN7Metrics16mShadowFrameTimeE");
    g_metrics_diag.shadow_frame_time_index =
        metrics_u32_symbol("_ZN7Metrics21mShadowFrameTimeIndexE");
    g_metrics_diag.script_gc_time_bits =
        metrics_u32_symbol("_ZN7Metrics18mTotalScriptGCTimeE");
    g_metrics_diag.script_gc_num =
        metrics_u32_symbol("_ZN7Metrics12mScriptGCNumE");
    g_metrics_diag.initialized = 1;
    l_info("SKINPROBE: engine timers skin=%p shadow=%p shadowIdx=%p gcTime=%p gcNum=%p",
           (void *)g_metrics_diag.software_skinning_time_bits,
           (void *)g_metrics_diag.shadow_frame_time_bits,
           (void *)g_metrics_diag.shadow_frame_time_index,
           (void *)g_metrics_diag.script_gc_time_bits,
           (void *)g_metrics_diag.script_gc_num);
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
#ifdef DEBUG_SOLOADER
    if (repairs && repairs_size > 0) {
        const size_t used = strlen(repairs);
        if (used < repairs_size - 1U) {
            snprintf(repairs + used, repairs_size - used, "%s%s", used ? "," : "", name);
        }
    }
#else
    (void)name;
    (void)repairs;
    (void)repairs_size;
#endif
    return 1;
}

#ifdef DEBUG_SOLOADER
static int diag_read_u8(const uint8_t *slot) {
    return slot ? (int)*slot : -1;
}

static uint32_t diag_read_u32(const uint32_t *slot) {
    return slot ? *slot : 0U;
}
#endif

/* From java.c — the low-res the game renders into the FBO before upscaling. */
extern int mcsm_get_render_scale_width(void);
extern int mcsm_get_render_scale_height(void);

static void force_native_render_dimensions(const char *phase) {
#ifdef DEBUG_SOLOADER
    static uint32_t force_count = 0;
    static uint32_t change_count = 0;
#else
    (void)phase;
#endif

    if (!g_render_gate_diag.initialized) {
        return;
    }

    /* RENDER-SCALE FIX 2026-06-29: force the engine's render dimensions to the
     * RENDER-SCALE size (= the FBO size), not hardcoded native 960x544. With
     * graphics.txt (resolution) set to a lower res, the engine was still rendering full
     * native into the smaller FBO -> the image filled only a corner / spilled off
     * one side. Matching the engine dims to the FBO makes it render fullscreen at
     * the low res, then gl_swap bilinear-upscales the FBO to native. At native
     * (no override) render_scale = 960x544 so behaviour is unchanged. */
    const uint32_t rw = (uint32_t)mcsm_get_render_scale_width();
    const uint32_t rh = (uint32_t)mcsm_get_render_scale_height();

#ifdef DEBUG_SOLOADER
    const uint32_t before_dw = diag_read_u32(g_render_gate_diag.device_width);
    const uint32_t before_dh = diag_read_u32(g_render_gate_diag.device_height);
    const uint32_t before_gw = diag_read_u32(g_render_gate_diag.game_width);
    const uint32_t before_gh = diag_read_u32(g_render_gate_diag.game_height);
    const int changed =
        before_dw != rw ||
        before_dh != rh ||
        before_gw != rw ||
        before_gh != rh;
#endif

    /* ★ 2026-08-06 — CORRECT DRIFT, DO NOT REWRITE AGREEMENT.
     *
     * These four were stored unconditionally on every call. Device log over a
     * 25-minute session: 480,909 calls, FIVE of which changed anything -- one at
     * init, one 544x408->720x408, and three fixing a 3-pixel 720x405 drift the
     * engine re-introduces. Everything else rewrote a value that already matched.
     *
     * That is not free. It runs 4-5 times per frame on the sim thread, and each
     * store is a cross-thread write to an engine global the render thread reads --
     * the same "stomping a global from another thread while the engine reads it"
     * hazard the animation flags are documented for. Comparing first keeps every
     * correction (there are several calls per frame, so real drift is still fixed
     * within the same frame) while leaving the engine alone whenever it already
     * agrees, which is essentially always. */
    if (g_render_gate_diag.device_width && *g_render_gate_diag.device_width != rw) {
        *g_render_gate_diag.device_width = rw;
    }
    if (g_render_gate_diag.device_height && *g_render_gate_diag.device_height != rh) {
        *g_render_gate_diag.device_height = rh;
    }
    if (g_render_gate_diag.game_width && *g_render_gate_diag.game_width != rw) {
        *g_render_gate_diag.game_width = rw;
    }
    if (g_render_gate_diag.game_height && *g_render_gate_diag.game_height != rh) {
        *g_render_gate_diag.game_height = rh;
    }

#ifdef DEBUG_SOLOADER
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
#endif
}


/* ★ 2026-08-06 — THIS FORCING IS THE RECOVERY PATH, NOT JUST A BOOT NUDGE.
 *
 * Application::msbApplicationActive and msbWaitForMessages are both BSS (they
 * start at 0/0) and on Vita there is no Android Activity to ever set them back
 * to "active". They are, however, written to the INACTIVE state by SDL's
 * app-lifecycle events. So anything that queues an SDL pause -- a power
 * notification, a common dialog, the PS button -- parks the engine in
 * "inactive / waiting for window messages" permanently.
 *
 * 1.10 restricted this to the pre-gameplay boot window (`if (g_boot_scene_active)
 * return;`) on the theory that the engine owns its own state once gameplay is
 * live. It does not: nothing on this platform ever re-activates it. That is the
 * "game freezes with audio still playing, have to quit and relaunch" report --
 * FMOD keeps streaming on its own thread while the engine loop sits inactive.
 *
 * Re-asserting every frame is cheap (two byte stores) and is what every build
 * before 1.10 did. Keep it unconditional: a transient inactive state then
 * self-heals on the next frame instead of wedging the session. */
static void force_application_active(const char *phase) {
#ifdef DEBUG_SOLOADER
    static uint32_t log_count = 0;
    const int had_wait = diag_read_u8(g_render_gate_diag.app_wait_for_messages);
    const int had_active = diag_read_u8(g_render_gate_diag.app_active);
#else
    (void)phase;
#endif

    /* Same rule as force_native_render_dimensions: compare, then write only on a
     * real mismatch. Device log: these changed EXACTLY ONCE in 25 minutes
     * (`AppState force[loop-pre] wait=0->0 active=0->1`, at boot) -- nothing on
     * this platform ever sets msbApplicationActive, so that one write matters and
     * the force stays. Every other frame was rewriting values that already agreed,
     * from the sim thread, into globals the engine reads elsewhere. */
    if (g_render_gate_diag.app_wait_for_messages && *g_render_gate_diag.app_wait_for_messages != 0) {
        *g_render_gate_diag.app_wait_for_messages = 0;
    }
    if (g_render_gate_diag.app_active && *g_render_gate_diag.app_active != 1) {
        *g_render_gate_diag.app_active = 1;
    }

#ifdef DEBUG_SOLOADER
    if ((had_wait != 0 || had_active != 1) &&
        (log_count < 32U || (log_count % 256U) == 0U)) {
        l_info("Diag: AppState force[%s] wait=%d->0 active=%d->1",
               phase ? phase : "?",
               had_wait,
               had_active);
        log_count++;
    }
#endif
}

#ifdef DEBUG_SOLOADER
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
#endif

static void metrics_diag_tick(uint32_t loop_count, const char *phase) {
#ifdef DEBUG_SOLOADER
    char repairs[96] = {0};
#else
    char *repairs = NULL;
#endif
    int repair_count = 0;

    repair_count += repair_metric_float(g_metrics_diag.frame_time,
                                        "mFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.actual_frame_time,
                                        "mActualFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.average_frame_time,
                                        "mAverageFrameTime",
                                        1.0f / 30.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.total_time,
                                        "mTotalTime",
                                        0.0f,
                                        0.0f,
                                        86400.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.scale,
                                        "mScale",
                                        1.0f,
                                        0.0f,
                                        10.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    /* Metrics::NewFrame stores -1.0 in mNextFrameTime as a sentinel. */
    repair_count += repair_metric_float(g_metrics_diag.fixed_time_step,
                                        "mFixedTimeStep",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.delay,
                                        "mDelay",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif
    repair_count += repair_metric_float(g_metrics_diag.min_frame_time,
                                        "mMinFrameTime",
                                        0.0f,
                                        0.0f,
                                        5.0f,
                                        repairs,
#ifdef DEBUG_SOLOADER
                                        sizeof(repairs));
#else
                                        0);
#endif

#ifdef DEBUG_SOLOADER
    log_metrics_diag(loop_count, phase, repair_count, repairs);
#else
    (void)loop_count;
    (void)phase;
    (void)repair_count;
#endif
}

static int playback_dt_is_usable(float value);

/* ANIMATION DELTA GOVERNOR ------------------------------------------------
 *
 * Metrics::NewFrame (disassembled 2026-08-06 at 0x00c71d58) computes
 *     frameTime = elapsed - Metrics::mMinFrameTime
 * and then hard-clamps it:
 *     if (frameTime > 5.0f)  frameTime = <const>
 *     if (frameTime > 0.1f)  frameTime = 0.1f      <-- 0x3dcccccd at 0xc7212c
 *
 * mMinFrameTime is BSS and nothing on this platform writes it, so the
 * subtraction is a no-op and the engine's delta is true elapsed -- RIGHT UP TO
 * THE 0.1s CLAMP. Below 10fps (heavy crowd scenes, and every scene-load hitch)
 * the engine advances animation by at most 100ms per frame while real time and
 * the FMOD audio clock advance by more. That is slow-motion animation that
 * drifts behind dialogue, and it gets worse the heavier the scene -- which is
 * exactly the "animations play at the wrong speed" and "facial animation breaks
 * in high-load scenes" reports.
 *
 * 1.10 removed this governor and left Metrics::NewFrame native. The intent was
 * right -- the engine keeps a safe delta and a real delta and controllers pick
 * between them, so flattening both is lossy -- but the 0.1s clamp means native
 * is not correct either.
 *
 * So: widen the ceiling to 0.25s instead of removing the correction. Normal
 * frames are untouched (the engine's own value is already true elapsed and well
 * under the clamp, so the >0.5ms guard below simply never fires). Only frames
 * the engine would have clamped get repaired, and genuine multi-second stalls
 * still clamp so a scene load cannot fling the pose across the room. */
#define ANIM_DT_MIN_SECONDS   (1.0f / 240.0f)
#define ANIM_DT_MAX_SECONDS   (1.0f / 4.0f)
#define ANIM_STALL_RESET_US   500000ULL

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

    if (elapsed_us >= 4000ULL && elapsed_us <= ANIM_STALL_RESET_US) {
        target = clamp_animation_dt((float)elapsed_us / 1000000.0f);
    } else if (elapsed_us > ANIM_STALL_RESET_US) {
        /* Never feed a multi-second load pause straight into simulation, but do
         * account for a useful slice of it. Reusing the previous small dt dropped
         * the whole pause while audio kept playing, which is what permanently
         * moved mouths and animation behind the soundtrack. */
        target = ANIM_DT_MAX_SECONDS;
    } else if (playback_dt_is_usable(engine_frame_time)) {
        target = clamp_animation_dt(engine_frame_time);
    } else if (playback_dt_is_usable(engine_actual_frame_time)) {
        target = clamp_animation_dt(engine_actual_frame_time);
    }

    /* Animation and audio must see the same wall-clock cadence, so no EMA here:
     * smoothing intentionally lags variable-rate scenes by a frame or more. */
    g_anim_governed_dt = target;
    g_anim_governor_initialized = 1;

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

    const float fixed_dt = clamp_animation_dt(dt);
    const float old_ft = g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : fixed_dt;
    const float old_aft = g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : fixed_dt;
    const float old_total = g_metrics_diag.total_time ? *g_metrics_diag.total_time : 0.0f;
    int changed = 0;

    /* Only correct a delta the engine actually got wrong. On a normal frame the
     * engine's value already IS the elapsed time, both of these differ by well
     * under half a millisecond, and this whole function is a no-op -- which is
     * what preserves the engine's own safe/actual distinction in the common
     * case and limits the rewrite to frames the 0.1s clamp truncated. */
    if (g_metrics_diag.frame_time && abs_float_delta(*g_metrics_diag.frame_time, fixed_dt) > 0.0005f) {
        *g_metrics_diag.frame_time = fixed_dt;
        changed = 1;
    }
    /* ★ 2026-08-06 — DO NOT TOUCH mActualFrameTime.
     *
     * Metrics::NewFrame produces TWO clocks on purpose: mFrameTime is the safe,
     * clamped delta (capped at 0.1s) and mActualFrameTime is the real wall-clock
     * delta. Controllers pick between them, and the real one is how audio-driven
     * playback catches up after a hitch.
     *
     * Overwriting it destroyed exactly that. Device log after a 2.6s load:
     *     ANIM: metrics fixed  ft 0.100000->0.250000  aft 2.585000->0.250000
     * The engine had the correct 2.585s and we replaced it with 0.25s, throwing
     * away 2.3s of catch-up information it was relying on -- which is the
     * "faces/audio out of sync after a hard load" behaviour.
     *
     * Repair only the clock the engine actually clamped. The real one is already
     * right by construction, so leaving it alone is both more correct and less
     * work. */
    /* averageFrameTime, fixedTimeStep and delay have separate engine meanings;
     * flattening them to one value destabilizes pacing and simulation. */
    if (g_metrics_diag.total_time &&
        playback_dt_is_usable(old_ft) &&
        old_ft <= 1.0f &&
        *g_metrics_diag.total_time >= old_ft) {
        /* Metrics::NewFrame already folded old_ft into mTotalTime. Apply the
         * difference in both directions so the clock the engine reports stays
         * consistent with the deltas we just handed it. */
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
 * settings/graphics.txt `skinning` = "0" -> disable (faster, animation degraded),
 * absent or "1" -> full (default, current behaviour). */
static int mcsm_anim_full(void) { return mcsm_cfg()->skinning_full; }

/* ANIMATION UPDATE-RATE CONTROL (2026-07-20, priority-3 sim lever). The heaviest
 * scenes spend ~50ms in the sim, dominated by animation blend across many
 * PlaybackControllers. Advancing them every Nth frame with N*accumulated-dt halves
 * (N=2) the blend cost at CORRECT wall-clock speed (no slow-mo). Opt-in + tunable:
 * settings/graphics.txt `anim_rate` = 1 (full, default) / 2 (half) / 3 (third).
 * Visual trade: close-up motion + lip-sync step at ~15Hz (N=2), so it's for heavy
 * gameplay/crowd scenes, not dialogue. Default 1 = zero change. */
static int mcsm_anim_rate(void) {
    /* Consolidated into settings/graphics.txt (`anim_rate`). mcsm_cfg() caches, so this is
     * just a field read; the value is already clamped to 1..3 at parse time. */
    const int rate = mcsm_cfg()->anim_rate;
#ifdef DEBUG_SOLOADER
    static int logged = 0;
    if (!logged) { logged = 1; l_info("ANIM: update-rate = 1/%d", rate); }
#endif
    return rate;
}

static void force_animation_runtime_flags(const char *phase) {
    static uint32_t count = 0;
    static int last_recursive_full = -1;

    /* ★ ON BY DEFAULT, AND IT MUST STAY THAT WAY (2026-08-01).
     *
     * These flags were briefly defaulted OFF on the theory that forcing
     * GameEngine::mbFixRecursiveAnimationContribution from our thread was racing the
     * engine's animation update and causing the reported intermittent head detachment.
     * DEVICE RESULT: animation got dramatically WORSE, not better. The theory was
     * wrong and the header comment above already said so -- the recursive-bone
     * contribution fix "is what makes skeletal animation work". The engine's own
     * default leaves it off; forcing it on is the thing holding skeletons together.
     *
     * So the residual "heads disconnect sometimes" is NOT caused by this override --
     * it is what remains WITH the override doing its job, and the next suspects are
     * elsewhere (detail/LOD picking a character mesh with different skinning weights,
     * or gpu_tier selecting a cheaper engine path).
     *
     * `anim_engine_flags = off` in graphics.txt still exists, but only to reproduce
     * the broken state for comparison. It is not a performance setting. */
    if (!mcsm_cfg()->anim_engine_flags) {
        if (count == 0U) {
            l_warn("ANIM: engine animation flags NOT forced (anim_engine_flags = off) — "
                   "skeletal animation is known to break without them.");
            count = 1U;
        }
        return;
    }

    resolve_animation_runtime_flags();
    /* Function scope so the diagnostic below can print what was ACTUALLY applied. */
    static int s_nonskel = -1;
    const int recursive_full = mcsm_anim_full();
    const int recursive_value =
        g_fix_recursive_animation_contribution ? (int)*g_fix_recursive_animation_contribution : -1;
    const int refresh = (count < 8U) || ((count & 0x3fU) == 0U);
    const int recursive_needs_write =
        refresh || last_recursive_full != recursive_full || recursive_value != recursive_full;

    /* This was forced to 1 unconditionally with no config and no measurement.
     * Including non-skeleton chores means MORE chores walked per frame, and the
     * chore tick is a nested linked-list walk (UpdateChoreInstances -> per-chore
     * -> per-agent) that scales directly with how many characters are on screen
     * -- which is exactly the reported symptom: fine with one or two characters,
     * collapses with more. Make it settable so its real cost can be measured
     * rather than assumed: anim_nonskel.txt = 0 disables it. */
    if (g_set_chore_filter_includes_non_skeleton && refresh) {
        if (s_nonskel < 0) {
            /* Default follows the profile rather than being hardcoded on.
             * Including non-skeleton chores means MORE entries in a walk that is
             * already O(chores x agents) on one thread, and it was forced to 1
             * with no measurement behind it. The max-fps profile turns it off
             * along with the other per-character costs; quality/default keep it,
             * since it is the engine's corrected behaviour. graphics.txt
             * `anim_nonskel` overrides; -1 there means "follow skinning". */
            const int want = mcsm_cfg()->anim_nonskel;
            s_nonskel = (want < 0) ? (mcsm_cfg()->skinning_full ? 1 : 0) : (want ? 1 : 0);
            l_info("ANIM: chore filter includes non-skeleton = %d (%s)", s_nonskel,
                   (want < 0) ? "from skinning" : "graphics.txt anim_nonskel");
        }
        g_set_chore_filter_includes_non_skeleton(s_nonskel);
    }
    /* BOTH the setter AND the direct write, deliberately. Dropping the raw write on
     * 2026-08-01 (reasoning: two writers for one piece of state) made animation
     * WORSE on device, so the direct poke is doing something the setter alone does
     * not -- most likely the setter only latches a value the engine re-reads later,
     * while the live global is what the animation update actually consults. Restored.
     * Do not remove it again without a device test proving animation still works. */
    if (g_set_fix_recursive_animation_contribution && recursive_needs_write) {
        g_set_fix_recursive_animation_contribution(recursive_full);
    }
    if (g_fix_recursive_animation_contribution && recursive_needs_write) {
        *g_fix_recursive_animation_contribution = (uint8_t)recursive_full;
    }
    last_recursive_full = recursive_full;

    count++;
    if (count <= 8U || (count & 0x1ffU) == 0U) {
        /* ☠ nonSkeleton was a LITERAL "1" in this format string, so the line
         * reported the chore filter as enabled even when anim_nonskel had turned
         * it off. A diagnostic that states a value it never read is worse than no
         * diagnostic -- this project reads these logs to decide what is broken. */
        l_info("ANIM: forced runtime flags #%u phase=%s nonSkeleton=%d recursive=%d value=%u",
               count,
               phase ? phase : "?",
               s_nonskel,
               recursive_full,
               g_fix_recursive_animation_contribution ? (unsigned)*g_fix_recursive_animation_contribution : 0U);
    }
}

/* CHARACTER-SCALING PROBE (2026-07-30). Reported symptom: one or two characters
 * hold 60fps, more and the frame collapses, with a single CPU core pinned at 99%
 * while the others idle -- the signature of a serial per-character walk.
 *
 * The chore tick is exactly that shape, verified by disassembly:
 *     ChoreInst::UpdateChoreInstances()   linked-list loop over every chore
 *       -> ChoreInst::Update(bool)        linked-list loop over every agent
 *          -> ChoreAgentInst::Update      -> SetCurrentTime -> PlaybackController
 * so its cost is O(chores x agents) on one thread, per frame.
 *
 * Counting the top-level walk tells us whether the collapse tracks character
 * count or something else. ChoreInst::UpdateChoreInstances is called ONCE per
 * frame -- not per chore -- so hooking it is cheap and, unlike the render-path
 * hooks that crashed, it is not called concurrently from multiple threads. */
#if MCSM_SIM_PROBES
static so_hook g_hook_chore_update_all;
#endif
static volatile unsigned g_chore_ticks = 0;
/* ---- ENGINE TUNABLE GLOBALS (2026-07-31) ----------------------------------
 * The engine exposes several plain globals that configure the render and audio
 * systems. They are ordinary exported data symbols, so they can be read and written
 * directly -- no hooking, no instruction patching, no SO_CONTINUE.
 *
 * Values as SHIPPED, read out of the .so (vaddr translated through the ELF program
 * headers -- reading at raw vaddr gives garbage, which is how a first attempt got
 * "mbEnableRendering = 249"; the 1.3333 aspect ratio is what confirms the mapping):
 *     gMultithreadRenderEnable          = 1        (already on)
 *     RenderDevice::mDepthSize          = 24
 *     Scene::sMinRenderedScenePriority  = -10000
 *     Scene::sMaxRenderedScenePriority  = +10000
 *     AudioThread::snMaxFmodChannels    = 128
 *     RenderDevice::sfGameContentAspectRatio = 1.3333 (4:3)
 *
 * ENGTUNE logs the LIVE values at boot. That matters more than the overrides: a
 * shipped default read from the binary is not proof of what the engine settles on
 * after init, and this port has repeatedly been burned by assuming otherwise.
 * Overrides all default to "leave alone". */
static void mcsm_engine_tunables(void) {
    struct { const char *sym; const char *name; int is_byte; } items[] = {
        { "gMultithreadRenderEnable",                 "multithread_render", 1 },
        { "_ZN12RenderDevice10mDepthSizeE",           "depth_size",         0 },
        { "_ZN5Scene25sMinRenderedScenePriorityE",    "scene_prio_min",     0 },
        { "_ZN5Scene25sMaxRenderedScenePriorityE",    "scene_prio_max",     0 },
        { "_ZN19SoundSystemInternal11AudioThread7Context17snMaxFmodChannelsE",
                                                      "fmod_max_channels",  0 },
    };
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        uintptr_t a = so_symbol(&so_mod_gameengine, items[i].sym);
        if (!a) { l_info("ENGTUNE: %-18s <symbol not found>", items[i].name); continue; }
        if (items[i].is_byte)
            l_info("ENGTUNE: %-18s = %d", items[i].name, (int)*(volatile uint8_t *)a);
        else
            l_info("ENGTUNE: %-18s = %d", items[i].name, (int)*(volatile int32_t *)a);
    }

    /* Optional overrides. Each is a straight store into an engine global.
     * fmod_max_channels is the one with a real CPU story: our FMOD output plugin
     * pulls the mixer INLINE on the drain thread, so the channel cap bounds how much
     * mixing that thread can be asked to do. It is a CAP though, not a count --
     * lowering it only helps if the game actually opens many channels, which is
     * exactly what the ENGTUNE line above lets us find out before guessing. */
    const McsmCfg *cfg = mcsm_cfg();
    if (cfg->fmod_channels > 0) {
        uintptr_t a = so_symbol(&so_mod_gameengine,
            "_ZN19SoundSystemInternal11AudioThread7Context17snMaxFmodChannelsE");
        if (a) {
            *(volatile int32_t *)a = cfg->fmod_channels;
            l_info("ENGTUNE: fmod_max_channels -> %d (graphics.txt)", cfg->fmod_channels);
        }
    }
    if (cfg->scene_prio_min || cfg->scene_prio_max) {
        uintptr_t lo = so_symbol(&so_mod_gameengine, "_ZN5Scene25sMinRenderedScenePriorityE");
        uintptr_t hi = so_symbol(&so_mod_gameengine, "_ZN5Scene25sMaxRenderedScenePriorityE");
        /* ☠ This narrows the range of scene priorities the engine will RENDER, so it
         * is a direct draw-count lever -- and equally a direct way to make the UI or
         * the world vanish. Both ends must be given together or not at all. */
        if (lo && hi && cfg->scene_prio_min < cfg->scene_prio_max) {
            *(volatile int32_t *)lo = cfg->scene_prio_min;
            *(volatile int32_t *)hi = cfg->scene_prio_max;
            l_warn("ENGTUNE: scene priority range -> [%d..%d] — if content disappears, "
                   "this is why", cfg->scene_prio_min, cfg->scene_prio_max);
        } else {
            l_warn("ENGTUNE: scene priority override IGNORED (need min < max, got %d..%d)",
                   cfg->scene_prio_min, cfg->scene_prio_max);
        }
    }
}

/* ---- ENGINE PLATFORM IDENTITY (2026-07-31) --------------------------------
 * TTPlatform::GetPlatformType() is a TWO-INSTRUCTION STUB in the shipped binary:
 *     mov r0, #8      ; ePlatform_Android
 *     bx  lr
 * ePlatform_Vita is 9 and has never been used by this port. Enum order was taken
 * from the string table's ADDRESS layout (16-byte stride at 0xF64B78+), not from
 * sorted `strings` output, which is alphabetical and would have given the wrong index:
 *     0 None 1 All 2 PC 3 Wii 4 Xbox 5 PS3 6 Mac 7 iPhone
 *     8 Android  9 Vita  10 Linux 11 PS4 12 XBOne 13 WiiU 14 Count
 *
 * It feeds only five call sites, three of them render-critical:
 *     RenderConfiguration::Initialize()
 *     RenderConfiguration::GetSupportedQualityTypes()
 *     T3EffectCacheInternal::GetProgram(...)   <- picks the shader program PER DRAW
 * and the effect system filters features by platform AND quality
 * (T3EffectUtil::GetValidDynamicFeatures(type, features, quality, PlatformType)).
 * With render at 75% of the frame, that makes this the most direct untested lever.
 *
 * OFF BY DEFAULT and behind graphics.txt `platform_vita`, because the per-platform
 * quality table lives in .bss and is built during init -- so whether Vita and Android
 * differ AT ALL cannot be determined offline. The PLATQUAL line below answers that on
 * device by asking the engine for BOTH, which is cheaper than shipping a guess. */
#define MCSM_EPLATFORM_ANDROID 8
#define MCSM_EPLATFORM_VITA    9

static so_hook g_hook_get_platform_type;
static int hook_get_platform_type(void) {
    /* The hook is installed only when the latched platform_vita option is enabled.
     * Avoid re-reading config and updating a diagnostic counter on this per-draw call. */
    return MCSM_EPLATFORM_VITA;
}

/* Ask the engine which render quality types each platform supports. The function is
 * `void GetSupportedQualityTypes(uint32_t *out, PlatformType)` -- r0 is an OUT pointer,
 * r1 the platform -- and it guards on a table pointer that is NULL until the render
 * config initialises, so `out` is left untouched when called too early. Seeded with a
 * sentinel so "not yet initialised" is distinguishable from "returned 0". */
void mcsm_report_platform_quality(void) {
    typedef void (*get_qual_fn)(uint32_t *, int);
    get_qual_fn f = (get_qual_fn)so_symbol(&so_mod_gameengine,
        "_ZN19RenderConfiguration24GetSupportedQualityTypesE12PlatformType");
    if (!f) { l_info("PLATQUAL: GetSupportedQualityTypes(PlatformType) not found"); return; }
    uint32_t a = 0xDEADBEEFu, v = 0xDEADBEEFu;
    f(&a, MCSM_EPLATFORM_ANDROID);
    f(&v, MCSM_EPLATFORM_VITA);
    /* The signature is confirmed against the binary, not guessed: at 0x00aa9d24 the
     * function takes r0 = sret out-pointer, r1 = PlatformType, and ends
     * `cmp r3,#0 / addne r1,r1,#28 / ldrne r3,[r3,r1,lsl #2] / str r3,[r0] / bx lr`.
     *
     * Note the store is UNCONDITIONAL, so "not built yet" writes 0 rather than
     * leaving the sentinel. Testing for the sentinel therefore never fired, and an
     * empty table was reported as "Android == Vita -> platform_vita cannot help" --
     * a firm conclusion drawn from no data, about the single most direct untested
     * perf lever this port has. Zero on BOTH sides means the table is empty; a
     * platform with genuinely no supported quality types would be a different and
     * far stranger finding, and would still be worth reporting as unbuilt. */
    if ((a == 0xDEADBEEFu && v == 0xDEADBEEFu) || (a == 0u && v == 0u)) {
        l_info("PLATQUAL: quality table not built yet (called before render init)");
        return;   /* not latched: this is the "ask again later" case */
    }
    l_info("PLATQUAL: supported quality types  Android(8)=0x%08X  Vita(9)=0x%08X  -> %s",
           a, v, (a == v) ? "IDENTICAL, platform_vita cannot help here"
                          : "DIFFERENT, platform_vita changes the engine's quality set");
}

/* ---- ACHIEVEMENT PROBE (2026-07-30) ---------------------------------------
 * The game's Lua already calls PlatformUnlockAchievement("<name>") at the right
 * story beats -- the engine exposes luaPlatformUnlockAchievement ->
 * TTPlatform::UnlockAchievement -> Platform_Android::UnlockAchievement, and on
 * Vita that last one goes nowhere (it was Google Play Games on Android).
 *
 * Before ANY trophy work is possible we need the exact achievement name strings,
 * because a Vita TROPHY.TRP has to map name -> trophy index and nothing documents
 * them. So: log what the engine passes, then continue unchanged. This unlocks
 * nothing by itself and needs no plugin -- it is purely the discovery step.
 *
 * SAFE TO HOOK: this is a cold, script-thread function taking a single reference
 * (pointer) argument. No floats, so the SO_CONTINUE float-argument corruption
 * hazard does not apply, and it is nowhere near the render path.
 *
 * The Telltale `String` layout is NOT documented here, so rather than guess we
 * dump the first 16 bytes AND a best-effort text read. One device run settles the
 * layout for good instead of shipping a wrong assumption. */
/* Resolve a Telltale `String` to a C string without assuming its layout.
 * The layout is NOT documented in this port, so both plausible forms are tried and
 * VALIDATED before use: a char* at +0 (the usual Telltale shape) or characters stored
 * inline in the object (small-string optimisation). A wrong guess here would fault
 * inside the engine's script thread, so every candidate pointer is range-checked and
 * every byte is required to be printable before it is trusted.
 * Returns 1 and fills `out` on success. */
static int telltale_string_read(const void *str, char *out, size_t cap) {
    if (!str || !out || cap < 2u) return 0;
    const uint32_t *w = (const uint32_t *)str;
    const uint32_t p0 = w[0];
    if (p0 > 0x1000u && p0 < 0xF0000000u) {          /* candidate: char* at +0 */
        const char *cand = (const char *)(uintptr_t)p0;
        size_t n = 0;
        while (n < cap - 1u) {
            const char c = cand[n];
            if (c == 0) break;
            if (c < 0x20 || (unsigned char)c > 0x7E) { n = 0; break; }
            out[n] = c; n++;
        }
        if (n) { out[n] = 0; return 1; }
    }
    { const char *inl = (const char *)str;           /* candidate: inline chars */
      size_t n = 0;
      while (n < cap - 1u && inl[n] >= 0x20 && (unsigned char)inl[n] <= 0x7E) { out[n] = inl[n]; n++; }
      if (n) { out[n] = 0; return 1; } }
    return 0;
}

static so_hook g_hook_unlock_achievement;
static void hook_unlock_achievement(void *self, const void *str) {
    char name[80];
    if (str && telltale_string_read(str, name, sizeof(name))) {
        /* Hand it to the trophy backend. That call does NOT block on the unlock
         * itself -- it flags the id and wakes a worker -- which matters because we
         * are on the engine's script thread mid-scene here. It also owns the
         * logging: the engine re-fires the same achievement whenever its beat
         * replays, and only the backend knows whether the trophy is already held. */
        extern void mcsm_trophies_unlock_by_name(const char *);
        mcsm_trophies_unlock_by_name(name);
    } else {
        /* Keep the raw bytes in the log: if the String layout ever changes this is
         * what identifies it, instead of a silent "(unreadable)". */
        const uint32_t *w = (const uint32_t *)str;
        l_warn("ACHIEVEMENT unlock: name unreadable | String bytes %08X %08X %08X %08X",
               str ? w[0] : 0u, str ? w[1] : 0u, str ? w[2] : 0u, str ? w[3] : 0u);
    }
    SO_CONTINUE_VOID(g_hook_unlock_achievement, self, str);
}

#if MCSM_SIM_PROBES
static so_hook g_hook_scene_update, g_hook_script_update;
static void hook_scene_update(void) {
    const uint64_t t0 = MCSM_PHASE_T0();
    SO_CONTINUE_VOID(g_hook_scene_update);
    MCSM_PHASE_ADD(g_scene_us, g_scene_n, t0);
}
static void hook_script_update(uint32_t dt_bits) {
    /* float arg arrives in r0 on this softfp build; pass the bits straight through
     * so nothing reinterprets it (SO_CONTINUE would promote a real float to double). */
    const uint64_t t0 = MCSM_PHASE_T0();
    SO_CONTINUE_VOID(g_hook_script_update, dt_bits);
    MCSM_PHASE_ADD(g_script_us, g_script_n, t0);
}
#endif /* MCSM_SIM_PROBES */

#if MCSM_SIM_PROBES
static void hook_chore_update_all(void) {
    g_chore_ticks++;
    const uint64_t t0 = MCSM_PHASE_T0();
    SO_CONTINUE_VOID(g_hook_chore_update_all);
    MCSM_PHASE_ADD(g_chore_us, g_chore_n, t0);
}
#endif

void mcsm_anim_report(void) {
    static unsigned last = 0;
    unsigned n = g_chore_ticks;
    if (n == last) return;
    l_info("ANIM: chore ticks=%u (+%u since last report)", n, n - last);
    last = n;
    /* mcsm_skin_report is deliberately NOT called from here. This function returns
     * early when the chore counter has not moved, which would suppress the whole
     * upload/skinning probe in menus, during scene loads, and in any scene with no
     * active chores. The watchdog calls both independently (main.c). */
}

/* SKINPROBE reporter (2026-07-30). Answers, per report window, the one question
 * the whole "collapses with more characters" thread rests on: how much work the
 * per-skinned-mesh upload path is doing per frame, and what the engine itself
 * says software skinning costs.
 *
 * Read these as follows:
 *   locks/f    T3VertexBuffer::PlatformLock calls per frame. This IS the skinned
 *              mesh count per frame (DoSoftwareSkinning -> Lock, one per mesh).
 *              If this rises with on-screen characters, the path scales with them.
 *   respec/f   full glBufferData respecifies per frame. Each one is a GPU free +
 *              GPU alloc + full-buffer memcpy inside vitaGL, NOT a partial update.
 *   KB/f       bytes copied CPU->GPU per frame by those respecifies alone.
 *   malloc/f   CPU staging buffers allocated AND freed per frame.
 *   skin       Metrics::mSoftwareSkinningTime, the engine's own timer. Printed as
 *              float-seconds->ms and as raw u32, since the type is not provable
 *              from the symbol table; whichever column is plausible is the real one.
 * Runs on the watchdog thread every 5s. Deltas, so no shared per-frame reset. */
void mcsm_skin_report(void) {
#ifndef DEBUG_SOLOADER
    return;
#else
    static uint32_t last_locks = 0, last_respec = 0, last_kb = 0, last_mallocs = 0;
    static uint32_t last_frames = 0;

    const uint32_t locks   = g_vb_locks;
    const uint32_t respec  = g_vb_respec;
    const uint32_t kb      = g_vb_kb;
    const uint32_t mallocs = g_vb_mallocs;
    const uint32_t frames  = launch_state_get_present_count();

    /* Unsigned subtraction wraps correctly, so a counter rollover still yields a
     * usable delta rather than a nonsense huge number. */
    const uint32_t dframes_raw = frames - last_frames;
    /* ☠ DEVICE-CONFIRMED (r79): the early `if (dframes == 0) return;` that used to
     * be here made this probe emit NOTHING for an entire 86s session. The first
     * report of a run lands during the load screen, when nothing has presented
     * yet -- so it returned before reaching `last_frames = frames`, leaving the
     * baseline latched at 0, and every later report differenced against a stale
     * zero. One fire, zero SKINPROBE/SIMSPLIT lines, silently. Report
     * unconditionally and clamp only the divisor: a report with no presented
     * frames is still evidence (it says the renderer is stalled), which is
     * precisely when the probe is most worth reading. */
    const uint32_t dframes = dframes_raw ? dframes_raw : 1u;

    const uint32_t dlocks   = locks   - last_locks;
    const uint32_t drespec  = respec  - last_respec;
    const uint32_t dkb      = kb      - last_kb;
    const uint32_t dmallocs = mallocs - last_mallocs;

    uint32_t skin_bits = 0;
    if (g_metrics_diag.software_skinning_time_bits)
        skin_bits = *g_metrics_diag.software_skinning_time_bits;
    const float skin_f = float_from_bits(skin_bits);

    /* mShadowFrameTime is a 2-entry ring with its own index. Which slot holds the
     * freshest sample depends on whether GameRender::RenderFrame writes-then-
     * increments or increments-then-writes, and that was NOT established from the
     * disassembly -- so picking one was a coin flip that would have reported a
     * permanently stale value half the time, silently. Report BOTH slots and the
     * index; one device run then settles the convention for good. */
    float shadow_a = 0.0f, shadow_b = 0.0f;
    uint32_t shadow_idx = 0u;
    if (g_metrics_diag.shadow_frame_time_bits) {
        shadow_a = float_from_bits(g_metrics_diag.shadow_frame_time_bits[0]);
        shadow_b = float_from_bits(g_metrics_diag.shadow_frame_time_bits[1]);
        if (g_metrics_diag.shadow_frame_time_index)
            shadow_idx = *g_metrics_diag.shadow_frame_time_index;
    }
    const float gc_f = g_metrics_diag.script_gc_time_bits
                     ? float_from_bits(*g_metrics_diag.script_gc_time_bits) : 0.0f;
    const uint32_t gc_n = g_metrics_diag.script_gc_num ? *g_metrics_diag.script_gc_num : 0u;

    /* x100 fixed point: these are fractions per frame and integer division to 0
     * would hide exactly the signal we are looking for. */
    /* KB/f is the number that converts straight into milliseconds: it is a
     * CPU->GPU copy that the engine's own mapped path would not perform at all.
     * At a realistic Vita large-block copy rate (~500 MB/s) 1 KB is ~2 us, so
     * KB/f x 2 us is a first-order estimate of the time this path costs per
     * frame -- before the GPU alloc/free churn and the malloc/free pair. */
    l_info("SKINPROBE: frames=%u locks/f=%u.%02u respec/f=%u.%02u KB/f=%u.%02u malloc/f=%u.%02u "
           "maxbytes=%u tid=0x%08X subdata=%u reuse=%u evict=%u skin=%.3fms(raw=%u) est=%u.%02ums/f "
           "[locks=%u respec=%u KB=%u malloc=%u]",
           dframes_raw,
           dlocks / dframes,   (dlocks   * 100u / dframes) % 100u,
           drespec / dframes,  (drespec  * 100u / dframes) % 100u,
           dkb / dframes,      (dkb      * 100u / dframes) % 100u,
           dmallocs / dframes, (dmallocs * 100u / dframes) % 100u,
           g_vb_max_bytes, g_vb_tid, g_vb_subdata, g_vb_reuse, g_vb_evict,
           (double)(skin_f * 1000.0f), skin_bits,
           (dkb * 2u / dframes) / 1000u, ((dkb * 2u / dframes) / 10u) % 100u,
           locks, respec, kb, mallocs);
    /* Separate line so the upload probe above stays readable. shadow[] finally
     * puts a measured cost on the shadows lever; gc= exposes Lua GC hitches. */
    const uint32_t caps = g_render_caps_ptr ? *g_render_caps_ptr : 0u;
    /* Per-frame averages of the two animation entry points, against DIP-SIM's
     * 25-27ms total. Whatever is left over is neither chore nor playback. */
    /* ☠ THESE WERE PER-CALL AVERAGES PRINTED UNDER A "/frame" LABEL (fixed 2026-07-30).
     * Each phase was divided by the delta of its OWN call counter, not by frames, so
     * any phase invoked more than once per frame reported 1/N of its real per-frame
     * cost -- silently, because the call counts were never printed. The entire
     * optimisation strategy rests on "render is 75% of the frame", and that reading
     * was only correct if GameEngine::Render happens to run exactly once per frame.
     * Now divided by dframes (true per-frame cost, which is what the label promises)
     * and every phase also prints its calls-per-frame in x0.00 form, so a reader can
     * both trust the number and see if a phase is running more often than assumed. */
    static uint32_t l_cu = 0, l_cn = 0, l_pu = 0, l_pn = 0;
    const uint32_t cu = g_chore_us, cn = g_chore_n, pu = g_pbc_us, pn = g_pbc_n;
    const uint32_t dcn = cn - l_cn, dpn = pn - l_pn;
    const uint32_t chore_pf = (cu - l_cu) / dframes;
    const uint32_t pbc_pf   = (pu - l_pu) / dframes;
    l_cu = cu; l_cn = cn; l_pu = pu; l_pn = pn;
    static uint32_t l_ru=0,l_rn=0,l_su=0,l_sn=0,l_cu2=0,l_cn2=0;
    const uint32_t ru=g_render_us, rn=g_render_n, su=g_scene_us, sn=g_scene_n,
                   cu2=g_script_us, cn2=g_script_n;
    const uint32_t drn=rn-l_rn, dsn=sn-l_sn, dcn2=cn2-l_cn2;
    const uint32_t r_pf  = (ru  - l_ru ) / dframes;
    const uint32_t s_pf  = (su  - l_su ) / dframes;
    const uint32_t sc_pf = (cu2 - l_cu2) / dframes;
    l_ru=ru; l_rn=rn; l_su=su; l_sn=sn; l_cu2=cu2; l_cn2=cn2;
#if !MCSM_SIM_PROBES
    /* ☠ SAY SO. With MCSM_PHASE_ADD compiled to a no-op, every accumulator above
     * stays 0 and the normal line below would render as
     *     SIMSPLIT: render=0.000ms(x0.00) chore=0.000ms(x0.00) ... PER FRAME
     * -- a well-formed measurement claiming every engine phase is free, which is
     * indistinguishable from a real reading of an idle frame. This project has
     * already burned debugging cycles twice on exactly that shape: mcsm_skin_report
     * disabling itself by early-returning before updating its baseline, and a GL
     * dedup reporting success while never firing. A diagnostic that is switched off
     * must announce it, not emit zeros. */
    (void)r_pf; (void)chore_pf; (void)pbc_pf; (void)s_pf; (void)sc_pf;
    (void)drn;  (void)dcn;      (void)dpn;    (void)dsn;  (void)dcn2;
    l_info("SIMSPLIT: phase timers COMPILED OUT (release build) — this is expected "
           "here. The LOGGING build enables them, so send a log from that if you need "
           "render/chore/playback/scenes/script attributed.");
#else
    l_info("SIMSPLIT: render=%u.%03ums(x%u.%02u) chore=%u.%03ums(x%u.%02u) "
           "playback=%u.%03ums(x%u.%02u) scenes=%u.%03ums(x%u.%02u) script=%u.%03ums(x%u.%02u)"
           " PER FRAME (xN = calls/frame) — GameEngine::Render runs INSIDE Loop, so DIP-SIM includes it",
           r_pf/1000u,     r_pf%1000u,     drn/dframes,  (drn  * 100u / dframes) % 100u,
           chore_pf/1000u, chore_pf%1000u, dcn/dframes,  (dcn  * 100u / dframes) % 100u,
           pbc_pf/1000u,   pbc_pf%1000u,   dpn/dframes,  (dpn  * 100u / dframes) % 100u,
           s_pf/1000u,     s_pf%1000u,     dsn/dframes,  (dsn  * 100u / dframes) % 100u,
           sc_pf/1000u,    sc_pf%1000u,    dcn2/dframes, (dcn2 * 100u / dframes) % 100u);
#endif
    /* Dedup effectiveness. If skipped/total is ~0 the dedup is not firing and the
     * assumption behind it is wrong -- which is exactly what went unnoticed for the
     * depth/cull dedup between 2026-07-20 and now. */
    /* ★ vitaGL's OWN DRAW-PATH PROFILER (HAVE_PROFILING build only).
     * These are plain global uint32_t in vitaGL's gxm.c, so they link directly --
     * no accessor needed. shaders_draw_profiler_cnt accumulates microseconds spent
     * INSIDE the shader draw path, which is the number that finally separates GL
     * submission from engine simulation. Declared weak so a normal (non-profiling)
     * libvitaGL.a still links: the symbols resolve to 0 and the line reports 0.
     *
     * COSTS WHAT IT MEASURES: vitaGL calls sceKernelGetProcessTimeLow twice per
     * draw, so at ~890 draws/frame this build carries ~1780 extra syscalls a frame.
     * Use it to find the answer, then ship without it. */
    { extern uint32_t shaders_draw_profiler_cnt __attribute__((weak));
      extern uint32_t shaders_draw_cnt __attribute__((weak));
      extern uint32_t frame_profiler_cnt __attribute__((weak));
      static uint32_t l_dp = 0, l_dc = 0;
      /* ☠ SAY SO WHEN THE SYMBOLS ARE NOT THERE -- same rule as the SIMSPLIT block
       * twenty lines up, which this failed to follow. The counters are weak, so on a
       * libvitaGL.a built WITHOUT HAVE_PROFILING (i.e. the one we ship) they resolve
       * to NULL and this printed
       *     VGLPROF: draw_submit=0.000ms/f draws/f=0 vgl_frame=0us
       * -- a well-formed measurement stating that draw submission is free, on a port
       * whose entire remaining problem is draw submission. Device log 2026-07-31
       * carried that line beside a scene doing hundreds of thousands of draws. */
      if (!(&shaders_draw_profiler_cnt) || !(&shaders_draw_cnt)) {
          l_info("VGLPROF: unavailable — this libvitaGL.a was not built with "
                 "HAVE_PROFILING, so there are no draw-submission counters to read "
                 "(rebuild vitaGL with it to attribute GL submission vs engine sim)");
      } else {
          const uint32_t dp = shaders_draw_profiler_cnt;
          const uint32_t dc = shaders_draw_cnt;
          const uint32_t fp = (&frame_profiler_cnt) ? frame_profiler_cnt : 0u;
          const uint32_t ddp = dp - l_dp, ddc = dc - l_dc;
          l_dp = dp; l_dc = dc;
          /* dframes is clamped to >=1 above, so no guard is needed here. */
          l_info("VGLPROF: draw_submit=%u.%03ums/f draws/f=%u vgl_frame=%uus "
                 "(vitaGL HAVE_PROFILING; compare against DIP-SIM total)",
                 (ddp / dframes) / 1000u, (ddp / dframes) % 1000u,
                 ddc / dframes, fp);
      } }
    /* Only the depth/cull dedup is left to report: the glUseProgram and glBindTexture
     * counters were deleted with their compares on 2026-07-30 after two device runs
     * measured them at ~1% and 0% respectively. */
    /* GLDEDUP is gone: all three redundant-call dedups were retired after their own
     * counters measured them dead on device (see the note above g_uniform_* in
     * glutil.c). Nothing left to report. */
    l_info("ENGPROF: shadow[0]=%.3fms shadow[1]=%.3fms idx=%u gc=%.3fms gcNum=%u "
           "rendercaps=0x%08X map_buffer(b21)=%u range(b22)=%u alt(b23)=%u",
           (double)(shadow_a * 1000.0f), (double)(shadow_b * 1000.0f), shadow_idx,
           (double)(gc_f * 1000.0f), gc_n,
           caps, (caps >> 21) & 1u, (caps >> 22) & 1u, (caps >> 23) & 1u);

    last_locks = locks; last_respec = respec; last_kb = kb;
    last_mallocs = mallocs; last_frames = frames;
#endif
}

/* ★ 2026-08-06 — MAKE IN-GAME SETTINGS PERSIST.
 *
 * Device evidence (25-minute session, r147): SavePrefs ran repeatedly and
 * `prefs.prop` appears NOWHERE in the log -- not one open, not one FAILED open,
 * in either direction. The engine's preferences are therefore never written and
 * never read back, so every in-game setting reverts on restart.
 *
 * Cause, from disassembly:
 *     luaSavePrefs            @0x00c41830 -> GameEngine::SavePrefs()
 *     GameEngine::SavePrefs   @0x00cb0dec builds String("prefs.prop") and saves
 *                                         the GetPreferences() property set to it
 * "prefs.prop" is a BARE name. Bare resource names do not bind to a writable
 * location on this port -- the same root cause the saveslot/choice.prop
 * redirects exist for -- so the save resolves to nothing and returns quietly.
 *
 * The existing redirect (redirect_logical_user_to_temp) already lists
 * "prefs.prop", but it can only rewrite names passed as arguments to hooked LUA
 * functions. luaSavePrefs takes no path: the string is built inside C++, so
 * nothing at the Lua layer ever sees it. There is likewise nothing at the file
 * layer to catch, because no open is ever attempted.
 *
 * So rewrite the name at its source. The function loads it as a PC-relative
 * literal:
 *     +0x10  e59f10ac  ldr r1, [pc, #172]     ; -> literal pool at +0xC4
 *     +0x18  e08f1001  add r1, pc, r1         ; r1 = (fn+0x20) + literal
 * Only the POOL WORD is rewritten, to point at our own
 * "logical:<Temp>/prefs.prop" instead. That is a single data store -- no code is
 * modified, nothing is rewritten on a hot path, and it happens once at init.
 * `logical:<Temp>/` is known to resolve and to be writable here: it is exactly
 * where the redirected saveslot bundles land in the same log.
 *
 * Both instruction encodings and the current string are verified before the
 * store, so a different engine build simply skips the patch. */
static void patch_saveprefs_path(void) {
    static int applied = 0;
    if (applied) return;
    applied = 1;

    /* Must stay resident for the life of the process: the engine re-reads this
     * literal on every SavePrefs call. */
    static const char k_prefs_path[] = "logical:<Temp>/prefs.prop";

    const uintptr_t fn = so_symbol(&so_mod_gameengine, "_ZN10GameEngine9SavePrefsEv");
    if (!fn) {
        l_warn("PREFSPATH: GameEngine::SavePrefs not found — in-game settings will not persist");
        return;
    }

    const uint32_t ldr_insn = *(volatile uint32_t *)(fn + 0x10);
    const uint32_t add_insn = *(volatile uint32_t *)(fn + 0x18);
    if (ldr_insn != 0xE59F10ACu || add_insn != 0xE08F1001u) {
        l_warn("PREFSPATH: unexpected SavePrefs prologue (ldr=0x%08X add=0x%08X) — "
               "not patching; in-game settings will not persist",
               (unsigned)ldr_insn, (unsigned)add_insn);
        return;
    }

    /* `add r1, pc, r1` reads PC as (its own address + 8) = fn + 0x20. */
    volatile uint32_t *pool = (volatile uint32_t *)(fn + 0xC4);
    const uintptr_t cur = (fn + 0x20) + (uintptr_t)*pool;
    if (!cur || strcmp((const char *)cur, "prefs.prop") != 0) {
        l_warn("PREFSPATH: literal is not \"prefs.prop\" — not patching; "
               "in-game settings will not persist");
        return;
    }

    const uint32_t want = (uint32_t)((uintptr_t)k_prefs_path - (fn + 0x20));
    kuKernelCpuUnrestrictedMemcpy((void *)pool, &want, sizeof(want));
    kuKernelFlushCaches((void *)pool, sizeof(want));

    const uintptr_t now = (fn + 0x20) + (uintptr_t)*pool;
    l_info("PREFSPATH: GameEngine::SavePrefs now writes '%s' (was 'prefs.prop') — "
           "in-game settings persist", (const char *)now);
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
    uintptr_t set_controller = so_symbol(&so_mod_gameengine,
        "_ZN14ChoreAgentInst13SetControllerE3PtrI18PlaybackControllerE");

    int ok_set_controller = 0;
    /* ★ REMOVED 2026-07-30 -- this patch was a PROVEN NO-OP, verified by
     * disassembly, and its comment claimed a fix it never delivered.
     *
     * It flipped `mov r1,#0` -> `mov r1,#1` inside UpdateChoreInstances, i.e.
     * forced the bool argument of ChoreInst::Update(bool) true for every chore.
     * Following that argument all the way down:
     *     ChoreInst::Update(bool)        r1 -> r5 -> r1, just forwards it
     *     ChoreAgentInst::Update(bool)   r1 -> r2, tail-calls SetCurrentTime
     *     ChoreAgentInst::SetCurrentTime(float, bool)
     *         r2 is NEVER READ. It is overwritten with #0 at +0x4c, +0x9c and
     *         +0xf0 before each virtual call, and appears as a source operand
     *         nowhere in the function.
     * So the flag was discarded. The patch changed no behaviour, fixed no
     * animation, and left a comment asserting otherwise -- which is worse than
     * doing nothing, because it makes the next reader believe the chore update
     * path has already been addressed. The symbol lookup that fed it is gone
     * too -- the hook install below resolves the same name independently. */

    if (set_controller) {
        ok_set_controller = patch_arm32_instruction(
            "ANIM ChoreAgentInst::SetController initial full update",
            set_controller + 0x178U,
            0xE1A01009U, /* mov r1,r9 (r9 is zero here) */
            0xE3A01001U  /* mov r1,#1 */);
    } else {
        l_warn("ANIM: ChoreAgentInst::SetController symbol not found.");
    }

#if MCSM_SIM_PROBES
    /* Opt-in: this hooks the CHORE SYSTEM, i.e. instrumentation sitting directly on
     * the animation path under investigation. Off unless graphics.txt asks for a
     * measurement, so the default build leaves the chore tick untouched. */
    if (mcsm_cfg()->sim_probes) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN9ChoreInst20UpdateChoreInstancesEv",
                                  "ChoreInst::UpdateChoreInstances (probe)",
                                  (uintptr_t)&hook_chore_update_all, &g_hook_chore_update_all);
    }
#endif
    /* Phase probes: attribute the sim time that chore+playback do NOT explain
     * (device says those are only ~4.8ms of 26-40ms). */
    /* Engine platform identity. Installed unconditionally so the hook is always in
     * place; the hook itself reads graphics.txt each call, which means platform_vita
     * can be flipped without a rebuild. Safe target: a 2-instruction constant-return
     * function, cold, no SO_CONTINUE needed. */
    /* ☠ ONLY HOOK THIS WHEN THE FEATURE IS ON. The engine function is a two-instruction
     * constant return, and this file's own header note records that its result feeds
     * T3EffectCacheInternal::GetProgram -- i.e. it is consulted PER DRAW, ~890 times a
     * frame. Replacing 2 cycles with a patched branch, a counter, a cross-TU
     * mcsm_cfg() call and a compare cost roughly 50us/frame to return the value the
     * engine already returned, because platform_vita defaults to 0.
     * The old justification -- "the hook reads graphics.txt each call, so it can be
     * flipped without a rebuild" -- was also wrong: mcsm_cfg() latches on first use,
     * so changing it already needs a restart. Deciding once, here, costs nothing. */
    if (mcsm_cfg()->platform_vita) {
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_ZN10TTPlatform15GetPlatformTypeEv",
                                  "TTPlatform::GetPlatformType",
                                  (uintptr_t)&hook_get_platform_type, &g_hook_get_platform_type);
    }
    /* Read (and optionally override) the engine render/audio globals. Pure data
     * access -- no hooking -- so it is safe here and the ENGTUNE line records the
     * LIVE values, which a binary read alone cannot prove. */
    mcsm_engine_tunables();
    /* Discovery only: logs the achievement name the game unlocks, changes nothing. */
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN16Platform_Android17UnlockAchievementERK6String",
                              "Platform_Android::UnlockAchievement (achievement probe)",
                              (uintptr_t)&hook_unlock_achievement, &g_hook_unlock_achievement);
#if MCSM_SIM_PROBES
    /* ☠ These two exist ONLY to feed MCSM_PHASE_ADD, which compiles to nothing in the
     * RELEASE build -- so installing them there bought a pair of empty SO_CONTINUE
     * hooks on two once-per-frame engine functions. SO_CONTINUE_VOID is 2 kubridge
     * memcpys + 2 cache flushes per call, so that was 8 kernel calls a frame for a
     * measurement the log itself reports as "phase timers COMPILED OUT" -- and it put
     * live-code rewriting on two more functions, which is the mechanism this file
     * documents as the cause of the diorama crash. Install them only when they can
     * actually measure something. */
    if (mcsm_cfg()->sim_probes) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN5Scene12UpdateScenesEv",
                                  "Scene::UpdateScenes (probe)",
                                  (uintptr_t)&hook_scene_update, &g_hook_scene_update);
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13ScriptManager6UpdateEf",
                                  "ScriptManager::Update (probe)",
                                  (uintptr_t)&hook_script_update, &g_hook_script_update);
        l_info("SIMSPLIT: sim probes ENABLED (graphics.txt sim_probes=on)");
    }
#endif

    /* Only set_controller is still a patch. update_all was removed as a proven
     * no-op (see above); reporting it as `update_all=0` forever read as a failing
     * patch on every boot, in a log that is the primary debugging surface. */
    l_info("ANIM: full chore update patch set_controller=%d", ok_set_controller);
}

static void hook_gameengine_start(void) {
    force_animation_runtime_flags("start-pre");
    SO_CONTINUE_VOID(g_hook_gameengine_start);
    force_animation_runtime_flags("start-post");
}

static void hook_metrics_new_frame(uint32_t min_frame_time_bits) {
    MCSM_DIAG_COUNTER(count);

    typedef void (*metrics_new_frame_raw_fn)(uint32_t);
    if (g_metrics_new_frame_tramp) {
        ((metrics_new_frame_raw_fn)g_metrics_new_frame_tramp)(min_frame_time_bits);
    } else {
        /* Exact-prologue validation failed: retain the old, slower correctness path. */
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
    }

    /* Runs AFTER the real NewFrame has written mFrameTime/mActualFrameTime and
     * BEFORE the rest of the loop consumes them -- the only point where the 0.1s
     * clamp can still be repaired for the frame it applies to. On frames the
     * engine timed correctly this is a no-op (see metrics_force_animation_dt). */
    const float engine_ft = g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : 0.0f;
    const float engine_aft = g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : 0.0f;
    const float fixed_dt = animation_governor_update(engine_ft, engine_aft, "newframe", count);
    metrics_force_animation_dt(fixed_dt, "newframe", count);

    metrics_diag_tick(count, "newframe");
}

static void hook_gameengine_loop(void) {
    static uint32_t count = 0;

    count++;
    /* STREAMING: advance the async preload batch while a ScenePreload is in
     * flight. Bounded per frame and by a hard deadline -- see stream_pump_preload. */
    stream_pump_preload();
    if ((count & 0x0fU) == 1U) {
        force_animation_runtime_flags("loop");
    }

    force_application_active("loop-pre");
    force_native_render_dimensions("loop-pre");
    metrics_diag_tick(count, "pre");
    /* DIP PROFILER: time the actual engine sim work (excludes our frame pace).
     * If the sim loop itself spikes (>22ms) the bottleneck is engine-side
     * logic/animation, not rendering. Keep this to severe spikes only; logging
     * every 20-50ms frame became part of the stutter during animation tests. */
#if defined(DEBUG_SOLOADER) || !MCSM_FAST_FINAL_RUNTIME
    const int measure_sim = 1;
#else
    /* Fixed-clock profiles never consume sim_us. Cache the latched config decision
     * and avoid two kernel-time queries per frame; adaptive/battery keeps the exact
     * same governor input. */
    static int measure_sim = -1;
    if (measure_sim < 0) measure_sim = mcsm_cfg()->clock_adaptive ? 1 : 0;
#endif
    const uint64_t sim_t0 = measure_sim ? sceKernelGetSystemTimeWide() : 0;
    SO_CONTINUE_VOID(g_hook_gameengine_loop);
    const uint32_t sim_us = measure_sim
                          ? (uint32_t)(sceKernelGetSystemTimeWide() - sim_t0)
                          : 0u;
    /* Feed the pure sim-work cost to the adaptive ARM-clock governor (battery):
     * downclock when scenes are light, jump to the ceiling the moment they aren't. */
    mcsm_clock_governor_tick(sim_us);
    /* DIAG: log gameplay CPU-sim cost on dips (>20ms), throttled. Compare against
     * DIP-RENDER dt for the same frames: sim≈dt => CPU-bound; sim<<dt => GPU/VRAM-bound. */
#ifdef DEBUG_SOLOADER
    { const uint32_t sim_ms = sim_us / 1000U;
      static unsigned s_simc = 0;
      if (sim_ms > 20U && (s_simc++ & 0xFU) == 0U) l_info("DIP-SIM frame=%u work=%ums", count, sim_ms); }
#endif
    force_application_active("loop-post");
    force_native_render_dimensions("loop-post");
    metrics_diag_tick(count, "post");
    /* Only when the repair is explicitly enabled. With anim_dt_repair = 0 the
     * loader must not write the engine's timing state from here either -- leaving
     * this call unguarded would have kept rewriting mFrameTime every frame while
     * the log claimed NewFrame was left native. */
    if (mcsm_cfg()->anim_dt_repair) {
        metrics_force_animation_dt(
            animation_governor_current_or_repair(
                g_metrics_diag.frame_time ? *g_metrics_diag.frame_time : 0.0f,
                g_metrics_diag.actual_frame_time ? *g_metrics_diag.actual_frame_time : 0.0f),
            "loop-post",
            count);
    }
    /* The render presenter is the only frame-rate gate. Sleeping the simulation
     * here as well stacked two independent caps; a small vblank miss at 30 FPS
     * became a 3-vblank/20 FPS frame and starved animation preparation. */
}

static void hook_gameengine_render(void) {
    force_native_render_dimensions("render-enter");
    const uint64_t r_t0 = MCSM_PHASE_T0();
    SO_CONTINUE_VOID(g_hook_gameengine_render);
    MCSM_PHASE_ADD(g_render_us, g_render_n, r_t0);
    force_native_render_dimensions("render-exit");
}

static int hook_renderdevice_begin_frame(void) {
    force_native_render_dimensions("begin-frame-pre");
    int ret = SO_CONTINUE(int, g_hook_render_begin_frame);
    force_native_render_dimensions("begin-frame-post");
    return ret;
}

#ifdef DEBUG_SOLOADER
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
#endif

static void hook_android_jni_poll_input_devices(void) {
    launch_state_mark_poll();
    mcsm_register_virtual_controller();
}

static void hook_android_pump_events(void) {
    launch_state_mark_poll();
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

static int hook_renderframe_execute(void *self, void *other_frame) {
    int ret;
    if (g_renderframe_execute_tramp) {
        ret = ((int (*)(void *, void *))g_renderframe_execute_tramp)(self, other_frame);
    } else {
        ret = SO_CONTINUE(int, g_hook_renderframe_execute, self, other_frame);
    }
    /* Bootstrap needs forced presents to escape the historical black-screen
     * path. After the first real scene render, leave the frame pairing exactly
     * as the engine requested; replacing the paired overlay frame with NULL
     * produced live RenderScene calls but unstable color-only output. */
    int forced = (!ret && !g_boot_scene_active) ? 1 : ret;
    return forced;
}

static void hook_gamerender_render_frame(void) {
    force_native_render_dimensions("game-render-frame");
    if (g_gamerender_render_frame_tramp) {
        ((void (*)(void))g_gamerender_render_frame_tramp)();
    } else {
        SO_CONTINUE_VOID(g_hook_gamerender_render_frame);
    }
}

static void hook_gamerender_render_scene(void *scene_ctx, const void *params) {
    if (!g_boot_scene_active) {
        /* First real scene render = assets loaded, the game is starting. */
        g_boot_scene_active = 1;
        launch_state_mark_scene_active();
    }
    force_native_render_dimensions("game-render-scene");
    if (g_gamerender_render_scene_tramp) {
        ((void (*)(void *, const void *))g_gamerender_render_scene_tramp)(scene_ctx, params);
    } else {
        SO_CONTINUE_VOID(g_hook_gamerender_render_scene, scene_ctx, params);
    }
}

static int playback_dt_is_usable(float value) {
    return float_bits_finite(value) && value > 0.0001f && value < 1.0f;
}

static void hook_playback_controller_update(uint32_t frame_time_bits,
                                            uint32_t actual_frame_time_bits) {
    /* Explicit animation-rate control: on non-Nth frames, accumulate
     * this frame's dt and SKIP the whole update (controllers don't advance); on the
     * Nth frame, run the update with the accumulated dt so wall-clock speed stays
     * correct. It is never applied during boot. There is deliberately no hidden
     * engage/release governor: `anim_rate = 2|3` means exactly what the user chose,
     * while the default `1` leaves every animation update intact. */
    {
        const int rate = mcsm_anim_rate();
        static uint32_t phase = 0;
        static float acc_ft = 0.0f, acc_aft = 0.0f;
        if (rate > 1 && g_boot_scene_active) {
            acc_ft  += float_from_bits(frame_time_bits);
            acc_aft += float_from_bits(actual_frame_time_bits);
            if ((++phase % (uint32_t)rate) != 0u) {
                return;   /* skip: hook stays armed, controllers hold this frame */
            }
            frame_time_bits        = float_bits(acc_ft);
            actual_frame_time_bits = float_bits(acc_aft);
            acc_ft = 0.0f; acc_aft = 0.0f;
        } else {
            phase = 0;
            acc_ft = 0.0f;
            acc_aft = 0.0f;
        }
    }

    MCSM_DIAG_COUNTER(count);

    uint32_t out_frame_bits = frame_time_bits;
    uint32_t out_actual_bits = actual_frame_time_bits;

    /* Do not rewrite either channel. Shipped engine code selects between the
     * safe frame delta and actual wall-clock delta per controller. Keeping that
     * distinction is what lets facial/audio controllers catch up after a hitch
     * without flinging ordinary body animation forward. The optional anim_rate
     * block above has already accumulated the original pair when explicitly used. */

    /* libGameEngine was built softfp, so these float arguments arrive in r0/r1.
     * Keep both hook entry and original call in integer registers and only
     * reinterpret the bits locally for validation. */
    typedef void (*playback_update_raw_fn)(uint32_t, uint32_t);
    const uint64_t t0_pbc = MCSM_PHASE_T0();
    if (g_playback_controller_update_tramp) {
        ((playback_update_raw_fn)g_playback_controller_update_tramp)(out_frame_bits,
                                                                     out_actual_bits);
    } else {
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
    MCSM_PHASE_ADD(g_pbc_us, g_pbc_n, t0_pbc);
}

__attribute__((unused))
static void *hook_renderframe_allocate_view(void *self, const void *params) {
    static uint32_t count = 0;

    count++;

    if (!self || !params) {
        return NULL;
    }

    void *view = SO_CONTINUE(void *, g_hook_renderframe_allocate_view, self, params);
    if (!view) {
        return NULL;
    }

    /* pass_type/target_ref/log_this were three pointer derefs computed only to
     * feed a diagnostic string nobody read; removed with it. */


    return view;
}

/* Compiled-in but unreferenced while ENABLE_HOT_RENDER_VIEW_DIAG_HOOKS is 0.
 * Kept because these are the per-view diagnostics, and deliberately NOT installed:
 * AllocateView/PushView run per view per frame on multiple render threads, which is
 * the exact profile that made the SO_CONTINUE hooks crash. */
__attribute__((unused))
static void *hook_renderframe_push_view(void *self, void *frame_scene, const void *params) {
    static uint32_t count = 0;
    static uint32_t overlay_track_logs = 0;

    count++;

    void *caller = __builtin_return_address(0);
    if (!self || !frame_scene || !params) {
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
    return view;
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
#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

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

/* ★ PER-CALL OVERHEAD IS THE COST, NOT BANDWIDTH (device-measured 2026-07-30).
 *
 * SKINPROBE on a heavy scene, per frame:
 *     locks/f 525.7   respec/f 262.8   KB/f 231.6   malloc/f 262.8
 * so 263 full glBufferData respecifies per frame, each paired with a malloc and a
 * free -- and the average payload is 231.6KB/263 = 902 BYTES. The earlier estimate
 * assumed a handful of large buffers and therefore predicted ~0.46ms of memcpy.
 * Wrong shape entirely: at 902 bytes the copy is free and the cost is 263 x
 * (malloc + free + vitaGL GPU-alloc + GPU-free), i.e. 1.3-5.3ms depending on
 * allocator cost. It is also what makes free VRAM collapse 80912KB -> 75KB, because
 * every respecify pushes the old allocation onto vitaGL's deferred-free list.
 *
 * Two side tables remove that overhead without changing a single byte the engine
 * sees:
 *   VB_CPU   T3VertexBuffer* -> a CPU staging buffer that PERSISTS across frames.
 *            [self+0xd0] is still set on lock and cleared on unlock, so the engine
 *            never observes our pointer outside the lock window -- which is what
 *            makes this safe even though the engine frees that field with
 *            operator delete[] on its own path.
 *   VB_SIZE  GL buffer name -> the size currently allocated on the GPU, so unlock
 *            can use glBufferSubData (verified in the SHIPPED libvitaGL.a to be a
 *            bare sceClibMemcpy/sceDmacMemcpy with NO allocation, because
 *            BUFFERS_SPEEDHACK is on) whenever the size has not changed.
 *
 * THREADING: SKINPROBE reports a single, unchanging tid (0x400103D5) for every lock
 * in a session, so this path is single-threaded in practice. Rather than assume it,
 * the tables record the owning thread and any call from a different one falls back
 * to the original malloc/glBufferData path, which is always correct. */
#define VB_SLOTS 1024u
typedef struct { void *key; void *cpu; uint32_t cap; } VbCpuSlot;
static VbCpuSlot g_vb_cpu[VB_SLOTS];
typedef struct { unsigned int name; uint32_t size; } VbSizeSlot;
static VbSizeSlot g_vb_size[VB_SLOTS];
static SceUID g_vb_owner_tid = -1;
#ifdef DEBUG_SOLOADER
static uint32_t g_vb_foreign = 0;
#endif
/* Consecutive locks by a thread that is NOT the current table owner. Reset to 0 the
 * moment the owner locks again, so this only climbs while the owner is truly idle. */
static uint32_t g_vb_steal_run = 0;

/* ★ REWRITTEN AFTER THE DEVICE PROVED THE FIRST VERSION INERT (2026-07-30).
 *
 * v1 was open-addressed, linear-probed and NEVER DELETED, on the assumption that a
 * few hundred live buffers would keep the load factor low. Device says otherwise:
 * 709 355 locks in one session and SKINPROBE reported `subdata=0 reuse=0` -- the
 * tables saturated almost immediately and every lookup fell back to the old path,
 * so the whole optimisation executed zero times while still compiling cleanly and
 * reporting "0 warnings". Compiling is not running.
 *
 * Fix: bounded probe with REPLACEMENT. A colliding key evicts the resident entry
 * (freeing its buffer) instead of walking the table and eventually failing. That
 * makes the tables self-renewing and O(1) worst case, so a hit rate below 100%
 * costs one realloc rather than disabling the feature. Slots raised to 1024. */
#define VB_PROBE 4u

/* ☠ SCAN THE WHOLE WINDOW FOR A MATCH BEFORE INSERTING. Both tables used to insert
 * at the FIRST FREE slot the moment they saw one, without checking whether the key
 * already lived further along the probe window -- and entries are deleted (evicted,
 * or cleared by mcsm_vb_forget_gl_buffer), so a free slot BEFORE an occupied one is
 * a normal state, not an impossible one.
 *
 * That produced TWO live entries for one key, and the two lookups then disagreed:
 *   - g_vb_size: the delete-forget guard cleared the first copy and left the second
 *     holding the OLD byte count. GL recycles buffer names, so the next buffer to
 *     get that name could match the stale entry, see `ss->size == size` on its very
 *     first upload, and take the glBufferSubData branch into storage vitaGL never
 *     allocated at that size -- exactly the memory-unsafe case the forget guard
 *     exists to prevent, reintroduced behind it.
 *   - g_vb_cpu: vb_cpu_slot (lock) and vb_cpu_find (unlock) could land on different
 *     copies, so unlock would see `slot->cpu != ptr`, conclude the staging buffer was
 *     a plain malloc, and free a pointer the table still hands out.
 *
 * Find first, then insert or evict. */
static VbCpuSlot *vb_cpu_slot(void *key) {
    uint32_t h = (uint32_t)(uintptr_t)key;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    VbCpuSlot *freeslot = NULL, *victim = NULL;
    for (uint32_t i = 0; i < VB_PROBE; ++i) {
        VbCpuSlot *s = &g_vb_cpu[(h + i) & (VB_SLOTS - 1u)];
        if (s->key == key) return s;
        if (!s->key) { if (!freeslot) freeslot = s; }
        else if (!victim) victim = s;
    }
    if (freeslot) { freeslot->key = key; freeslot->cpu = NULL; freeslot->cap = 0; return freeslot; }
    /* Evict. Freeing here is safe: the slot is not the live lock (that one matched
     * above), and unlock only frees a pointer the table does not own. */
    if (victim->cpu) { free_soloader(victim->cpu); MCSM_VB_DIAG(g_vb_evict++); }
    victim->key = key; victim->cpu = NULL; victim->cap = 0;
    return victim;
}
/* ☠ MUST be called when a GL buffer name is deleted. The size table is keyed on the
 * GL NAME and its entries used to live for the whole process, but GL RECYCLES deleted
 * names: the engine streams a mesh out with glDeleteBuffers and a later glGenBuffers
 * hands the same name to an unrelated buffer. The stale entry then carries the OLD
 * size, and if the new buffer's first upload happens to be that same byte count,
 * upload_cpu_buffer sees `ss->size == size` on its VERY FIRST upload and takes the
 * glBufferSubData branch -- writing into storage vitaGL never allocated at that size.
 * Forgetting the name forces that first upload back through the allocating path.
 * (g_vb_cpu is NOT touched here: it is keyed on the T3VertexBuffer object, not the GL
 * name, so a deleted GL buffer says nothing about it.) */
void mcsm_vb_forget_gl_buffer(unsigned int name) {
    if (!name) return;
    uint32_t h = name; h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    /* Clears EVERY match in the window, not just the first. vb_size_slot can no
     * longer create a duplicate, but this is the guard that stands between a
     * recycled GL name and an unallocated glBufferSubData -- it should not depend on
     * the insert path staying correct. */
    for (uint32_t i = 0; i < VB_PROBE; ++i) {   /* mirror vb_size_slot's hash+probe */
        VbSizeSlot *s = &g_vb_size[(h + i) & (VB_SLOTS - 1u)];
        if (s->name == name) { s->name = 0; s->size = 0; }
    }
}

/* PURE lookup: never inserts, never evicts, never frees. Unlock needs to answer
 * "does the table own this pointer?" and must not perturb the table to do it.
 * Scans the FULL window rather than stopping at the first free slot, so it agrees
 * with vb_cpu_slot by construction instead of by relying on g_vb_cpu never clearing
 * an entry -- the two disagreeing is what frees a live staging buffer. */
static VbCpuSlot *vb_cpu_find(void *key) {
    uint32_t h = (uint32_t)(uintptr_t)key;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    for (uint32_t i = 0; i < VB_PROBE; ++i) {
        VbCpuSlot *s = &g_vb_cpu[(h + i) & (VB_SLOTS - 1u)];
        if (s->key == key) return s;
    }
    return NULL;
}

/* Find-then-insert, for the reason spelled out above vb_cpu_slot: entries here ARE
 * cleared (mcsm_vb_forget_gl_buffer), so a free slot ahead of an occupied one is
 * routine, and inserting on sight of it would leave a second entry for the same GL
 * name carrying a stale byte count. */
static VbSizeSlot *vb_size_slot(unsigned int name) {
    uint32_t h = name; h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    VbSizeSlot *freeslot = NULL, *victim = NULL;
    for (uint32_t i = 0; i < VB_PROBE; ++i) {
        VbSizeSlot *s = &g_vb_size[(h + i) & (VB_SLOTS - 1u)];
        if (s->name == name) return s;
        if (!s->name) { if (!freeslot) freeslot = s; }
        else if (!victim) victim = s;
    }
    if (freeslot) { freeslot->name = name; freeslot->size = 0; return freeslot; }
    victim->name = name; victim->size = 0;   /* forces one full respecify, then hits */
    return victim;
}
/* Are we the thread that owns the staging tables?
 *
 * ☠ "FIRST CALLER CLAIMS THEM" WAS WRONG, AND IT COST 256 MALLOCS PER FRAME.
 * Device evidence (r82, heavy scene): SKINPROBE reported malloc/f=256 and reuse=0 --
 * i.e. the reuse path this gate protects NEVER ran -- alongside 16204 warnings, all
 * identical: `lock from foreign tid 0x400103D5 (owner 0x400101F5)`. Exactly ONE
 * foreign tid in the whole session. The first PlatformLock of a run happens on an
 * init-time thread, which claimed the tables permanently; every REAL lock then came
 * from the render thread and was rejected, falling through to a raw malloc/free pair
 * per skinned mesh per frame.
 *
 * Fix: ownership is now TRANSFERABLE. A thread that is consistently doing the locking
 * takes the tables over from an owner that has gone quiet. The counter resets whenever
 * the incumbent owner locks, so a genuinely contended pattern (two threads alternating)
 * never reaches the threshold and both keep using the safe malloc path -- the tables
 * are still only ever touched by one thread at a time, which is what makes them safe
 * without a lock. Only a persistently absent owner is displaced. */
#define VB_STEAL_AFTER 64u   /* consecutive foreign locks before ownership moves */

/* ☠ CALL vb_thread_claim() EXACTLY ONCE PER LOCK. It is the side-effecting half --
 * it counts foreign locks and can transfer ownership -- so calling it again later in
 * the same lock/unlock cycle inflates both the counter and the steal rate. It used to
 * be called three times per vertex buffer (lock, upload, unlock), which made
 * VB_STEAL_AFTER mean 21 locks rather than 64, tripled the reported `foreign=` count,
 * and, worse, let ownership move BETWEEN a lock and its unlock: the unlocking thread
 * would then see itself as a non-owner, conclude the staging buffer was its own
 * malloc, and free a pointer the table still holds -- a dangling table entry handed
 * straight to the next lock of that buffer.
 * Everywhere else asks vb_thread_is_owner(), or (at unlock) does not ask at all and
 * uses vb_cpu_find() to answer the only question that actually matters: is this
 * pointer ours to free? */
/* ☠ THIS DOES THE SYSCALL, AND IT HAS TO. A cached "am I the owner" flag was tried
 * on 2026-07-31 to save ~525 sceKernelGetThreadId() calls a frame, and it was WRONG
 * in two ways that only a release review caught:
 *   - vb_thread_claim() runs inside PlatformLock's `if (!ptr)` branch, so a re-lock
 *     (ptr already set) never refreshes the flag -- the upload then consumed a verdict
 *     from an EARLIER lock cycle;
 *   - the T3IndexBuffer path never calls vb_thread_claim() at all, yet its unlock goes
 *     through upload_cpu_buffer() and read the flag anyway -- so index uploads used
 *     whatever the last VERTEX lock happened to leave behind.
 * The flag gates use of g_vb_size, which drives the no-allocation glBufferSubData
 * path. A wrongly-affirmative answer from a foreign thread is precisely the race this
 * gate exists to prevent, and that path is memory-unsafe when the bookkeeping is
 * wrong. A kernel round-trip per upload is the price of the guarantee.
 *
 * The saving that DID survive is the third call site: unlock no longer asks "am I the
 * owner" at all, because vb_cpu_find() answers the only question it actually has --
 * is this pointer ours to free -- which is a property of the pointer, not the caller. */
static int vb_thread_is_owner(void) {
    return g_vb_owner_tid == sceKernelGetThreadId();
}

static int vb_thread_claim(void) {
    SceUID me = sceKernelGetThreadId();
    if (g_vb_owner_tid == -1) { g_vb_owner_tid = me; return 1; }
    if (g_vb_owner_tid == me) { g_vb_steal_run = 0; return 1; }

#ifdef DEBUG_SOLOADER
    g_vb_foreign++;
#endif
    if (++g_vb_steal_run >= VB_STEAL_AFTER) {
        /* The incumbent has not locked once in VB_STEAL_AFTER consecutive locks.
         * Take the tables and LEAVE THE EXISTING ENTRIES ALONE.
         *
         * ☠ An earlier version freed every cached buffer here "so the new owner
         * cannot hand out a stale one". That was a use-after-free waiting to happen:
         * these pointers are given to the engine by writing them into the
         * T3VertexBuffer at u32[0xd0/4], and by design the slot RETAINS the pointer
         * across unlock (unlock only clears the engine's copy). If the previous owner
         * were between its PlatformLock and PlatformUnlock at the moment of transfer,
         * its live staging buffer would be freed underneath it and the engine would
         * skin vertices into freed memory. vb_thread_ok() is deliberately lock-free,
         * so nothing orders those two threads.
         * Keeping the entries is safe AND correct: slots are keyed on the buffer's
         * own `self` pointer so the new owner can only reach an entry by presenting
         * the same object, and the existing `slot->cap < bytes` growth check already
         * handles an entry sized for different traffic. */
        l_info("VBOPT: staging tables move 0x%08X -> 0x%08X after %u consecutive "
               "foreign locks (was costing a malloc per lock)",
               (unsigned)g_vb_owner_tid, (unsigned)me, VB_STEAL_AFTER);
        g_vb_owner_tid = me;
        g_vb_steal_run = 0;
        return 1;
    }
#ifdef DEBUG_SOLOADER
    if ((g_vb_foreign & 0xFFu) == 0u)
        l_warn("VBOPT: lock from foreign tid 0x%08X (owner 0x%08X) — using the "
               "malloc path for it (#%u)", (unsigned)me, (unsigned)g_vb_owner_tid, g_vb_foreign);
#endif
    return 0;
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

#ifdef DEBUG_SOLOADER
    static uint32_t s_upload_count = 0;
    s_upload_count++;
    g_vb_kb += (uint32_t)(size / 1024u);
    if ((uint32_t)size > g_vb_max_bytes) g_vb_max_bytes = (uint32_t)size;
    /* Sample the owning thread on the FIRST upload as well as every 256th. The
     * `& 0xFF` test alone never fired below 256 uploads, and a menu produces about
     * 16 -- so SKINPROBE reported `tid=0x00000000` for the whole session, which
     * reads as "no thread" or "the probe is broken" rather than "not sampled yet".
     * Device log 2026-07-31 showed exactly that next to a perfectly healthy run. */
    if (s_upload_count == 1u || (s_upload_count & 0xFFu) == 0u)
        g_vb_tid = (uint32_t)sceKernelGetThreadId();
#endif

    /* ★ glBufferSubData WHEN THE SIZE HAS NOT CHANGED (2026-07-30).
     *
     * RenderDevice::AllocateGLBuffer ends in glBufferData, which in vitaGL frees the
     * old GPU allocation (onto a deferred dirty list), takes a NEW one, and copies.
     * Doing that 263 times a frame for buffers averaging 902 bytes is where the time
     * went, and the deferred frees are why free VRAM fell 80912KB -> 75KB.
     *
     * glBufferSubData needs no allocation at all. Verified in the SHIPPED
     * libvitaGL.a (not the source -- that mistake already cost one crash):
     *     glNamedBufferSubData:
     *       ldr r0,[r0,#0] ; add r0,r1 ; <size>=8192 && addr>=0x81000000 ?
     *       b sceDmacMemcpy : b sceClibMemcpy
     * i.e. a bare memcpy with a DMA fast path, because BUFFERS_SPEEDHACK is on.
     * It also has no NULL/bounds check (NO_DEBUG), which is exactly why this is
     * gated on OUR OWN record of the size we last allocated for this buffer name --
     * never on an assumption about vitaGL's state. First sight of a buffer, or any
     * size change, still goes through the full allocating path. */
    /* ★ INDEX BUFFERS BELONG HERE TOO (2026-07-30). This used to test
     * `target == GL_ARRAY_BUFFER`, which let only HALF the traffic take the
     * no-allocation path. Device (r82 heavy scene): locks/f=512 against respec/f=256
     * -- the engine locks a vertex AND an index buffer per skinned mesh, ~256 of each
     * per frame. The vertex half reached glBufferSubData; the ~256
     * GL_ELEMENT_ARRAY_BUFFER uploads fell through to the allocating path below,
     * which inside vitaGL is a GPU free + realloc + full copy EVERY FRAME. Index data
     * for a skinned mesh is static -- only the vertex positions change -- so those
     * respecifies were near-pure waste.
     * Safe because the table is keyed on the GL buffer NAME, which is unique across
     * targets (glGenBuffers hands out one namespace), so vertex and index buffers
     * cannot collide in vb_size_slot. The size-change and first-sight paths are
     * unchanged: anything new or resized still goes through the full allocation. */
    VbSizeSlot *ss = ((target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER)
                      && vb_thread_is_owner()) ? vb_size_slot(buffer) : NULL;
    if (ss && ss->size == (uint32_t)size && size > 0) {
        glBindBuffer(target, buffer);
        glBufferSubData(target, 0, (GLsizeiptr)size, data);
        MCSM_VB_DIAG(g_vb_subdata++);
        return 1;
    }

    MCSM_VB_DIAG(g_vb_respec++);
#ifdef DEBUG_SOLOADER
    GLenum pre_err = patch_drain_gl_errors();
#else
    (void)patch_drain_gl_errors();
#endif
    int ret = alloc(buffer, target, (unsigned int)size, data, GL_STREAM_DRAW);
    GLenum err = glGetError();
    /* ☠ RECORD THE SIZE ONLY IF THE ALLOCATION ACTUALLY HAPPENED, and that means
     * BOTH signals. This used to check the GL error alone and ignore the engine
     * allocator's own return value, which it merely logged. If AllocateGLBuffer failed
     * WITHOUT leaving a GL error -- an early-out on its own bookkeeping, say -- we
     * recorded a size for storage that was never created, and the next upload of the
     * same size would sail into the glBufferSubData fast path and write into a buffer
     * GL never allocated at that size. That path is memory-unsafe by construction
     * (vitaGL is built NO_DEBUG, so glNamedBufferSubData is a bare memcpy with no
     * bounds check); the whole point of this record is that it is only ever set from
     * an allocation we watched succeed. Being wrong in the conservative direction just
     * costs one extra respecify. */
    if (ss && ret && err == GL_NO_ERROR) ss->size = (uint32_t)size;
#ifdef DEBUG_SOLOADER
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
#endif
    return ret && err == GL_NO_ERROR;
}

static int hook_vertexbuffer_platform_lock(void *self, int read_only) {
    (void)read_only;
    if (!self) {
        return 0;
    }

    MCSM_VB_DIAG(g_vb_locks++);
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

        /* REUSE the staging buffer across frames instead of malloc/free per lock.
         * Device data: 263 of these per frame at an average of 902 bytes, so the
         * allocator traffic -- not the copy -- was the cost. Grow-only; the slot
         * keeps the pointer after unlock clears [self+0xd0], so the engine never
         * sees a pointer it might free with the wrong allocator. */
        VbCpuSlot *slot = vb_thread_claim() ? vb_cpu_slot(self) : NULL;
        if (slot) {
            if (slot->cap < bytes) {
                void *grown = slot->cpu ? realloc_soloader(slot->cpu, bytes)
                                        : malloc_soloader(bytes);
                if (!grown) {
                    l_error("Patch: T3VertexBuffer::PlatformLock staging grow to %u FAILED.", (unsigned)bytes);
                    return 0;
                }
                slot->cpu = grown; slot->cap = (uint32_t)bytes;
                MCSM_VB_DIAG(g_vb_mallocs++); /* counts only real allocations now */
            } else {
                MCSM_VB_DIAG(g_vb_reuse++);
            }
            ptr = slot->cpu;
        } else {
            ptr = malloc_soloader(bytes);
            if (!ptr) {
                l_error("Patch: T3VertexBuffer::PlatformLock fallback malloc failed (%u bytes).", (unsigned)bytes);
                return 0;
            }
            MCSM_VB_DIAG(g_vb_mallocs++);
        }
        u32[0xd0 / 4] = (uint32_t)(uintptr_t)ptr;
        /* Throttled: this fired 7420x/run (the #1 log-spam line) — synchronous
         * file writes that pile up during the already-slow scene loads. */
#ifdef DEBUG_SOLOADER
        static unsigned int s_vb_log = 0;
        if (s_vb_log++ < 16u)
            l_info("Patch: T3VertexBuffer::PlatformLock allocated CPU vertex buffer (%u bytes).", (unsigned)bytes);
#endif
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
            /* Do not leave a phantom nested lock: the next call must retry the
             * allocation instead of succeeding with a NULL staging pointer. */
            u32[0x24 / 4] = 0;
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

    void *ptr = (void *)(uintptr_t)u32[0xd0 / 4];
    const size_t bytes = (size_t)u32[0xc0 / 4] * (size_t)u32[0xc4 / 4];
    /* upload_cpu_buffer binds for glBufferSubData; the engine allocator binds for
     * glBufferData. Binding here as well duplicated the same driver call on every
     * upload (~525/frame in the measured scene). */
    const int ok = upload_cpu_buffer(u32[0x20 / 4], GL_ARRAY_BUFFER, bytes, ptr, "T3VertexBuffer");

    /* Only free when the allocation is NOT owned by the reuse table. Asked as a plain
     * table lookup rather than "am I the owner thread?": ownership can move between a
     * lock and its unlock, and a thread that had just lost it would otherwise free the
     * table's live staging buffer and leave the entry dangling. Whether the pointer is
     * ours to free is a property of the pointer, not of the caller. */
    if (ptr) {
        VbCpuSlot *slot = vb_cpu_find(self);
        if (!slot || slot->cpu != ptr) free_soloader(ptr);
    }

    u32[0xd0 / 4] = 0;
    u32[0xe0 / 4] = 0;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return ok ? 1 : 0;
}

static int hook_indexbuffer_platform_unlock(void *self) {
    if (!self) {
        return 0;
    }

    uint32_t *u32 = (uint32_t *)self;

    /* ☠ DECREMENT BEFORE THE GL-NAME BAIL. This used to `return 0` on a zero GL
     * buffer name WITHOUT touching the lock counter, while PlatformLock increments it
     * unconditionally (it only inspects the element count). One unlock down that path
     * therefore left the counter permanently above zero, and PlatformLock's
     * `if (lock_count > 1) return 1;` fast path then short-circuited every subsequent
     * lock -- so that index buffer would never allocate its CPU staging buffer and
     * never upload again, for the rest of the session. The vertex unlock next door
     * decrements on every path, which is the correct shape. */
    uint32_t lock_count = u32[0x24 / 4];
    if (lock_count > 0) {
        lock_count -= 1;
        u32[0x24 / 4] = lock_count;
    }

    if (lock_count > 0 || u32[0x20 / 4] == 0) {
        return 0;
    }

    const void *ptr = (const void *)(uintptr_t)u32[0x3c / 4];
    const size_t bytes = (size_t)u32[0x2c / 4] * (size_t)u32[0x30 / 4];
    const int ok = upload_cpu_buffer(u32[0x20 / 4], GL_ELEMENT_ARRAY_BUFFER, bytes, ptr, "T3IndexBuffer");
    /* Leave the element-array binding as we found it, exactly as the vertex unlock
     * does for GL_ARRAY_BUFFER. Walking away with a mesh's index buffer still bound
     * changes what a later glDrawElements with a client-side pointer reads, and the
     * failure path did not restore it at all.
     * ☠ The CPU pointer is deliberately NOT freed and [self+0x3c] deliberately NOT
     * cleared: unlike the vertex path, that field normally holds the ENGINE's own
     * buffer (PlatformLock only allocates when it finds NULL there), so freeing it
     * would hand the engine's allocation to the wrong allocator. */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return ok ? 1 : 0;
}

/* ---- map_buffers = 1: PRE-ALLOCATING LOCK HOOKS (2026-08-06) --------------
 *
 * This is the missing half the `map_buffers` comment at the bottom of this file
 * asks for, and the reason enabling it used to crash on boot.
 *
 * Disassembly of the engine (T3VertexBuffer::PlatformLock @0x57b998 ->
 * RenderDevice::MapGLBuffer @0x578fcc) shows MapGLBuffer only ever reaches
 * RenderDevice::AllocateGLBuffer on the branch taken when cap 22
 * (mRenderCaps & 0x400000, EXT_map_buffer_range) is CLEAR. On this device the
 * boot log reports `OES_mapbuffer=1 EXT_map_buffer_range=1`, so caps 21 and 22
 * are both set, the allocating branch is never taken, and the engine maps a GL
 * buffer name whose storage nothing ever created. The shipped libvitaGL.a is
 * built NO_DEBUG, so glMapNamedBufferRange returns `ptr + offset` with no null
 * check -- and ptr is NULL. The skinner then writes hundreds of KB near address
 * zero. That is the boot crash, not anything wrong with the zero-copy idea.
 *
 * So: keep a hook on PlatformLock ONLY (never on Unlock -- the engine's own
 * glUnmapBuffer must run), guarantee storage exists at the right size, then
 * SO_CONTINUE into the engine so it maps and skins straight into GPU memory.
 * glBufferData is issued once per buffer and again only if the size changes,
 * which is what removes the per-frame malloc + full respecify + free that makes
 * framerate collapse once a scene has more than a couple of animated NPCs.
 *
 * ⚠ NOT ON BY DEFAULT. This is written from static analysis of the engine and
 * the installed vitaGL, and has not been run on hardware. `map_buffers = 1` in
 * settings/graphics.txt selects it. Verify with the SKINPROBE line: respec/f and
 * KB/f should fall to ~0. Promote to default only after a device run. */
#define VB_ALLOC_NAME_CAP 8192
static uint32_t g_vb_alloc_size[VB_ALLOC_NAME_CAP];

static void vb_ensure_gl_storage(uint32_t name, GLenum target, size_t bytes) {
    if (!name || !bytes) {
        return;
    }
    /* Names beyond the table are rare; respecify them every lock rather than
     * risk mapping unallocated storage. Correctness first, then the fast path. */
    if (name < VB_ALLOC_NAME_CAP && g_vb_alloc_size[name] == (uint32_t)bytes) {
        return;
    }
    glBindBuffer(target, name);
    glBufferData(target, (GLsizeiptr)bytes, NULL, GL_DYNAMIC_DRAW);
    const GLenum err = glGetError();
    if (err == GL_NO_ERROR) {
        if (name < VB_ALLOC_NAME_CAP) {
            g_vb_alloc_size[name] = (uint32_t)bytes;
        }
    } else {
        if (name < VB_ALLOC_NAME_CAP) {
            g_vb_alloc_size[name] = 0;
        }
        l_warn("MAPBUF: glBufferData(name=%u, %u bytes) failed err=0x%04X",
               (unsigned)name, (unsigned)bytes, (unsigned)err);
    }
    glBindBuffer(target, 0);
}

static so_hook g_hook_vb_prealloc_lock;
static so_hook g_hook_ib_prealloc_lock;

static int hook_vertexbuffer_prealloc_lock(void *self, int write) {
    if (self) {
        const uint32_t *u32 = (const uint32_t *)self;
        /* Only the FIRST lock allocates: a nested lock already has a live mapped
         * pointer at +0xd0 and the engine just bumps the count at +0xe0. */
        if (u32[0xe0 / 4] == 0 && u32[0xd0 / 4] == 0) {
            vb_ensure_gl_storage(u32[0x20 / 4], GL_ARRAY_BUFFER,
                                 (size_t)u32[0xc0 / 4] * (size_t)u32[0xc4 / 4]);
        }
    }
    return SO_CONTINUE(int, g_hook_vb_prealloc_lock, self, write);
}

static int hook_indexbuffer_prealloc_lock(void *self, int write) {
    if (self) {
        const uint32_t *u32 = (const uint32_t *)self;
        if (u32[0x24 / 4] == 0 && u32[0x3c / 4] == 0) {
            vb_ensure_gl_storage(u32[0x20 / 4], GL_ELEMENT_ARRAY_BUFFER,
                                 (size_t)u32[0x2c / 4] * (size_t)u32[0x30 / 4]);
        }
    }
    return SO_CONTINUE(int, g_hook_ib_prealloc_lock, self, write);
}

static void patch_vertexbuffer_prealloc_lock(void) {
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN14T3VertexBuffer12PlatformLockEb",
                              "T3VertexBuffer::PlatformLock (map_buffers prealloc)",
                              (uintptr_t)&hook_vertexbuffer_prealloc_lock,
                              &g_hook_vb_prealloc_lock);
}

static void patch_indexbuffer_prealloc_lock(void) {
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_ZN13T3IndexBuffer12PlatformLockEb",
                              "T3IndexBuffer::PlatformLock (map_buffers prealloc)",
                              (uintptr_t)&hook_indexbuffer_prealloc_lock,
                              &g_hook_ib_prealloc_lock);
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

static void enable_validated_hot_trampoline(const char *label,
                                            so_hook *hook,
                                            uintptr_t *trampoline) {
    *trampoline = mcsm_build_tramp(hook);
    if (!*trampoline) {
        /* Correctness fallback: the handler remains installed and uses the original
         * SO_CONTINUE path if the exact runtime binary fails relocation validation. */
        l_warn("TRAMP: %s using guarded SO_CONTINUE fallback", label);
    }
}

static void patch_engine_diag_hooks(void) {
    init_metrics_diag();
    init_render_gate_diag();
    force_native_render_dimensions("patch");
    force_animation_runtime_flags("patch");
    patch_chore_full_update_path();
    patch_saveprefs_path();

    /* ★ OFF BY DEFAULT — THE ENGINE OWNS ITS ANIMATION CLOCKS.
     *
     * With anim_dt_repair = 0 (the default) Metrics::NewFrame is left completely
     * native: no hook, no trampoline, and no loader code writes mFrameTime,
     * mActualFrameTime or mTotalTime anywhere. That is the least-interference
     * configuration, and it is the one animation should be judged on.
     *
     * The engine's 0.1s frame-delta clamp is real (disassembled at 0xc71e38, and
     * visible in device logs as engine=0.100000/2.585000 after a load), but
     * repairing it means writing the engine's own timing state from a hook on
     * every frame, and that has never been shown to make animation better on
     * hardware. `anim_dt_repair = on` in graphics.txt turns it back on without a
     * rebuild if a measurement ever justifies it. */
    if (!mcsm_cfg()->anim_dt_repair) {
        l_info("ANIM: Metrics::NewFrame left NATIVE (anim_dt_repair=off) — the engine "
               "owns mFrameTime/mActualFrameTime/mTotalTime; the loader writes none of them");
    } else if (hook_symbol_checked(&so_mod_gameengine,
                                   "_ZN7Metrics8NewFrameEf",
                                   "Metrics::NewFrame",
                                   (uintptr_t)&hook_metrics_new_frame,
                                   &g_hook_metrics_new_frame)) {
        g_metrics_new_frame_tramp =
            mcsm_build_metrics_new_frame_tramp(&g_hook_metrics_new_frame);
        if (!g_metrics_new_frame_tramp) {
            l_warn("TRAMP: Metrics::NewFrame using guarded SO_CONTINUE fallback");
        }
        l_info("ANIM: frame-delta repair ENABLED (anim_dt_repair=on)");
    }

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

    /* Rate 1 is the correctness/default path and needs no hook at all. Advanced
     * rates 2/3 still work by accumulating the engine's original two deltas. */
    if (mcsm_anim_rate() > 1 &&
        hook_symbol_checked(&so_mod_gameengine,
                            "_ZN18PlaybackController25UpdatePlaybackControllersEff",
                            "PlaybackController::UpdatePlaybackControllers (explicit reduced rate)",
                            (uintptr_t)&hook_playback_controller_update,
                            &g_hook_playback_controller_update)) {
        enable_validated_hot_trampoline("PlaybackController::UpdatePlaybackControllers",
                                        &g_hook_playback_controller_update,
                                        &g_playback_controller_update_tramp);
    } else if (mcsm_anim_rate() == 1) {
        l_info("ANIM: playback controllers left native at full rate");
    }

    if (hook_symbol_checked(&so_mod_gameengine,
                            "_ZN10GameRender11RenderFrameEv",
                            "GameRender::RenderFrame",
                            (uintptr_t)&hook_gamerender_render_frame,
                            &g_hook_gamerender_render_frame)) {
        enable_validated_hot_trampoline("GameRender::RenderFrame",
                                        &g_hook_gamerender_render_frame,
                                        &g_gamerender_render_frame_tramp);
    }

    if (hook_symbol_checked(&so_mod_gameengine,
                            "_ZN10GameRender11RenderSceneER18RenderSceneContextRK16RenderParameters",
                            "GameRender::RenderScene",
                            (uintptr_t)&hook_gamerender_render_scene,
                            &g_hook_gamerender_render_scene)) {
        enable_validated_hot_trampoline("GameRender::RenderScene",
                                        &g_hook_gamerender_render_scene,
                                        &g_gamerender_render_scene_tramp);
    }

    if (hook_symbol_checked(&so_mod_gameengine,
                            "_ZN11RenderFrame7ExecuteEPS_",
                            "RenderFrame::Execute",
                            (uintptr_t)&hook_renderframe_execute,
                            &g_hook_renderframe_execute)) {
        enable_validated_hot_trampoline("RenderFrame::Execute",
                                        &g_hook_renderframe_execute,
                                        &g_renderframe_execute_tramp);
    }

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

#ifdef DEBUG_SOLOADER
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
#endif

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
/* ☠ LOG-ONLY. This used to raise the IME itself, which is now a hazard rather than a
 * fallback: the engine's REAL text entry is the JNI generic-text-dialog contract
 * (openGenericTextDialog / ...Finished / ...Cancelled / ...Value, see java.c), and
 * only ONE common dialog can exist at a time. An IME raised from here is one nothing
 * is polling -- its result has nowhere to go, and worse, it makes the genuine
 * openGenericTextDialog fail because a dialog is already up.
 * Neither this nor luaPlatformShowKeyboard has ever been observed firing on device
 * (both were traced empty across several sessions), so the log line is kept purely to
 * catch it if one ever does -- at which point wire it to mcsm_ime_begin_vkbd() with a
 * matching result path, not to a bare IME. */
static void hook_sdl_show_text_input(void *inputRect) {
    l_info("KEYBOARD: Android_JNI_ShowTextInput fired (not raising an IME — the JNI "
           "text-dialog path owns text entry)");
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

#ifdef DEBUG_SOLOADER
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
#endif

#ifdef DEBUG_SOLOADER
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
    count++;
    return SO_CONTINUE(int, g_hook_scriptmgr_init, a, b);
}

static int hook_lua_register_resdesc(void *L) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    return SO_CONTINUE(int, g_hook_lua_register_resdesc, L);
}

static int hook_lua_retry_resdesc(void *L) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    // Always log: if this fires repeatedly, resource descriptions are failing to
    // load and the boot is stuck retrying them.
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
        count++;                                                       \
        launch_state_mark_progress();                                 \
        return SO_CONTINUE(int, hook_var, L);                         \
    }

BOOT_LUA_TRACER(hook_lua_loadasync, "lua_LoadAsync", g_hook_lua_loadasync)
BOOT_LUA_TRACER(hook_lua_preloadasync, "luaPreloadAsync", g_hook_lua_preloadasync)
BOOT_LUA_TRACER(hook_lua_rset_create, "luaResourceSetCreate", g_hook_lua_rset_create)
BOOT_LUA_TRACER(hook_lua_rset_enable, "luaResourceSetEnable", g_hook_lua_rset_enable)
BOOT_LUA_TRACER(hook_lua_rset_loadingcall, "luaResourceSetLoadingCall", g_hook_lua_rset_loadingcall)
#endif /* DEBUG_SOLOADER */

// Forward declaration: LOGIN_BYPASS macros below use lua_push_forced_bool
// which is defined later (after the ChorePlay/scene tracers that need
// LuaToLStringFn etc.).
static int lua_push_forced_bool(void *L, int value, const char *label);
static int lua_push_forced_string(void *L, const char *value, const char *label);
static int lua_push_forced_integer(void *L, int value, const char *label);

// Login/connect/cloud-sync bypass (2026-06-21). The Telltale authentication
// server (services.telltalegames.com) is permanently offline.  Every one of
// these primitives tries real network I/O through the un-replaced
// UNetworkAPI::* C++ methods, which block on dead sockets until their ~30s
// timeout.  With luaPlatformIsUserSignedIn already forced to true the engine
// does NOT need a password or a remote credential — it can proceed with a
// local-offline user.  Bypass the originals entirely: return 0 Lua values
// immediately.  All are low-frequency main-thread Lua calls.
#ifdef DEBUG_SOLOADER
#define LOGIN_BYPASS_DIAG(label) do {                                   \
        static uint32_t count = 0;                                      \
        count++;                                                        \
        launch_state_mark_progress();                                   \
        if (count <= 8U || (count & 0x3FU) == 0U) {                    \
            l_info("Login bypass: %s -> true (offline) count=%u",      \
                   label, count);                                       \
        }                                                               \
    } while (0)
#else
#define LOGIN_BYPASS_DIAG(label) do { (void)(label); } while (0)
#endif
#define LOGIN_BYPASS(fn_name, label, hook_var)                          \
    static so_hook hook_var;                                            \
    static int fn_name(void *L) {                                       \
        LOGIN_BYPASS_DIAG(label);                                       \
        return lua_push_forced_bool(L, 1, label) >= 0 ? 1 : 0;         \
    }

LOGIN_BYPASS(hook_lua_show_password_box, "luaShowPasswordBox", g_hook_lua_show_password_box)
LOGIN_BYPASS(hook_lua_is_password_box_finished, "luaIsPasswordBoxFinished", g_hook_lua_is_password_box_finished)
LOGIN_BYPASS(hook_lua_get_password_box_results, "luaGetPasswordBoxResults", g_hook_lua_get_password_box_results)
/* GetCredential returns a fake NON-EMPTY credential (2026-07-23). The LOGIN_BYPASS
 * version returned bool true, which the menu read as "no stored credential" -> the
 * "sign up for full access" MyTelltale banner. A non-empty string reads as a valid
 * stored credential = already signed in. Network I/O that would USE the credential
 * (SessionLog/CloudSync/uploads) is still bypassed to no-ops, so nothing reaches
 * the dead auth server. */
static so_hook g_hook_lua_network_get_credential;
static int hook_lua_network_get_credential(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    launch_state_mark_progress();
    if (++count <= 8U) l_info("FIX: luaNetworkAPIGetCredential -> stored credential (signed-in) count=%u", count);
#endif
    int ret = lua_push_forced_string(L, "mcsm_local_ttg_cred", "NetworkAPIGetCredential");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_network_get_credential, L);
}
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
#ifdef DEBUG_SOLOADER
    MCSM_DIAG_COUNTER(count);
    launch_state_mark_progress();
#endif
    const char *nm = trace_arg_str(L, 1);
#ifdef DEBUG_SOLOADER
    if (count <= 24U || (count & 0x3FU) == 0U) {
        l_info("Diag: luaChorePlay #%u chore='%s'", count, nm ? nm : "(non-string)");
    }
#endif
    mark_character_select_license_window("chore", nm);
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
    /* ★ BOTH SPELLINGS. The game writes the SAME location two ways, and this
     * matched only one of them until 2026-07-31:
     *
     *     StatChoicesHandler.lua   "logical:<User>/choice.prop"     (one slash)
     *     menu_stats.lua           "logical://<User>/choice.prop"   (TWO slashes)
     *
     * Both strings are literals in the shipped Lua -- read out of the packed
     * .ttarch2 constant tables, not inferred -- and the engine treats them as the
     * same location. The two-slash form is what the end-of-episode / stats screen
     * uses to persist the choice_tracker document, so under a one-slash-only match
     * that write was never redirected: it addressed the <User> location, which does
     * not bind on this port, and was dropped. That is the "choices do not save"
     * mechanism, and it has nothing to do with being online -- the data is local.
     *
     * (The engine's own <Project>/<Menu> paths use the two-slash form as well, so
     * this is the general spelling rather than a one-off typo.) */
    static const char kPrefix1[] = "logical:<User>/";
    static const char kPrefix2[] = "logical://<User>/";
    const char *suffix = NULL; /* the part after any location prefix, or the whole bare name */
    if (strncmp(s, kPrefix1, sizeof(kPrefix1) - 1) == 0) {
        suffix = s + (sizeof(kPrefix1) - 1);
    } else if (strncmp(s, kPrefix2, sizeof(kPrefix2) - 1) == 0) {
        suffix = s + (sizeof(kPrefix2) - 1);
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
            strcmp(s, "choice.prop") == 0 || strcmp(s, "choicestats.prop") == 0 ||
            /* PLAYER-SELECTION PERSISTENCE FIX (2026-07-24). Same root cause as the
             * saveslot/choice.prop entries above, just never extended to the engine's
             * OWN property sets: prefs.prop / user.prop / game_prefs.prop are saved
             * and loaded by BARE name (SavePrefs -> Save('prefs.prop')), so with no
             * redirect they resolve against a NULL location that does not bind on
             * this port -- the write silently goes nowhere and the read finds nothing.
             * Device evidence (79-min session): SavePrefs ran 15 times, yet all three
             * files are still 0 bytes on the memory card and NONE of them ever appears
             * in the save-IO path, while the redirected saveslot/estore writes land
             * fine. Consequences the player sees: the chosen Jesse model reverts to the
             * default male on an existing save (a fresh save works because the choice
             * is still in memory that session), and the choices made in a session are
             * gone on the next launch. Redirect these to <Temp> so read and write
             * resolve to the same real, writable place -- exact-match only, so
             * unrelated *_prefs.prop resources are untouched. */
            strcmp(s, "prefs.prop") == 0 || strcmp(s, "user.prop") == 0 ||
            strcmp(s, "game_prefs.prop") == 0) {
            suffix = s;
        }
    }
    if (!suffix) {
        return 0;
    }
    char newpath[256];
    sceClibSnprintf(newpath, sizeof(newpath), "logical:<Temp>/%s", suffix);
#ifdef DEBUG_SOLOADER
    static uint32_t redirect_count = 0;
    redirect_count++;
    if (redirect_count <= 48U) {
        l_info("SAVEFIX2: redirecting '%s' -> '%s' (#%u)", s, newpath, redirect_count);
    }
#endif
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
#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

/* Save()/Create() are also called constantly for non-save resources (props,
 * configs) at boot -- trace only the ones whose arg string looks save-shaped
 * so this doesn't flood the log. */
static so_hook g_hook_lua_save;
static int hook_lua_save(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
#endif
    redirect_logical_user_to_temp(L, 1);
#ifdef DEBUG_SOLOADER
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: Save('%s') ENTER #%u", nm ? nm : "?", count);
    }
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_save, L);
#ifdef DEBUG_SOLOADER
    if (is_save_shaped) {
        l_info("SAVETRACE: Save('%s') RETURN #%u", nm ? nm : "?", count);
    }
#endif
    return ret;
}

static so_hook g_hook_lua_create;
static int hook_lua_create(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
#endif
    redirect_logical_user_to_temp(L, 1);
#ifdef DEBUG_SOLOADER
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: Create('%s') ENTER #%u", nm ? nm : "?", count);
    }
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_create, L);
#ifdef DEBUG_SOLOADER
    if (is_save_shaped) {
        l_info("SAVETRACE: Create('%s') RETURN #%u", nm ? nm : "?", count);
    }
#endif
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
/* ★ CHOICE-SAVE SPLIT-BRAIN (fixed 2026-07-30). luaPropertyGet and
 * luaPropertyExists were redirected logical:<User>/ -> logical:<Temp>/ but the
 * WRITE side never was: luaPropertySet, luaPropertyCreate, luaPropertyRemove and
 * luaPropertyClearKeys all shipped unhooked despite existing in the engine.
 *
 * So every read resolved to <Temp> while every write went to the still-unbound
 * <User> location -- the exact dead-location failure the redirect exists to fix.
 * Choices were written into nowhere and read back from a file that never received
 * them. That is the mechanism behind "my choices vanish next session": not a
 * durability or fsync problem, a destination problem.
 *
 * ChoiceStats.lua is the clearest case -- its read path (PropertyGet /
 * PropertyExists / ResourceExists on choice.prop) was redirected while its write
 * path (PropertyClearKeys + PropertyCreate on choicestats.prop) was not.
 *
 * All four take the resource name in arg1, so the existing macro applies. */
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
/* CHOICE-LOSS DIAGNOSTIC (2026-07-24): this is how the Choices screen reads the
 * player's own past decisions out of the event log. Log it (capped) so we can see
 * whether it is even called, and against which log, when the screen comes up
 * empty on a fresh launch. */
static so_hook g_hook_lua_query_event_log;
static int hook_lua_query_event_log(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    const char *a1 = trace_arg_str(L, 1);
    if (++count <= 24U) {
        l_info("EVENTLOG: QueryEventLog #%u arg1='%s'", count, a1 ? a1 : "(?)");
    }
#endif
    redirect_logical_user_to_temp(L, 1);
    return SO_CONTINUE(int, g_hook_lua_query_event_log, L);
}
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
REDIRECT_ARG1_HOOK(hook_lua_property_set,        g_hook_lua_property_set)
REDIRECT_ARG1_HOOK(hook_lua_property_create,     g_hook_lua_property_create)
REDIRECT_ARG1_HOOK(hook_lua_property_remove,     g_hook_lua_property_remove)
REDIRECT_ARG1_HOOK(hook_lua_property_clearkeys,  g_hook_lua_property_clearkeys)

/* CHOICE-LOSS DIAGNOSTIC (2026-07-24): player choices survive the session they
 * are made in but are gone on the next launch. Device evidence: the .estore
 * index is rewritten O_TRUNC ~20x per session, and older event PAGES that exist
 * on the memory card (_saveslot1_id_PageNNNN.epage from earlier sessions) are
 * NEVER re-opened -- only the live page is. So the events are on disk but the
 * log never loads its history back. The two candidates are (a) the log being
 * re-created empty instead of opened, and (b) DeleteAllEventsAfterEvent wiping
 * the log when the engine rolls it back to the loaded save point (if the anchor
 * event is not found, "delete everything after" can mean "delete all"). Both are
 * silent today. Log the whole lifecycle so one test session identifies which. */
static so_hook g_hook_lua_delete_all_events_after;
static int hook_lua_delete_all_events_after(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    const char *a1 = trace_arg_str(L, 1);
    const char *a2 = trace_arg_str(L, 2);
    if (++count <= 24U) {
        l_info("EVENTLOG: DeleteAllEventsAfterEvent #%u log='%s' afterEvent='%s'",
               count, a1 ? a1 : "(?)", a2 ? a2 : "(?)");
    }
#endif
    redirect_logical_user_to_temp(L, 1);
    return SO_CONTINUE(int, g_hook_lua_delete_all_events_after, L);
}

static so_hook g_hook_lua_event_log_create;
static int hook_lua_event_log_create(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    const char *name = trace_arg_str(L, 1);
    const char *res  = trace_arg_str(L, 3);
    if (++count <= 24U) {
        l_info("EVENTLOG: EventLogCreate #%u name='%s' resource='%s'",
               count, name ? name : "(?)", res ? res : "(?)");
    }
#endif
    redirect_logical_user_to_temp(L, 3); /* the backing-resource path arg */
    int ret = SO_CONTINUE(int, g_hook_lua_event_log_create, L);
#ifdef DEBUG_SOLOADER
    if (count <= 24U) {
        l_info("EVENTLOG: EventLogCreate #%u RETURN ret=%d", count, ret);
    }
#endif
    return ret;
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
#ifdef DEBUG_SOLOADER
    const char *a1 = trace_arg_str(L, 1);
    const char *a2 = trace_arg_str(L, 2);
    l_info("CHOICEIO: SaveDownloadedDoc arg1='%s' arg2='%s'", a1 ? a1 : "?", a2 ? a2 : "?");
#endif
    redirect_logical_user_to_temp(L, 1);
    /* The output path. Two callers, two spellings -- StatChoicesHandler.lua passes
     * "logical:<User>/choice.prop" and menu_stats.lua passes
     * "logical://<User>/choice.prop"; the redirect accepts both. */
    redirect_logical_user_to_temp(L, 2);
    return SO_CONTINUE(int, g_hook_lua_save_downloaded_doc_as_propset, L);
}

/* 2026-07-16 CHOICE-STATS READ DIAGNOSIS. ChoiceStats.lua reads the crowd stats
 * via ResourceExists("choice.prop") + PropertyGet; StatChoicesHandler may also
 * retrieve the downloaded doc via these. Offline the server fetch never lands, so
 * LOG the whole download/retrieve flow (and redirect any <User> path so a pre-
 * placed choice.prop is served) to see EXACTLY where the offline read stops. */
static so_hook g_hook_lua_download_doc_retrieve;
static int hook_lua_download_doc_retrieve(void *L) {
#ifdef DEBUG_SOLOADER
    const char *a1 = trace_arg_str(L, 1);
    l_info("CHOICEIO: DownloadDocumentRetrieve arg1='%s'", a1 ? a1 : "?");
#endif
    redirect_logical_user_to_temp(L, 1);
    redirect_logical_user_to_temp(L, 2);
    return SO_CONTINUE(int, g_hook_lua_download_doc_retrieve, L);
}
#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

/* Keep this hook in production: SavePrefs is synchronous, so immediately after
 * it returns we can reconcile its native bare-resource output with the Lua-facing
 * <Temp> copy. Diagnostics remain conditional. */
static so_hook g_hook_lua_save_prefs;
static int hook_lua_save_prefs(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    count++;
    l_info("SAVETRACE: SavePrefs ENTER #%u", count);
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_save_prefs, L);
#ifdef DEBUG_SOLOADER
    l_info("SAVETRACE: SavePrefs RETURN #%u", count);
#endif
    mcsm_sync_prefs_after_save();
    return ret;
}

static so_hook g_hook_lua_resource_exists_redirect;
static int hook_lua_resource_exists_redirect(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
#endif
    redirect_logical_user_to_temp(L, 1);
#ifdef DEBUG_SOLOADER
    const char *nm = trace_arg_str(L, 1);
    int is_save_shaped = nm && (strstr(nm, "bundle") || strstr(nm, "saveSlot") ||
                                 strstr(nm, "saveslot") || strstr(nm, "_saveslot") ||
                                 strstr(nm, "choice") || strstr(nm, "Choice"));
    if (is_save_shaped) {
        count++;
        l_info("SAVETRACE: ResourceExists('%s') ENTER #%u", nm ? nm : "?", count);
    }
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_resource_exists_redirect, L);
#ifdef DEBUG_SOLOADER
    if (is_save_shaped) {
        l_info("SAVETRACE: ResourceExists('%s') RETURN #%u", nm ? nm : "?", count);
    }
#endif
    return ret;
}

#ifdef DEBUG_SOLOADER
static so_hook g_hook_lua_sceneopen;
static int hook_lua_sceneopen(void *L) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    const char *nm = trace_arg_str(L, 1);
    l_info("Diag: luaSceneOpen #%u scene='%s'", count, nm ? nm : "(non-string)");
    /* A menu-vs-gameplay flag used to be tracked here, to report "online" only
     * during gameplay. It was never read by anything, and the reasoning behind it
     * was wrong twice over: the end-of-episode choice-stats screen -- the one place
     * that needs the online report -- IS a menu (menu_endepisode.lua gates it on
     * MenuUtils_PlatformIsConnectedToInternet), and the upsell banner it was meant
     * to suppress is suppressed by forcing the season purchased instead. Removed
     * rather than left as a lie in the source. */
    return SO_CONTINUE(int, g_hook_lua_sceneopen, L);
}
#endif /* DEBUG_SOLOADER */

/* DIAGNOSTIC (2026-06-21): the menu reached its scripts but parks in
 * Menu_Main's startup BEFORE opening its scene, while Load()-ing the menu
 * CHARACTER MESHES (skM1_jesse/olivia/lukas/axel/petra/ellie/gabriel_*.d3dmesh
 * + portal blocks). Trace luaLoad ENTER+RETURN with the resource name: the
 * resource that logs ENTER and never RETURN is the one whose load hangs.
 * Also trace luaResourceIsLoaded (throttled) in case it polls "is it loaded?"
 * forever on an async load that never completes. Main-thread Lua -> safe. */
static so_hook g_hook_lua_load;
static int hook_lua_load(void *L) {
    /* ☠ NO trace_arg_str() HERE. It was a real lua_tolstring round trip on every
     * luaLoad, and its only two readers were verbose-diag log lines that had already
     * been compiled out to `return 0` -- so the engine paid for a string conversion
     * on every resource load to produce a value nothing could read. The redirect is
     * the entire job of this hook. */
    redirect_logical_user_to_temp(L, 1);
    return SO_CONTINUE(int, g_hook_lua_load, L);
}
#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

/* ★ SCENE-PRELOAD PUMP (restored 2026-08-06) ------------------------------
 *
 * luaResourceAdvancePreloadBatch (engine 0x00c41858) is a Lua-callable C
 * function: on Android the game's script update loop calls it every frame until
 * the async preload started by ScenePreload reports done. On this port that
 * pumping does not happen, so a preload that is started is never advanced --
 * the scene never becomes ready and the game sits waiting for it while FMOD
 * keeps streaming dialogue on its own thread. That is the "frozen with audio
 * still playing at a scene change, have to quit and relaunch" report, and why
 * it reproduces around story branches (the Ruben choice) where a new scene is
 * preloaded.
 *
 * 1.10 deleted the pump. The objection behind that was fair -- the old version
 * cached a raw lua_State and could in principle keep calling forever -- so this
 * restores the pump WITH the bounds it never had:
 *   - at most 2 batch calls and 4ms of work per frame (as before),
 *   - a hard wall-clock deadline, after which we stop pumping regardless, so a
 *     wedged or stale preload degrades to the old behaviour instead of pinning
 *     the sim thread every frame for the rest of the session,
 *   - the pump is dropped the moment a batch reports completion.
 *
 * The engine keeps one long-lived script lua_State, so the cached pointer stays
 * valid for the window this is used in (ScenePreload -> preload complete). */
#define STREAM_PRELOAD_MAX_US   (20ULL * 1000000ULL)  /* hard stop for one preload */

static void    *g_preload_lua_state = NULL;
static volatile int g_preload_pending = 0;
static uint64_t g_preload_started_us = 0;
#ifdef DEBUG_SOLOADER
static volatile int g_preload_log_once  = 0;
#endif

/* Direct function pointer to luaResourceAdvancePreloadBatch so the batch can be
 * driven from the C++ game loop without a round trip through Lua. */
typedef int (*AdvancePreloadBatchFn)(void *L);
static AdvancePreloadBatchFn g_advance_preload_fn = NULL;

static int stream_resolve_preload_api(void) {
    if (!g_advance_preload_fn) {
        uintptr_t addr = so_symbol(&so_mod_gameengine,
                          "_Z30luaResourceAdvancePreloadBatchP9lua_State");
        if (!addr) {
#ifdef DEBUG_SOLOADER
            if (!g_preload_log_once) {
                l_warn("STREAM: cannot resolve AdvancePreloadBatch symbol");
                g_preload_log_once = 1;
            }
#endif
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

    const uint64_t pump_t0 = sceKernelGetSystemTimeWide();

    /* Hard deadline. A preload this old is not going to finish by being pumped
     * harder, and continuing to call into it every frame is the failure mode the
     * removal was worried about. Give up and let the engine proceed. */
    if (g_preload_started_us && (pump_t0 - g_preload_started_us) > STREAM_PRELOAD_MAX_US) {
        g_preload_pending = 0;
        l_warn("STREAM: preload pump deadline reached; releasing pump");
        return;
    }

#ifdef DEBUG_SOLOADER
    int pumped = 0;
#endif
    for (int i = 0; i < 2 && g_preload_pending; ++i) {
        int ret = g_advance_preload_fn(g_preload_lua_state);
#ifdef DEBUG_SOLOADER
        pumped++;
#endif
        if (ret <= 0) {
            g_preload_pending = 0;
#ifdef DEBUG_SOLOADER
            l_info("STREAM: preload pump complete after %d calls", pumped);
#endif
            break;
        }
        if ((sceKernelGetSystemTimeWide() - pump_t0) >= 4000ULL) {
            break;
        }
    }
#ifdef DEBUG_SOLOADER
    if (pumped && g_preload_pending && !g_preload_log_once) {
        g_preload_log_once = 1;
        l_info("STREAM: preload pump still pending after %d calls", pumped);
    }
    const uint32_t pump_ms = (uint32_t)((sceKernelGetSystemTimeWide() - pump_t0) / 1000ULL);
    if (pump_ms > 25U) {
        l_info("STREAM: preload pump budget overrun calls=%d work=%ums", pumped, pump_ms);
    }
#endif
}

static so_hook g_hook_lua_scene_preload;
static int hook_lua_scene_preload(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    count++;
    const char *nm = trace_arg_str(L, 1);
#endif
    /* 2026-06-21 (revised): SKIPPING ScenePreload makes the PRESENT hang — across
     * runs the render thread's eglSwapBuffers wedges only when ScenePreload is
     * skipped (logs 185908, 233044), whereas with the real ScenePreload running
     * the present stays alive for 25k+ frames (log 215037). There the game only
     * stalled in ScenePreload's RenderThread::FinishFrame because its texture
     * uploads OOM'd and never finished — which the GL-layer LRU eviction cap
     * (glutil.c) now prevents. So run the REAL ScenePreload (keeps present alive)
     * and let the cap bound its uploads so FinishFrame completes and it returns. */
#ifdef DEBUG_SOLOADER
    l_info("STREAM: luaScenePreload #%u scene='%s' (real preload)",
           count, nm ? nm : "(?)");
#endif
    g_preload_lua_state  = L;
    g_preload_pending    = 1;
    g_preload_started_us = sceKernelGetSystemTimeWide();
#ifdef DEBUG_SOLOADER
    g_preload_log_once  = 0;
#endif
    int ret = SO_CONTINUE(int, g_hook_lua_scene_preload, L);
#ifdef DEBUG_SOLOADER
    l_info("STREAM: luaScenePreload #%u returned %d", count, ret);
#endif
    if (ret == 0) {
        g_preload_pending = 0;
    }
    return ret;
}

#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

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
#ifdef DEBUG_SOLOADER
    (*count)++;
    launch_state_mark_progress();
    if (*count <= 12U || ((*count) & 0x7fU) == 0U) {
        l_info("DLC fastpath: %s -> %s count=%u L=%p",
               label,
               value ? "true" : "false",
               *count,
               L);
    }
#else
    (void)count;
#endif
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
    /* CROWD-CHOICE STATS (2026-07-22): the end-of-episode "% of players" screen
     * checks THIS right before reading the (locally-served) choice.prop and shows
     * "offline" when it's false. Every real call happens in-game (device logs: all
     * at frame ~3000+, none during the boot DLC verification), so report connected
     * ONCE A SCENE IS LIVE and stay false during pure boot. Online once a scene is
     * live (menu included) so the crowd-choice stats work; the upsell banner is
     * killed separately by forcing the season purchased (IsEpisodePurchased=1).
     *
     * ★ CONFIRMED FROM THE SHIPPED LUA (2026-07-31), not inferred. Two different
     * gates read this, and they disagree about whether it is required:
     *   menu_endepisode.lua: connected = MenuUtils_PlatformIsConnectedToInternet()
     *       and not IsPlatformWiiU(); if false -> popupSplash_statsOffline_body.
     *       No local fallback -- so the end-of-episode stats genuinely need this.
     *   menu_main.lua: the Stats entry also accepts ResourceExists('choice.prop'),
     *       i.e. the game's OWN offline path, which is why the pre-baked crowd file
     *       matters (init.c mirror_crowd_choice_data).
     * Nothing about SAVING choices depends on this flag; that path was broken by an
     * unredirected "logical://<User>/" spelling and is fixed in
     * redirect_logical_user_to_temp(). Every server call this flag can reach
     * (SessionLogProcess, Upload*ToServer, CloudSyncUserData) is already bypassed to
     * an immediate local success, so "online" costs no network wait. */
    int forced = launch_state_scene_active() ? 1 : 0;
    int ret = hook_forced_lua_bool(L, forced, "PlatformIsConnectedToInternet", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_connected_to_internet, L);
}

static int hook_lua_platform_is_connected_to_license_server(void *L) {
    static uint32_t count = 0;
    /* Tracks IsConnectedToInternet EXACTLY (both false at boot, both true once a
     * scene is live). The documented DLC boot-loop is the "license-true +
     * internet-false" IMPOSSIBLE state; keeping the two coupled means that combo
     * never occurs. Reporting license-server connected in-game (alongside internet)
     * makes the game consider full access VERIFIED, so the online menu stops
     * showing the "sign up for full access" upsell banner. (2026-07-22)
     * Gameplay-only (not the menu), coupled to the internet flag above. */
    int forced = launch_state_scene_active() ? 1 : 0;
    int ret = hook_forced_lua_bool(L, forced, "PlatformIsConnectedToLicenseServer", &count);
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

/* EPISODE VISIBILITY via settings/game.txt
 * -----------------------------------------------------------------------------
 * The engine asks luaIsEpisodeAvailable/Downloaded once per episode. We answer
 * per chapter so ONLY the chapters you want appear in Episode Select -- and you
 * change which ones show WITHOUT ever rebuilding again: just edit a text file on
 * the Vita:
 *
 *     chapter1 = on
 *     chapter2 = auto
 *     chapter3 = off
 *
 * Resolution order for a given chapter N:
 *   1. Chapter 1 is ALWAYS available (never break the base game / a CH1 test).
 *   2. If game.txt lists N as on/off -> obey it exactly.
 *   3. Otherwise fall back to "is its data archive actually on disk?"
 *      (assets/MCSM_android_Minecraft10N_data.ttarch2).
 *   4. If we cannot identify the episode at all -> available (old safe default).
 * game.txt is read once and cached; `auto` uses on-disk detection. */
static int mcsm_file_present(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}
static signed char s_ch_override[9];        /* [1..8]: 1=show, 0=hide, -1=auto */
static int         s_ch_settings_loaded = 0;
static void mcsm_load_chapter_settings(void) {
    const McsmGame *g = mcsm_game();
    s_ch_override[0] = -1;
    for (int i = 0; i < 8; i++) s_ch_override[i + 1] = (signed char)g->chapters[i];
    s_ch_settings_loaded = 1;
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
#ifdef DEBUG_SOLOADER
    static signed char s_logged[9];                    /* log each decision once (diag) */
#endif
    int ch = mcsm_episode_chapter(L);
    if (ch == 1) return 1;                              /* CH1 = base game, always available */
    if (!s_ch_settings_loaded) mcsm_load_chapter_settings();
    if (ch >= 2 && ch <= 8) {
        int vis; const char *src;
        if (s_ch_override[ch] == 1)      { vis = 1; src = "game.txt"; }
        else if (s_ch_override[ch] == 0) { vis = 0; src = "game.txt"; }
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
#ifdef DEBUG_SOLOADER
        if (!s_logged[ch]) { s_logged[ch] = 1;
            l_info("EPVIS: ch=%d -> %s (%s)", ch, vis ? "SHOW" : "hide", src); }
#else
        (void)src;
#endif
        return vis;
    }
    /* ch <= 0: an episode we cannot map to chapter 1..8. This previously returned
     * 1 (show), which presented chapters with no data behind them. Default to HIDE
     * so nothing unrecognized leaks into Episode Select. CH1 is recognized above,
     * so the base game is never affected. */
#ifdef DEBUG_SOLOADER
    if (!s_logged[0]) { s_logged[0] = 1; l_info("EPVIS: unidentified episode -> hide"); }
#endif
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
     * no data.
     * FORCED 1 AGAIN (2026-07-23): the online menu shows a "sign up for full
     * access" upsell whenever ANY episode reads as not-purchased. Report the whole
     * season as OWNED to kill it. The menu no longer presents empty chapters off
     * THIS flag -- visibility/"installed" now runs off IsEpisodeAvailable +
     * IsEpisodeDownloaded (both still per-chapter below), so CH3-8 without data
     * stay hidden while the season counts as bought. */
    int ret = hook_forced_lua_bool(L, 1, "IsEpisodePurchased", &count);
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
 * getLocale doesn't pick the text language — luaGetUserSystemLanguage supplies
 * the initial choice, and luaLangSetCurLanguage applies later menu/prefs choices.
 * Force BOTH boundaries from settings/game.txt so stale prefs cannot switch a
 * builder-selected language back to English. The
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
#ifdef DEBUG_SOLOADER
    static int logged = 0;
#endif
#ifdef DEBUG_SOLOADER
    if (!logged) {
        logged = 1;
        if (!g_lua_tolstring_fast) {
            g_lua_tolstring_fast = (LuaToLStringFn)so_symbol(&so_mod_gameengine, "lua_tolstring");
        }
        const char *nat = g_lua_tolstring_fast ? g_lua_tolstring_fast(L, -1, NULL) : NULL;
        l_info("LANG: GetUserSystemLanguage native=\"%s\" forced=\"%s\"",
               nat ? nat : "(null)", forced[0] ? forced : "(none)");
    }
#endif
    if (forced[0]) {
        int r = lua_push_forced_string(L, forced, "GetUserSystemLanguage");
        if (r >= 0) return r;
    }
    return orig;
}

typedef void (*LuaReplaceFn)(void *L, int idx);
static LuaReplaceFn g_lua_replace_fast;
static so_hook g_hook_lua_lang_set_cur_language;

static int hook_lua_lang_set_cur_language(void *L) {
    char forced[32];
    mcsm_forced_language_name(forced, sizeof(forced));
    if (forced[0]) {
        if (!g_lua_pushstring_fast) {
            g_lua_pushstring_fast = (LuaPushStringFn)so_symbol(&so_mod_gameengine, "lua_pushstring");
        }
        if (!g_lua_replace_fast) {
            g_lua_replace_fast = (LuaReplaceFn)so_symbol(&so_mod_gameengine, "lua_replace");
        }
        if (g_lua_pushstring_fast && g_lua_replace_fast) {
            /* Replace arg1 in place. Do not add a second call or a second return:
             * one language-set operation reaches the engine, with one identity. */
            g_lua_pushstring_fast(L, forced);
            g_lua_replace_fast(L, 1);
#ifdef DEBUG_SOLOADER
            static unsigned logged = 0;
            if (logged++ < 8U) l_info("LANG: LangSetCurLanguage forced to \"%s\"", forced);
#endif
        }
    }
    return SO_CONTINUE(int, g_hook_lua_lang_set_cur_language, L);
}

static int hook_lua_get_demo_mode(void *L) {
    static uint32_t count = 0;
    int ret = hook_forced_lua_bool(L, 0, "GetDemoMode(full game)", &count);
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_get_demo_mode, L);
}

static int hook_lua_get_demo_timeout(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GetDemoTimeout -> 0 count=%u L=%p", count, L);
    }
    int ret = lua_push_forced_integer(L, 0, "GetDemoTimeout");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_get_demo_timeout, L);
}

/* GPU TIER (2026-07-29). PlatformGetGPUQuality is how the game's own Lua asks
 * "how strong is this device's GPU?", and it drives the quality the engine then
 * configures itself with. Disassembling it shows a 0..3 tier derived from the
 * platform enum: >18 -> 3, 15..18 -> 2, 10..14 -> 1, 1..9 -> 0, else 2. We report
 * as Android, so the game sees a mid/high tier and sets itself up accordingly --
 * which is why it boots at maximum render quality on hardware far weaker than any
 * phone in that bracket.
 *
 * Forcing 0 makes the game select the SAME low-spec path budget Android phones
 * get. That is the engine's own supported configuration rather than us guessing at
 * internal quality numbers, so it should scale every chapter consistently and can
 * be undone by deleting one config line. Default is -1 (untouched) -- this only
 * does anything if graphics.txt asks for it. */
static so_hook g_hook_lua_platform_get_gpu_quality;
static int hook_lua_platform_get_gpu_quality(void *L) {
    MCSM_DIAG_COUNTER(count);
    const int tier = mcsm_cfg()->gpu_tier;
    if (count <= 8U) {
        l_info("GPUTIER: PlatformGetGPUQuality -> %s (cfg gpu_tier=%d)",
               tier >= 0 ? "forced" : "engine default", tier);
    }
    if (tier < 0) {
        return SO_CONTINUE(int, g_hook_lua_platform_get_gpu_quality, L);
    }
    int ret = lua_push_forced_integer(L, tier, "PlatformGetGPUQuality");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_get_gpu_quality, L);
}

static int hook_lua_platform_get_trial_timeout(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: PlatformGetTrialTimeout -> 0 count=%u L=%p", count, L);
    }
    int ret = lua_push_forced_integer(L, 0, "PlatformGetTrialTimeout");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_get_trial_timeout, L);
}

static int hook_lua_platform_can_user_make_purchases(void *L) {
    static uint32_t count = 0;
    /* FORCED FALSE (2026-07-22): every episode is already forced owned+licensed
     * (IsEpisodePurchased->available, IsEpisodeUnlicensed->false, Licensed->true),
     * so there is nothing to buy. Once the game reports "online" in-game (for the
     * crowd-choice stats), true here made the menu show the Amazon store "sign up
     * for full access" upsell banner. False = no store entry = no banner, and owned
     * content is unaffected. */
    int ret = hook_forced_lua_bool(L, 0, "PlatformCanUserMakePurchases", &count);
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
    MCSM_DIAG_COUNTER(count);
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
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GameEngine::GetTrialVersion -> false (#%u)", count);
    }
    return 0;
}

static int hook_gameengine_get_trial_version_secure(void) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: GameEngine::GetTrialVersionSecure -> false (#%u)", count);
    }
    return 0;
}

static int hook_ttplatform_is_trial_version(void *self) {
    MCSM_DIAG_COUNTER(count);
    (void)self;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("FULLGAME: TTPlatform::IsTrialVersion -> false (#%u)", count);
    }
    return 0;
}

static int hook_ttplatform_is_user_space_available(void *self) {
    MCSM_DIAG_COUNTER(count);
    (void)self;
    if (count <= 12U || (count & 0x7fU) == 0U) {
        l_info("SAVEFIX: TTPlatform::IsUserSpaceAvailable -> true (#%u)", count);
    }
    return 1;
}

static int hook_platform_android_is_user_space_available(void *self) {
    MCSM_DIAG_COUNTER(count);
    (void)self;
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

#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

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
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
    count++;
    resolve_lua_str_api();
    const char *name = NULL;
    if (g_lua_type_fast && g_lua_tolstring_fast &&
        g_lua_type_fast(L, 1) == LUA_TSTRING_TAG) {
        name = g_lua_tolstring_fast(L, 1, NULL);
    }
    l_info("TRACE: ResetGame -> '%s' (#%u)", name ? name : "(non-string)", count);
#endif
    clear_character_select_license_window();
    return SO_CONTINUE(int, g_hook_lua_reset_game, L);
}

/* Trace which resource sets get enabled -- on a successful episode launch the
 * episode's set (e.g. Android_KP_101 / Minecraft101 / JC101) is enabled here.
 * Confirms whether the chapter load is actually reached after the Licensed flip. */
#ifdef DEBUG_SOLOADER
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
#endif /* DEBUG_SOLOADER */

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
 * for the current episode's Jesse; the 320MB heap has room).
 *
 * ☠ ONE SWITCH, NOT TWO (fixed 2026-07-31). There used to be a second, opt-OUT
 * file here (no_keep_resident.txt) from back when this was on by default. Once the
 * feature became opt-in the two switches were ANDed together, which is the same
 * "two switches for one resource" shape that previously left the ARM clock owned by
 * nobody -- and here it had a worse property: keep_resident_opt_in() LOGS
 * "keep-resident ENABLED", while the opt-out vetoed silently. A user with a stale
 * no_keep_resident.txt from an older build who then created keep_resident.txt got a
 * log line saying the feature was on and a game where it was off, with nothing to
 * explain the difference. The opt-in file is now the only control. */
static const char *resource_set_arg_name(void *L); /* fwd decl (defined below) */
/* ★ THIS OPTIMISATION CAUSED THE "female Jesse reverts to male" BUG (fixed
 * 2026-07-30). It is now OPT-IN and off by default.
 *
 * The stutter analysis above was correct about the mechanism but wrong about the
 * meaning. It observed that "PlayerChoice_Set (Lua) disables THEN re-enables the
 * character resource set" and read that as pure scene-transition thrash. It is
 * not: PlayerChoice_Set is the GENDER CHOICE. Disabling JesseMale and enabling
 * JesseFemale is exactly how the game switches Jesse's model.
 *
 * By dropping the disable we left BOTH genders enabled simultaneously, and the
 * engine resolved to male -- the default. That matches every report: it happened
 * on brand-new saves (so never a save-persistence problem, which is what it was
 * originally misdiagnosed as), and "she sometimes appears if I reload" was just
 * load-order luck between two sets that were both live.
 *
 * A correct version is possible -- defer the disable, cancel it if the SAME set
 * is re-enabled (real thrash), execute it if the OPPOSITE gender is enabled (a
 * real switch) -- but replaying a dropped Lua call means rewriting the argument
 * at stack index 1 under a hook, and that is not worth the risk against a
 * visible gameplay bug. Correctness first; the freeze is an annoyance, a Jesse
 * the player did not choose is not.
 *
 * Opt back in with graphics.txt `keep_resident = on` if you play male Jesse
 * and want the ~4-5s character-swap freezes gone. */
static int keep_resident_opt_in(void) {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = mcsm_cfg()->keep_resident ? 1 : 0;
        if (s_on) l_warn("PERF: keep-resident ENABLED via graphics.txt keep_resident — "
                         "note this forces MALE Jesse regardless of your choice.");
    }
    return s_on;
}

static int resource_set_should_stay_resident(const char *name) {
    /* This hook is installed only when keep_resident_opt_in() was latched true. */
    return name &&
        (strstr(name, "JesseMale") != NULL ||
         strstr(name, "JesseFemale") != NULL);
}
static so_hook g_hook_lua_resource_set_disable;
static int hook_lua_resource_set_disable(void *L) {
    const char *name = resource_set_arg_name(L);
    if (resource_set_should_stay_resident(name)) {
#ifdef DEBUG_SOLOADER
        static uint32_t skipped = 0;
        skipped++;
        if (skipped <= 24U || (skipped & 0x3FU) == 0U) {
            l_info("PERF: kept '%s' resident (skipped disable -> avoids 30MB re-decode freeze) #%u",
                   name, skipped);
        }
#endif
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

/* Is chapter 2 meant to be shown? game.txt is authoritative (chapter2=on/off);
 * `auto` shows it only when its data archive is present. */
static int mcsm_ch2_forced_visible(void) {
    if (!s_ch_settings_loaded) mcsm_load_chapter_settings();
    if (s_ch_override[2] == 1) return 1;
    if (s_ch_override[2] == 0) return 0;
    /* Memoized: this is reached from the resource-set hooks, which fire hundreds of
     * times during a load, and the probe is an fopen on a .ttarch2 path. Chapter
     * archives are copied in before boot and never appear or vanish mid-session, so
     * one probe is as good as hundreds -- the same reasoning (and the same fix) as
     * the per-episode s_present memo in mcsm_episode_available(). */
    static signed char s_ch2_present = -1;
    if (s_ch2_present < 0) {
        s_ch2_present = (signed char)(
            mcsm_file_present(DATA_PATH "assets/MCSM_android_Minecraft102_data.ttarch2") ? 1 : 0);
    }
    return s_ch2_present;
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
     * (game.txt chapter2=off / no CH2 data) return 0 so none of it runs: Episode 2
     * reads as genuinely absent, so the menu neither shows it installed nor lets you
     * "restart" it — the pre-CH2-subsystem behaviour. Set game.txt chapter2=on to
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
#ifdef DEBUG_SOLOADER
        static uint32_t count = 0;
        count++;
        if (count <= 32U) {
            l_info("SAVEFIX2: redirecting %s location '<User>' -> '<Temp>' (#%u)", label, count);
        }
#else
        (void)label;
#endif
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
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
#endif

    /* CH2 mount probe: answer with the engine's REAL state. */
    if (g_ch2_probe_active) {
        return SO_CONTINUE(int, g_hook_lua_resource_set_exists, L);
    }

    const char *name = resource_set_arg_name(L);

    if (resource_set_name_is_vita_ui(name) ||
        resource_set_name_is_other_playstation_ui(name) ||
        resource_set_name_is_xbox_ui(name)) {
        const int value = resource_set_name_is_vita_ui(name) ? 1 : 0;
#ifdef DEBUG_SOLOADER
        count++;
        if (count <= 16U) {
            l_info("FIX: ResourceSetExists('%s') -> %s for controller glyph selection (#%u)",
                   name,
                   value ? "true" : "false",
                   count);
        }
#endif
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
#ifdef DEBUG_SOLOADER
        count++;
        if (count <= 32U) {
            l_info("CH2: ResourceSetExists('%s') -> true for local Episode 2 payload (#%u)",
                   name,
                   count);
        }
#endif
        int ret = lua_push_forced_bool(L, 1, "ResourceSetExists");
        if (ret >= 0) {
            return ret;
        }
    }

    return SO_CONTINUE(int, g_hook_lua_resource_set_exists, L);
}

static so_hook g_hook_lua_resource_set_enabled;
static int hook_lua_resource_set_enabled(void *L) {
#ifdef DEBUG_SOLOADER
    static uint32_t count = 0;
#endif
    const char *name = resource_set_arg_name(L);

    if (resource_set_name_is_vita_ui(name) ||
        resource_set_name_is_other_playstation_ui(name) ||
        resource_set_name_is_xbox_ui(name)) {
        const int value = resource_set_name_is_vita_ui(name) ? 1 : 0;
#ifdef DEBUG_SOLOADER
        count++;
        if (count <= 16U) {
            l_info("FIX: ResourceSetEnabled('%s') -> %s for controller glyph selection (#%u)",
                   name,
                   value ? "true" : "false",
                   count);
        }
#endif
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
#ifdef DEBUG_SOLOADER
        count++;
        if (count <= 32U) {
            l_info("CH2: ResourceSetEnabled('%s') -> true for local Episode 2 payload (#%u)",
                   name,
                   count);
        }
#endif
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
    /* ★ ORDER IS LOAD-BEARING (fixed 2026-07-30). Capture the property-set name
     * BEFORE redirecting, because the redirect rewrites arg1 in place.
     *
     * The character-select Licensed gate below compares arg1 against the literal
     * "user.prop". On 2026-07-24 "user.prop" was added to the bare-name redirect
     * whitelist, so by the time that comparison ran arg1 had already become
     * "logical:<Temp>/user.prop" and it could never match again. The gate has been
     * silently dead since -- PropertyGet("user.prop","Licensed") always returned
     * true during character select, which per the bytecode notes below sends
     * Menu_CharacterSelect_Complete down SubProject_Switch("Menu_Main") instead of
     * Menu_StartEpisode. The safety counter never decremented either, so the armed
     * window was never disarmed. One whitelist entry disabled a whole fix. */
    resolve_lua_str_api();
    const char *pset_pre_redirect =
        (g_lua_type_fast && g_lua_tolstring_fast && g_lua_type_fast(L, 1) == LUA_TSTRING_TAG)
            ? g_lua_tolstring_fast(L, 1, NULL) : NULL;
    const int is_userprop_gate_pre = pset_pre_redirect &&
        (strcmp(pset_pre_redirect, "user.prop") == 0 ||
         strstr(pset_pre_redirect, "/user.prop") != NULL);
    redirect_logical_user_to_temp(L, 1);
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
#ifdef DEBUG_SOLOADER
                static uint32_t count = 0;
                count++;
#endif
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
                /* Use the pre-redirect answer; see the note at the top of this
                 * function. Comparing the post-redirect name here is what broke it. */
                const int is_userprop_gate = is_userprop_gate_pre;
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
#ifdef DEBUG_SOLOADER
                if (count <= 24U || (count & 0x7fU) == 0U) {
                    l_info("FULLGAME: PropertyGet(\"Licensed\") -> %s%s (#%u active=%d safety=%u)",
                           value ? "true" : "false",
                           force_demo_path ? " (character-select window)" : "",
                           count,
                           g_character_select_license_active,
                           g_character_select_license_safety);
                }
#endif
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
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaPlatformIsUserSignedIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "PlatformIsUserSignedIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_user_signed_in, L);
}
static so_hook g_hook_lua_is_user_signed_in;
static int hook_lua_is_user_signed_in(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaIsUserSignedIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsUserSignedIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_user_signed_in, L);
}
/* If the flow still requests a sign-in despite the above, report success
 * immediately instead of waiting for a sign-in UI that never completes. */
static so_hook g_hook_lua_platform_request_sign_in;
static int hook_lua_platform_request_sign_in(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaPlatformRequestSignIn -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "PlatformRequestSignIn");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_request_sign_in, L);
}

/* MyTelltale ACCOUNT (2026-07-23): when the game reports online (needed for the
 * crowd-choice stats), the menu shows a "sign up for full access" banner unless
 * it believes a MyTelltale account is registered. The auth server is dead, so
 * force the registration/online checks TRUE — the game then treats the local
 * offline user as a fully-registered account and drops the banner. Network I/O
 * that would use the account is already bypassed (GetCredential/SessionLog/
 * CloudSync return immediately), so nothing tries to actually reach the server. */
static so_hook g_hook_lua_is_registered;
static int hook_lua_is_registered(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaIsRegistered -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsRegistered");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_registered, L);
}
static so_hook g_hook_lua_platform_is_registered;
static int hook_lua_platform_is_registered(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaPlatformIsRegistered -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "PlatformIsRegistered");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_platform_is_registered, L);
}
static so_hook g_hook_lua_is_user_online;
static int hook_lua_is_user_online(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaIsUserOnline -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsUserOnline");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_user_online, L);
}

/* Prompt/platform identity: the Android binary contains the official Vita Lua
 * platform probe, but it returns false by default. Make scripts that branch on
 * Vita see the native handheld identity without changing TTPlatform's platform
 * enum, which still controls Android resource loading. */
static so_hook g_hook_lua_is_engine_vita;
static int hook_lua_is_engine_vita(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsEngineVita -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "IsEngineVita");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_vita, L);
}

static so_hook g_hook_lua_is_joystick_xbox;
static int hook_lua_is_joystick_xbox(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsJoystickXbox -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsJoystickXbox");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_joystick_xbox, L);
}

static so_hook g_hook_lua_is_engine_xbox360;
static int hook_lua_is_engine_xbox360(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsEngineXbox360 -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEngineXbox360");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_xbox360, L);
}

static so_hook g_hook_lua_is_engine_xbone;
static int hook_lua_is_engine_xbone(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsEngineXBOne -> false (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEngineXBOne");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_xbone, L);
}

static so_hook g_hook_lua_is_engine_ps3;
static int hook_lua_is_engine_ps3(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsEnginePS3 -> false (Vita-only device identity) (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEnginePS3");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_ps3, L);
}

static so_hook g_hook_lua_is_engine_ps4;
static int hook_lua_is_engine_ps4(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaIsEnginePS4 -> false (Vita-only device identity) (#%u)", count);
    int ret = lua_push_forced_bool(L, 0, "IsEnginePS4");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_is_engine_ps4, L);
}

static so_hook g_hook_lua_input_has_joystick;
static int hook_lua_input_has_joystick(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaInputHasJoystick -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputHasJoystick");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_has_joystick, L);
}

static so_hook g_hook_lua_input_supports_joystick;
static int hook_lua_input_supports_joystick(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaInputSupportsJoystick -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputSupportsJoystick");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_supports_joystick, L);
}

static so_hook g_hook_lua_input_is_joystick_enabled;
static int hook_lua_input_is_joystick_enabled(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 12U) l_info("FIX: luaInputIsJoystickEnabled -> true (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputIsJoystickEnabled");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_is_joystick_enabled, L);
}

static so_hook g_hook_lua_input_has_touch;
static int hook_lua_input_has_touch(void *L) {
    MCSM_DIAG_COUNTER(count);
    if (count <= 8U) l_info("FIX: luaInputHasTouch -> true while controller is active (#%u)", count);
    int ret = lua_push_forced_bool(L, 1, "InputHasTouch");
    return ret >= 0 ? ret : SO_CONTINUE(int, g_hook_lua_input_has_touch, L);
}

static so_hook g_hook_lua_input_supports_touch;
static int hook_lua_input_supports_touch(void *L) {
    MCSM_DIAG_COUNTER(count);
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
#ifdef DEBUG_SOLOADER
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
    l_info("Diag: GameEngine::Initialize2 ENTER arg=%p", arg);
    int ret = SO_CONTINUE(int, g_hook_ge_init2, arg);
    l_info("Diag: GameEngine::Initialize2 RETURNED %d (0 => _boot.lua will NOT load)", ret);
    return ret;
}

static int hook_sm_load(void *str, int b) {
    static uint32_t count = 0;
    count++;
    l_info("Diag: ScriptManager::Load ENTER #%u (boot script load reached!) b=%d", count, b);
    int ret = SO_CONTINUE(int, g_hook_sm_load, str, b);
    l_info("Diag: ScriptManager::Load RETURNED %d", ret);
    return ret;
}

static int hook_sm_doload(void *str) {
    static uint32_t count = 0;
    count++;
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

static int hook_sm_loadresource(void *L, const char *name) {
    static uint32_t count = 0;
    static uint32_t dlc_load_counts[3];
    count++;

    const int dlc_index = dlc_resource_index(name);

    launch_state_mark_progress();
    int ret = SO_CONTINUE(int, g_hook_sm_loadresource, L, name);
    launch_state_mark_progress();
    if (dlc_index >= 0) {
        dlc_load_counts[dlc_index]++;
    }
    return ret;
}

static int hook_job_init(void) {
    static uint32_t count = 0;
    count++;
    launch_state_mark_progress();
    return SO_CONTINUE(int, g_hook_job_init);
}
#endif /* DEBUG_SOLOADER */


/* ---- SHADOW DISABLE (perf) --------------------------------------------------
 * Shadows render the scene geometry a SECOND time (a depth pass) + cost CPU in
 * Scene::PrepareToRenderShadows — a big chunk of the 488k-vert/100ms-CPU heavy
 * frames. Hooking LightInstance::IsShadowLight / IsContributingShadowLight to
 * report FALSE makes the engine treat no light as a shadow caster, so it skips
 * the whole shadow setup + pass. Controlled by settings/graphics.txt `shadows` (visual
 * trade: objects lose cast shadows). */
static int shadows_disabled(void) { return !mcsm_cfg()->shadows; }
static so_hook g_hook_is_shadow_light;
static int hook_is_shadow_light(void *self) {
    (void)self;
    return 0; /* installed only when shadows_disabled() was latched true */
}
static so_hook g_hook_is_contributing_shadow;
static int hook_is_contributing_shadow(void *self) {
    (void)self;
    return 0; /* installed only when shadows_disabled() was latched true */
}

/* POST-PROCESS AUDIT (2026-07-29). libGameEngine exports a full post-effect chain
 * -- FX_Glow with four blur levels, FX_DepthOfField with H/V blur passes, FX_FXAA,
 * FX_MotionBlur -- plus HDR/RGBM/GBuffer/quarter-res render targets. NOTHING in the
 * loader touches any of it, so all of it runs at whatever the engine chose at boot.
 * These are full-screen passes: cheap in draw calls, expensive in pixels, and the
 * frame is now GPU-bound, so they are the largest unexamined cost left.
 *
 * Before disabling anything we need to know what actually runs. This is REPORT-ONLY
 * on purpose: C++ mangling does not encode return types, so for DrawGlow and
 * _UpdateMotionBlur we cannot tell void from non-void, and returning the wrong thing
 * from a skip path would corrupt r0. RenderConfiguration::TestFeature is the one
 * safe gate -- a Test* predicate is unambiguously boolean -- and it is the single
 * point the renderer asks "is feature N enabled?", so logging its arguments gives us
 * the exact RenderFeatureType ids to target, with zero behaviour change now. */

/* REMOVED 2026-07-29 -- THIS HOOK WAS THE DIORAMA CRASH.
 *
 * RenderConfiguration::TestFeature was hooked in r39 for the post-effect audit.
 * The crash dump resolves the faulting PC to RenderFrame::RenderFrame +0x60,
 * which disassembles to the instruction immediately AFTER
 *     bl <RenderConfiguration::TestFeature>
 * i.e. threads were faulting on the way back out of this exact function, in both
 * captured MCSM dumps, with the same crashed-thread marker.
 *
 * The mechanism is SO_CONTINUE itself: it UNPATCHES the target, calls the
 * original, then RE-PATCHES it, rewriting live instruction bytes on every single
 * invocation. That is neither reentrant nor thread-safe. TestFeature is called
 * from the render path -- the audit it enabled logged 5066 calls in one session --
 * and this game has multiple render threads. When two overlap, one rewrites the
 * instructions the other is executing.
 *
 * It was report-only and it already gave its answer (RenderFeature[0] answered
 * false 5066/5066, nothing to disable), so it is pure liability now. The lesson
 * generalises: SO_CONTINUE hooks are only safe on functions that are called
 * rarely and from one thread. Never put one on a hot render-path function. */


/* LinearHeap instrumentation REMOVED 2026-07-29 (added and withdrawn same day).
 * It hooked _AllocatePage with SO_CONTINUE -- the identical unpatch/call/repatch
 * of live instruction bytes that had just been identified as the diorama crash,
 * on a function this file's own comment placed inside RenderFrame::RenderFrame,
 * the faulting frame. Adding it directly contradicted the rule written 30 lines
 * above it, and _AllocatePage is called far more often than TestFeature was.
 *
 * It had also already answered its question and found nothing: pages=34,
 * live_bytes=3017, fails=0 -- a 3KB heap that never fails. Exhaustion was not the
 * crash. Keeping a probe with a known-fatal mechanism to re-measure a settled
 * negative is not a trade worth making. The counters were global across every
 * LinearHeap instance anyway, so they could not have answered it properly. */


/* Scene FX levers REMOVED 2026-07-29 (added and withdrawn same day). Three
 * reasons, any one sufficient:
 *
 *  1. Three of the six mangled names carried the wrong Itanium length prefix
 *     (26/27/25 where libGameEngine.so has 27/28/24), so AmbientOcclusion,
 *     EnvReflections and VignetteTint never bound at all. The report then printed
 *     "AO 0/0 Refl 0/0" -- which reads as "the scene never asked for it", the
 *     exact false negative the audit existed to avoid.
 *  2. The three that DID bind measured the levers as near-worthless: Spec 0/9,
 *     DOF 1/9, Tonemap 2/9. The engine had already declined them, same as the
 *     post-effect chain before it.
 *  3. They used SO_CONTINUE_VOID on engine setters -- the mechanism that caused
 *     the diorama crash. Carrying that risk for a lever worth ~1 effect in 9 is
 *     not a trade worth making. */


static void patch_boot_diag_hooks(void) {
    if (shadows_disabled()) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13LightInstance13IsShadowLightEv",
                                  "LightInstance::IsShadowLight",
                                  (uintptr_t)&hook_is_shadow_light, &g_hook_is_shadow_light);
        (void)hook_symbol_checked(&so_mod_gameengine, "_ZN13LightInstance25IsContributingShadowLightEv",
                                  "LightInstance::IsContributingShadowLight",
                                  (uintptr_t)&hook_is_contributing_shadow, &g_hook_is_contributing_shadow);
        l_info("PERF: shadows DISABLED (graphics.txt shadows=off) — shadow geometry pass skipped.");
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
#ifdef DEBUG_SOLOADER
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
#endif
    if (keep_resident_opt_in()) {
        (void)hook_symbol_checked(&so_mod_gameengine, "_Z21luaResourceSetDisableP9lua_State",
                              "luaResourceSetDisable",
                              (uintptr_t)&hook_lua_resource_set_disable,
                              &g_hook_lua_resource_set_disable);
    }
#ifdef DEBUG_SOLOADER
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
#endif
}

/* SAVE-RENAME KEYBOARD (2026-07-18): the game calls luaPlatformShowKeyboard when
 * it wants text entry (renaming a save). On Android that pops the OS soft keyboard;
 * on Vita it did nothing. Hook it to raise the Vita IME (dialog.c mcsm_ime_begin);
 * gl_swap pumps it and feeds the typed name back to the engine. */
static so_hook g_hook_lua_platform_show_keyboard;
/* Log-only for the same reason as hook_sdl_show_text_input above. */
static int hook_lua_platform_show_keyboard(void *L) {
    l_info("KEYBOARD: luaPlatformShowKeyboard fired (not raising an IME — the JNI "
           "text-dialog path owns text entry)");
    return SO_CONTINUE(int, g_hook_lua_platform_show_keyboard, L);
}

/* ENGINE VIRTUAL-KEYBOARD BRIDGE (2026-07-20). TTPlatform::{Open,IsFinished,GetResult}
 * VirtualKeyboard are vtable STUBS (Open=nop, IsFinished=return 1, GetResult=nop —
 * confirmed via the TTPlatform vtables in libGameEngine .data.rel.ro), so the engine's
 * rename flow instantly "finished" with an empty name. Replace them with a bridge to
 * the real Vita IME (dialog.c). We DON'T call the originals (they do nothing). */
extern void mcsm_ime_begin_vkbd(const char *title, const char *initial);
extern int mcsm_vkbd_finished(void);
extern const char *mcsm_vkbd_result(int *cancelled);
extern void mcsm_vkbd_reset(void);
static so_hook g_hook_open_vkbd, g_hook_is_vkbd_finished, g_hook_get_vkbd_result;

/* OpenVirtualKeyboard(String const& title, String const& initial, bool, int, bool).
 * We ignore the engine args (no need to read the Telltale String layout) and raise
 * an empty IME. Return 1 in case the caller checks a success bool. */
static int hook_open_virtual_keyboard(void) {
    l_info("KEYBOARD: TTPlatform::OpenVirtualKeyboard -> Vita IME");
    mcsm_ime_begin_vkbd(NULL, "");
    return 1;
}
/* IsVirtualKeyboardFinished() -> 0 while the IME is up, 1 when the user confirms/
 * cancels (the stub always returned 1 so the engine never waited for input). */
static int hook_is_virtual_keyboard_finished(void) {
    /* ONE-SHOT PROBE. If the rename screen polls this but never calls Open, the engine
     * uses the vkbd interface but enters it somewhere we have not hooked. If NEITHER
     * ever fires, the rename does not use this interface at all and the whole vtable
     * bridge is aimed at the wrong thing -- which is the difference between "nearly
     * working" and "wrong tree", and no other line distinguishes them. */
#ifdef DEBUG_SOLOADER
    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        l_info("KEYBOARD: engine polled IsVirtualKeyboardFinished (vkbd interface IS "
                "in use; finished=%d)", mcsm_vkbd_finished());
    }
#endif
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
/* ★★ INSTALL THE VKBD BRIDGE BY PATCHING THE VTABLES, NOT BY HOOKING THE FUNCTIONS.
 *
 * The inline hooks below CANNOT install for two of the three. hook_symbol_checked()
 * refuses any function smaller than INLINE_HOOK_BYTES (8), because an inline hook
 * writes 8 bytes and a shorter function would have its successor clobbered. Measured
 * in the shipped libGameEngine.so:
 *     OpenVirtualKeyboard        @0x56df2c  4 bytes  (bx lr)              -> SKIPPED
 *     IsVirtualKeyboardFinished  @0x56df30  8 bytes  (mov r0,#1; bx lr)   -> hooked
 *     GetVirtualKeyboardResult   @0x56df38  4 bytes  (bx lr)              -> SKIPPED
 * and the device log said so plainly every boot:
 *     [WARN] Patch: skipping hook for TTPlatform::OpenVirtualKeyboard ... too small
 * So the bridge has never actually been installed -- only its middle third was, which
 * is worse than none (see mcsm_vkbd_finished()). Every previous "rename still does
 * nothing" report is explained by this line.
 *
 * These stubs are reached through a VTABLE, so the fix is to replace the vtable slots
 * instead: no code is written, no size limit applies, and neighbouring functions
 * cannot be clobbered. Both TTPlatform vtables in .data.rel.ro hold the three stubs in
 * consecutive slots. Static vaddrs (file offset + 0x1000, per the section headers):
 *     vtable A: 0xff0f60   vtable B: 0xff1360     <- address of the Open slot
 *
 * Verify-before-write, the same discipline patch_u32() uses: if all three slots do not
 * still hold the exact stub addresses, the binary is not the one this was measured
 * against and we leave it alone rather than corrupt a vtable. */
static void install_vkbd_vtable_bridge(void) {
    const uintptr_t base = so_mod_gameengine.text_base;
    const uintptr_t expect[3] = { base + 0x56df2cu,   /* Open      */
                                  base + 0x56df30u,   /* IsFinished*/
                                  base + 0x56df38u }; /* GetResult */
    const uintptr_t repl[3]   = { (uintptr_t)&hook_open_virtual_keyboard,
                                  (uintptr_t)&hook_is_virtual_keyboard_finished,
                                  (uintptr_t)&hook_get_virtual_keyboard_result };
    static const uint32_t kVtabOpenSlot[] = { 0x00ff0f60u, 0x00ff1360u };

    int patched = 0;
    for (unsigned i = 0; i < sizeof(kVtabOpenSlot) / sizeof(kVtabOpenSlot[0]); i++) {
        const uintptr_t slot = base + kVtabOpenSlot[i];
        const volatile uintptr_t *cur = (const volatile uintptr_t *)slot;
        if (cur[0] != expect[0] || cur[1] != expect[1] || cur[2] != expect[2]) {
            l_warn("KEYBOARD: vtable %u at %p does not hold the expected vkbd stubs "
                   "(%08X %08X %08X, wanted %08X %08X %08X) — not patching.",
                   i, (void *)slot,
                   (unsigned)cur[0], (unsigned)cur[1], (unsigned)cur[2],
                   (unsigned)expect[0], (unsigned)expect[1], (unsigned)expect[2]);
            continue;
        }
        kuKernelCpuUnrestrictedMemcpy((void *)slot, repl, sizeof(repl));
        kuKernelFlushCaches((void *)slot, sizeof(repl));
        patched++;
        l_info("KEYBOARD: vkbd vtable %u at %p -> bridge installed.", i, (void *)slot);
    }
    if (!patched) {
        l_warn("KEYBOARD: NO vkbd vtable was patched — save rename will not open the "
               "IME. Check the stub addresses against this libGameEngine.so.");
    } else {
        l_info("KEYBOARD: vkbd bridge live in %d vtable(s); rename should raise the "
               "Vita IME.", patched);
    }
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
#ifdef DEBUG_SOLOADER
static so_hook g_hook_lua_saveload_set_display_name;
static int hook_lua_saveload_set_display_name(void *L) {
    return SO_CONTINUE(int, g_hook_lua_saveload_set_display_name, L);
}
#endif /* DEBUG_SOLOADER */

static void patch_dlc_fast_path_hooks(void) {
#ifdef DEBUG_SOLOADER
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z29luaSaveLoadSetSaveDisplayNameP9lua_State",
                              "luaSaveLoadSetSaveDisplayName",
                              (uintptr_t)&hook_lua_saveload_set_display_name,
                              &g_hook_lua_saveload_set_display_name);
#endif
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
    /* The three calls above are kept for any DIRECT (non-virtual) call site, but two
     * of them cannot install -- the functions are 4 bytes. The vtable patch is what
     * actually makes the bridge live; see install_vkbd_vtable_bridge(). */
    install_vkbd_vtable_bridge();
#ifdef DEBUG_SOLOADER
    const int install_gpu_quality_hook = 1;
#else
    const int install_gpu_quality_hook = mcsm_cfg()->gpu_tier >= 0;
#endif
    if (install_gpu_quality_hook) {
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_Z24luaPlatformGetGPUQualityP9lua_State",
                                  "luaPlatformGetGPUQuality",
                                  (uintptr_t)&hook_lua_platform_get_gpu_quality,
                                  &g_hook_lua_platform_get_gpu_quality);
    }
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
#ifdef DEBUG_SOLOADER
    const int install_language_hook = 1;
#else
    const int install_language_hook = mcsm_game()->language[0] != '\0';
#endif
    if (install_language_hook) {
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_Z24luaGetUserSystemLanguageP9lua_State",
                                  "luaGetUserSystemLanguage",
                                  (uintptr_t)&hook_lua_get_user_system_language,
                                  &g_hook_lua_get_user_system_language);
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_Z21luaLangSetCurLanguageP9lua_State",
                                  "luaLangSetCurLanguage",
                                  (uintptr_t)&hook_lua_lang_set_cur_language,
                                  &g_hook_lua_lang_set_cur_language);
    }
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
    /* The write side of the same redirect -- see the note above the macro. */
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z14luaPropertySetP9lua_State",
                              "luaPropertySet",
                              (uintptr_t)&hook_lua_property_set, &g_hook_lua_property_set);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z17luaPropertyCreateP9lua_State",
                              "luaPropertyCreate",
                              (uintptr_t)&hook_lua_property_create, &g_hook_lua_property_create);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z17luaPropertyRemoveP9lua_State",
                              "luaPropertyRemove",
                              (uintptr_t)&hook_lua_property_remove, &g_hook_lua_property_remove);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z20luaPropertyClearKeysP9lua_State",
                              "luaPropertyClearKeys",
                              (uintptr_t)&hook_lua_property_clearkeys, &g_hook_lua_property_clearkeys);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z17luaPropertyExistsP9lua_State",
                              "luaPropertyExists",
                              (uintptr_t)&hook_lua_property_exists,
                              &g_hook_lua_property_exists);
#ifdef DEBUG_SOLOADER
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z16luaSetSubProjectP9lua_State",
                              "luaSetSubProject",
                              (uintptr_t)&hook_lua_set_subproject,
                              &g_hook_lua_set_subproject);
#endif
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z12luaResetGameP9lua_State",
                              "luaResetGame",
                              (uintptr_t)&hook_lua_reset_game,
                              &g_hook_lua_reset_game);
#ifdef DEBUG_SOLOADER
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
#endif
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
                              "_Z12luaSavePrefsP9lua_State",
                              "luaSavePrefs",
                              (uintptr_t)&hook_lua_save_prefs,
                              &g_hook_lua_save_prefs);
#ifdef DEBUG_SOLOADER
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
                              "_Z20luaResourceSetEnableP9lua_State",
                              "luaResourceSetEnable",
                              (uintptr_t)&hook_lua_resource_set_enable,
                              &g_hook_lua_resource_set_enable);
#endif
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
                              "_Z15luaIsRegisteredP9lua_State",
                              "luaIsRegistered",
                              (uintptr_t)&hook_lua_is_registered,
                              &g_hook_lua_is_registered);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z23luaPlatformIsRegisteredP9lua_State",
                              "luaPlatformIsRegistered",
                              (uintptr_t)&hook_lua_platform_is_registered,
                              &g_hook_lua_platform_is_registered);
    (void)hook_symbol_checked(&so_mod_gameengine,
                              "_Z15luaIsUserOnlineP9lua_State",
                              "luaIsUserOnline",
                              (uintptr_t)&hook_lua_is_user_online,
                              &g_hook_lua_is_user_online);
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

#ifdef DEBUG_SOLOADER
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
#endif
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
#ifdef DEBUG_SOLOADER
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z12luaSceneOpenP9lua_State",
                              "luaSceneOpen",
                              (uintptr_t)&hook_lua_sceneopen, &g_hook_lua_sceneopen);
#endif
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z7luaLoadP9lua_State",
                              "luaLoad",
                              (uintptr_t)&hook_lua_load, &g_hook_lua_load);
#ifdef DEBUG_SOLOADER
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z19luaResourceIsLoadedP9lua_State",
                              "luaResourceIsLoaded",
                              (uintptr_t)&hook_lua_resource_is_loaded, &g_hook_lua_resource_is_loaded);
#endif
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z15luaScenePreloadP9lua_State",
                              "luaScenePreload",
                              (uintptr_t)&hook_lua_scene_preload, &g_hook_lua_scene_preload);
#ifdef DEBUG_SOLOADER
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z30luaResourceAdvancePreloadBatchP9lua_State",
                              "luaResourceAdvancePreloadBatch",
                              (uintptr_t)&hook_lua_advance_preload, &g_hook_lua_advance_preload);
    (void)hook_symbol_checked(&so_mod_gameengine, "_Z19luaWaitForCallbacksP9lua_State",
                              "luaWaitForCallbacks",
                              (uintptr_t)&hook_lua_wait_for_callbacks, &g_hook_lua_wait_for_callbacks);
#endif
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
 *   (2) OUTLINE pass — RenderObject_Mesh::SetRenderToonOutline(bool). Forcing
 *       it off skips the per-mesh outline submit. Opt-in: settings/graphics.txt (outlines).
 * Both alter visuals, so both are OFF by default — zero effect on the shader-key-fix
 * validation; enable to A/B toward reliable 30fps. */
static int render_quality_override(void) {
    static int v = -2;   /* -2 unread, -1 no override, >=0 forced level */
    if (v == -2) {
        v = -1;
        /* MCSM boots at q=15 (logged) — outside the 0-4 desktop enum, so its mobile
         * build uses a wider scale. Accept 0..15; the engine already runs 15 without
         * crashing, so any value in range is safe to probe (revert = delete the file). */
        /* Consolidated into settings/graphics.txt (`render_quality`); -1 = leave
         * the engine's own value (it boots at 15). */
        const int q = mcsm_cfg()->render_quality;
        if (q >= 0 && q <= 15) v = q;
    }
    return v;
}
static so_hook g_hook_set_render_quality;
static void hook_set_render_quality(void *self, int quality) {
    const int ov = render_quality_override();
#ifdef DEBUG_SOLOADER
    static unsigned n = 0;
    if (n++ < 8U) l_info("RQUAL: RenderConfiguration::SetQuality(q=%d) override=%d", quality, ov);
#endif
    if (ov >= 0) quality = ov;
    SO_CONTINUE_VOID(g_hook_set_render_quality, self, quality);
}

static int outlines_disabled(void) { return !mcsm_cfg()->outlines; }
static so_hook g_hook_set_toon_outline;
static uintptr_t g_set_toon_outline_tramp;
static void hook_set_toon_outline(void *self, int on) {
    (void)on;
    /* Installed only for outlines=off, so avoid a cached-config read per mesh. */
    if (g_set_toon_outline_tramp) {
        ((void (*)(void *, int))g_set_toon_outline_tramp)(self, 0);
    } else {
        SO_CONTINUE_VOID(g_hook_set_toon_outline, self, 0);
    }
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
static int unsafe_render_hooks_enabled(void);   /* defined below with the rationale */
static float detail_scale(void) {
    if (!unsafe_render_hooks_enabled()) return -1.0f;   /* see the note above */
    /* Consolidated into settings/graphics.txt (`detail`, stored x1000). Returns
     * -1 for "no override" so the hooks stay uninstalled at the default, exactly
     * as the stray-file version did -- the default build is unchanged. */
    const int d = mcsm_cfg()->detail;
    if (d <= 0 || d >= 1000) return -1.0f;
    return (float)d / 1000.0f;
}
static so_hook g_hook_scene_far_detail, g_hook_scene_near_detail;
static uintptr_t g_hook_scene_far_detail_tramp, g_hook_scene_near_detail_tramp;
/* HISTORY, and why these three hooks are shaped the way they are.
 *
 * They CRASHED the render thread until 2026-07-30. The macro below was named
 * "trampoline" but was not one: it fixed the FLOAT argument (via a correctly-typed
 * function pointer, which SO_CONTINUE genuinely does corrupt) and then did the
 * exact thing that caused the diorama crash -- unpatch the live function, call it,
 * re-patch it, on EVERY invocation.
 *
 * Crash dump proof, kept because it is the evidence that shaped the fix. Faulting
 * thread marker (+0x70 = 0x00030003) points at the render thread; its PC resolves
 * to RenderFrameScene::AllocateView(RenderViewParams const&) +0x25c, which
 * disassembles to the instruction immediately AFTER `bl Camera::SetFarClip` -- a
 * thread faulting on the way back out of the hooked function, the identical
 * signature that identified RenderConfiguration::TestFeature as the previous
 * crash. AllocateView runs per view per frame and this game has more than one
 * render thread, so two overlapped and one rewrote the bytes the other was
 * executing.
 *
 * ★ FIXED, and therefore ON BY DEFAULT NOW: mcsm_build_tramp() builds a real
 * cave trampoline ONCE at install time (stolen instructions + LDR PC,[PC,#-4] +
 * resume address). The live function keeps its patch permanently, nothing is ever
 * rewritten, and concurrent callers cannot collide. mcsm_insn_is_relocatable()
 * enforces the precondition that relocating the stolen instructions is legal,
 * rather than trusting a one-time manual audit, and mcsm_install_tramp_hook()
 * restores the original bytes if a trampoline cannot be built -- so a refusal
 * costs the lever, never the engine's own call.
 *
 * That matters because draw_distance and detail are the only runtime levers that
 * attack the vertex count, and disabling them measured 341k verts / 21 fps.
 * no_render_hooks.txt turns them back off if anything regresses. */
static int unsafe_render_hooks_enabled(void) {
    /* Back ON by default: these no longer rewrite live code. mcsm_build_tramp()
     * gives each hook a real cave trampoline, so the crash mechanism is gone and
     * the vertex levers (draw_distance, detail) are usable again -- which matters,
     * because heavy scenes measured 341k verts / 21 fps with them disabled.
     * no_render_hooks.txt turns them back off if anything regresses. */
    static int s_on = -1;
    if (s_on < 0) {
        s_on = mcsm_cfg()->render_hooks ? 1 : 0;
        if (!s_on) l_info("PERF: far-clip/detail hooks disabled (graphics.txt render_hooks=off)");
    }
    return s_on;
}

/* A REAL trampoline, built ONCE at install time (2026-07-30).
 *
 * What was here rewrote the live function on EVERY call -- unpatch, call,
 * re-patch -- which is the mechanism that crashed the render thread. Crash dump:
 * the faulting PC was RenderFrameScene::AllocateView +0x25c, which disassembles
 * to the instruction immediately after `bl Camera::SetFarClip`, i.e. a thread
 * faulting on the way back out while another rewrote those same bytes.
 *
 * This builds the standard thing instead: copy the two stolen instructions into
 * so_util's code cave, append LDR PC,[PC,#-4] + the resume address, call THAT.
 * The live function keeps its patch permanently, nothing is ever rewritten, and
 * concurrent callers cannot collide. Same shape as so_util's own trampoline_ldm.
 *
 * Only safe because the stolen instructions are position-independent -- checked
 * against the binary before writing this:
 *   Camera::SetFarClip         vldr s14,[r0,#392] ; vmov s15,r1   register-relative
 *   Scene::SetBrushFarDetail   str r1,[r0,#..]    ; bx lr         8-byte function --
 *   Scene::SetBrushNearDetail  str r1,[r0,#..]    ; bx lr         bx returns before
 *                                                                 the LDR, so the
 *                                                                 tail is dead code
 * No PC-relative loads in any of them. Float typing is preserved, which is why
 * these needed their own path rather than SO_CONTINUE (which corrupts floats). */
/* so_util.c defines this but does not export it in the header. */
extern uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

/* Metrics::NewFrame's shipped prologue is almost relocatable, but its second
 * instruction is a PC-relative literal load:
 *   e92d43f0  push {r4-r9,lr}
 *   e59f43d8  ldr  r4,[pc,#0x3d8]
 * A blind cave copy would load from the cave's PC and corrupt the GOT base used by
 * the rest of the function. Build an exact-prologue trampoline that embeds the
 * original literal beside the resume address and rewrites only that load. Any
 * binary drift falls back to the slower live-unpatch path in the handler. */
static uintptr_t mcsm_build_metrics_new_frame_tramp(so_hook *h) {
    if (!h || !h->addr || h->thumb_addr ||
        h->orig_instr[0] != 0xE92D43F0u ||
        h->orig_instr[1] != 0xE59F43D8u) {
        return 0;
    }

    /* The LDR is the second instruction: ARM PC = instruction address + 8. */
    const uintptr_t literal_addr = h->addr + 12u + 0x3D8u;
    uint32_t literal_value = 0;
    kuKernelCpuUnrestrictedMemcpy(&literal_value,
                                  (const void *)literal_addr,
                                  sizeof(literal_value));

    uintptr_t t = so_alloc_arena(&so_mod_gameengine, 0, h->addr, 20);
    if (!t) {
        return 0;
    }
    const uint32_t code[5] = {
        0xE92D43F0u,              /* original push */
        0xE59F4004u,              /* ldr r4,[pc,#4] -> embedded literal below */
        0xE51FF004u,              /* ldr pc,[pc,#-4] -> resume word */
        (uint32_t)(h->addr + 8u),
        literal_value
    };
    kuKernelCpuUnrestrictedMemcpy((void *)t, code, sizeof(code));
    kuKernelFlushCaches((void *)t, sizeof(code));
    l_info("TRAMP: Metrics::NewFrame @0x%08X -> literal-safe cave 0x%08X",
           (unsigned)h->addr, (unsigned)t);
    return t;
}

/* Is this ARM instruction safe to EXECUTE FROM A DIFFERENT ADDRESS?
 *
 * mcsm_build_tramp copies the two stolen instructions into a code cave and runs
 * them there, so anything whose behaviour depends on its own address changes
 * meaning. The rule used to be "checked against the binary before writing this"
 * -- a one-time manual audit recorded in a comment, with nothing enforcing it.
 * That is a trap rather than a safeguard: RenderConfiguration::SetQuality, the
 * next hook in this very file and the obvious candidate for migrating off
 * SO_CONTINUE, begins
 *     ldr r3, [pc, #32]
 *     add r3, pc, r3
 * which is the standard GOT-base idiom. Relocated into the cave, that computes a
 * GOT base from the WRONG pc and every subsequent global access reads garbage --
 * silently, with no crash at the point of the mistake.
 *
 * Conservative: reject anything that reads or writes pc, or branches relatively.
 * Rejecting a safe instruction only costs us a lever; accepting an unsafe one
 * corrupts the renderer.
 * Verified against the three current users, all of which pass:
 *   Camera::SetFarClip          vldr s14,[r0,#392] / vmov s15,r1
 *   Scene::SetBrushFarDetail    str r1,[r0,#700]   / bx lr
 *   Scene::SetBrushNearDetail   str r1,[r0,#692]   / bx lr */
static int mcsm_insn_is_relocatable(uint32_t insn) {
    /* BX/BLX <register> encode Rn=Rd=0xF as should-be-one bits, so they trip the
     * pc checks below despite being fully position-independent. Allow explicitly. */
    const uint32_t noncond = insn & 0x0FFFFFF0u;
    if (noncond == 0x012FFF10u || noncond == 0x012FFF30u) return 1;  /* BX / BLX reg */

    const uint32_t op = (insn >> 25) & 0x7u;
    if (op == 0x5u) return 0;                       /* B / BL: pc-relative target  */
    if (((insn >> 25) & 0x7Fu) == 0x7Du) return 0;  /* BLX immediate               */
    if (((insn >> 24) & 0xFu) == 0xFu)   return 0;  /* SVC                         */

    /* Rn == pc for data-processing (00x), load/store (01x), block transfer (100),
     * or coprocessor load/store (110) means the address is the operand. */
    const uint32_t rn = (insn >> 16) & 0xFu;
    if (rn == 0xFu && (op <= 0x3u || op == 0x4u || op == 0x6u)) return 0;

    /* Writing pc is a branch by another name. */
    const uint32_t rd = (insn >> 12) & 0xFu;
    if (rd == 0xFu && op <= 0x3u) return 0;

    return 1;
}

static uintptr_t mcsm_build_tramp(so_hook *h) {
    if (!h || !h->addr) return 0;
    /* The cave sequence below copies two 32-bit ARM instructions. Thumb targets
     * need instruction-width decoding and a Thumb-state resume branch; fall back
     * to SO_CONTINUE instead of pretending this ARM-only builder can handle them. */
    if (h->thumb_addr) {
        l_error("TRAMP: REFUSING Thumb hook @0x%08X (ARM cave builder only)",
                (unsigned)h->addr);
        return 0;
    }
    /* Refuse rather than relocate something position-dependent. The caller must
     * then leave the function UNHOOKED -- swallowing the call would be worse. */
    for (int i = 0; i < 2; ++i) {
        if (!mcsm_insn_is_relocatable(h->orig_instr[i])) {
            l_error("TRAMP: REFUSING hook @0x%08X — stolen insn[%d]=0x%08X is "
                    "position-DEPENDENT (reads/writes pc or branches relatively); "
                    "copying it into the cave would compute from the wrong address",
                    (unsigned)h->addr, i, (unsigned)h->orig_instr[i]);
            return 0;
        }
    }
    uintptr_t t = so_alloc_arena(&so_mod_gameengine, 0, h->addr, 16);
    if (!t) { l_error("TRAMP: no cave space for hook @0x%08X", (unsigned)h->addr); return 0; }
    uint32_t code[4];
    code[0] = h->orig_instr[0];
    code[1] = h->orig_instr[1];
    code[2] = 0xE51FF004u;              /* LDR PC, [PC, #-4] */
    code[3] = (uint32_t)(h->addr + 8);  /* resume just past the patched bytes */
    kuKernelCpuUnrestrictedMemcpy((void *)t, code, sizeof(code));
    kuKernelFlushCaches((void *)t, sizeof(code));
    l_info("TRAMP: hook @0x%08X -> cave 0x%08X (no live-code rewriting)",
           (unsigned)h->addr, (unsigned)t);
    return t;
}

/* Install a hook AND its trampoline as one unit, or leave the function alone.
 *
 * hook_symbol_checked patches the live function immediately, and the trampoline
 * was built afterwards -- so any failure to build one (no cave space, or now a
 * position-dependent stolen instruction) left the function patched with a NULL
 * trampoline. MCSM_DETAIL_TRAMPOLINE then quietly does nothing, which does not
 * merely disable the lever: it SWALLOWS the call. Camera::SetFarClip never
 * running means the camera's far plane is never set at all, which is far worse
 * than not having the lever in the first place. Restore the original bytes so the
 * engine's own function runs unhooked. */
static int mcsm_install_tramp_hook(const char *symbol, const char *label,
                                   uintptr_t handler, so_hook *h, uintptr_t *tramp_out) {
    *tramp_out = 0;
    if (!hook_symbol_checked(&so_mod_gameengine, symbol, label, handler, h)) return 0;
    uintptr_t t = mcsm_build_tramp(h);
    if (!t) {
        kuKernelCpuUnrestrictedMemcpy((void *)h->addr, h->orig_instr, sizeof(h->orig_instr));
        kuKernelFlushCaches((void *)h->addr, sizeof(h->orig_instr));
        l_error("TRAMP: %s left UNHOOKED (original instructions restored) — the "
                "lever is off, but the engine's own call still runs", label);
        return 0;
    }
    *tramp_out = t;
    return 1;
}

#define MCSM_DETAIL_TRAMPOLINE(H, SELF, V) do {     uintptr_t t_ = (H##_tramp);     if (t_) ((void (*)(void *, float))t_)((SELF), (V)); } while (0)
static void hook_scene_far_detail(void *self, float v) {
    const float s = detail_scale();
#ifdef DEBUG_SOLOADER
    static unsigned n = 0;
    if (n++ < 8U) l_info("DETAIL: Scene::SetBrushFarDetail=%d/1000 scale=%d/1000", (int)(v * 1000.0f), (int)(s * 1000.0f));
#endif
    if (s > 0.0f) v *= s;
    MCSM_DETAIL_TRAMPOLINE(g_hook_scene_far_detail, self, v);
}
static void hook_scene_near_detail(void *self, float v) {
    const float s = detail_scale();
#ifdef DEBUG_SOLOADER
    static unsigned n = 0;
    if (n++ < 8U) l_info("DETAIL: Scene::SetBrushNearDetail=%d/1000 scale=%d/1000", (int)(v * 1000.0f), (int)(s * 1000.0f));
#endif
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
    if (!unsafe_render_hooks_enabled()) return -1.0f;   /* see the note above */
    int d = mcsm_cfg()->draw_distance;   /* 0 = engine default (no clamp) */
    return d > 0 ? (float)d : -1.0f;
}
static so_hook g_hook_camera_far_clip;
static uintptr_t g_hook_camera_far_clip_tramp;
static void hook_camera_far_clip(void *self, float v) {
    const float cap = far_clip_cap();
#ifdef DEBUG_SOLOADER
    static unsigned n = 0;
    if (n++ < 8U) l_info("FARCLIP: Camera::SetFarClip=%d cap=%d", (int)v, (int)cap);
#endif
    if (cap > 0.0f && v > cap) v = cap;   /* clamp the far plane IN; never push it out */
    MCSM_DETAIL_TRAMPOLINE(g_hook_camera_far_clip, self, v);
}

static void patch_render_perf_hooks(void) {
    const int quality_override = render_quality_override();
    /* A production hook is useful only when it changes quality. The logging build
     * still installs it unconditionally so natural engine values remain observable. */
#ifdef DEBUG_SOLOADER
    const int install_quality_hook = 1;
#else
    const int install_quality_hook = quality_override >= 0;
#endif
    if (install_quality_hook) {
        (void)hook_symbol_checked(&so_mod_gameengine,
                                  "_ZN19RenderConfiguration10SetQualityE17RenderQualityType",
                                  "RenderConfiguration::SetQuality",
                                  (uintptr_t)&hook_set_render_quality, &g_hook_set_render_quality);
    }
    if (quality_override >= 0) {
        l_info("PERF: render quality OVERRIDE -> %d (graphics.txt)", quality_override);
    }
    if (outlines_disabled()) {
        if (hook_symbol_checked(&so_mod_gameengine,
                                "_ZN17RenderObject_Mesh20SetRenderToonOutlineEb",
                                "RenderObject_Mesh::SetRenderToonOutline",
                                (uintptr_t)&hook_set_toon_outline, &g_hook_set_toon_outline)) {
            enable_validated_hot_trampoline("RenderObject_Mesh::SetRenderToonOutline",
                                            &g_hook_set_toon_outline,
                                            &g_set_toon_outline_tramp);
        }
        l_info("PERF: outlines DISABLED (graphics.txt outlines=off) — outline submit skipped.");
    }
    if (detail_scale() > 0.0f) {
        (void)mcsm_install_tramp_hook("_ZN5Scene17SetBrushFarDetailEf",
                                      "Scene::SetBrushFarDetail",
                                      (uintptr_t)&hook_scene_far_detail,
                                      &g_hook_scene_far_detail, &g_hook_scene_far_detail_tramp);
        (void)mcsm_install_tramp_hook("_ZN5Scene18SetBrushNearDetailEf",
                                      "Scene::SetBrushNearDetail",
                                      (uintptr_t)&hook_scene_near_detail,
                                      &g_hook_scene_near_detail, &g_hook_scene_near_detail_tramp);
        l_info("PERF: brush detail scaled to %d/1000 (graphics.txt detail) — NOTE: verified post-effect, NOT a vert cut.",
               (int)(detail_scale() * 1000.0f));
    }
    if (far_clip_cap() > 0.0f) {
        (void)mcsm_install_tramp_hook("_ZN6Camera10SetFarClipEf",
                                      "Camera::SetFarClip",
                                      (uintptr_t)&hook_camera_far_clip,
                                      &g_hook_camera_far_clip, &g_hook_camera_far_clip_tramp);
        l_info("PERF: far-clip capped at %d (graphics.txt draw_distance) — culls distant geometry (verts+draws).",
               (int)far_clip_cap());
    }
}

void so_patch(void) {
    patch_fmod_audio_hooks();
    patch_engine_diag_hooks();
    patch_sdl_android_runtime_hooks();
#ifdef DEBUG_SOLOADER
    patch_input_diag_hooks();
#endif
    patch_dlc_fast_path_hooks();
    patch_login_diag_hooks();
    patch_boot_diag_hooks();
    patch_render_perf_hooks();

    /* ★ ZERO-COPY BUFFER PATH (2026-07-30) -----------------------------------
     * These four hooks replace T3VertexBuffer/T3IndexBuffer PlatformLock and
     * PlatformUnlock, and they implement ONLY the engine's slow non-mapped
     * branch: malloc a CPU staging buffer -> let the skinner fill it -> full
     * glBufferData RESPECIFY -> free. Inside vitaGL that respecify is a GPU
     * free + a GPU alloc + a full-buffer memcpy, and it happens once per
     * SKINNED MESH per FRAME (RenderObject_Mesh::_RenderMeshInstance ->
     * DoSoftwareSkinning -> T3VertexBuffer::Lock), so it scales with character
     * count -- the reported failure exactly.
     *
     * The engine has a zero-copy path for this and it is ALREADY FULLY WIRED,
     * verified end to end against the shipped libGameEngine.so:
     *   1. vitaGL advertises "GL_OES_mapbuffer" (get_info.c extension list).
     *   2. RenderDevice::Initialize (@0x579e64) does
     *        strstr(glGetString(GL_EXTENSIONS), "GL_OES_mapbuffer")
     *      and on a hit calls _SetCap(21), i.e. sets bit 0x200000 of
     *      RenderDevice::mRenderCaps, then resolves glMapBufferOES and
     *      glUnmapBufferOES through GetExtension -> eglGetProcAddress.
     *   3. Our eglGetProcAddress goes to lookup_symbol_soloader_quiet, and
     *      dynlib.c already binds glMapBufferOES/glUnmapBufferOES/
     *      glMapBufferRange to vitaGL's real implementations. They resolve.
     *   4. So with bit 21 set, PlatformLock/Unlock (@0x57b998/@0x57ba74) use
     *      RenderDevice::MapGLBuffer + glUnmapBuffer -- the skinner writes
     *      STRAIGHT INTO GPU MEMORY. No staging buffer, no copy, no alloc.
     * vitaGL's glMapNamedBufferRange is literally `mapped = TRUE; return
     * ptr + offset;`.
     *
     * So the engine was ready to do this all along and these hooks are the only
     * thing preventing it. Not installing them hands the path back to the
     * engine. Confirm with the SKINPROBE line: respec/f and KB/f should fall to
     * ~0, because the respecify stops happening at all.
     *
     * ☠ DEVICE RESULT 2026-07-30: map_buffers = 1 CRASHES ON BOOT. Default 0.
     *
     * The engine half above is all confirmed on device -- the boot log shows
     * `GL extensions: OES_mapbuffer=1 EXT_map_buffer_range=1`, so caps 21 and 22
     * are both set and MapGLBuffer genuinely is reached. What is NOT true is the
     * assumption that the vitaGL entry point behind those pointers is usable.
     * The SHIPPED libvitaGL.a is built NO_DEBUG (SKIP_ERROR_HANDLING), which
     * reduces glMapNamedBufferRange to five instructions:
     *     ldr r3,[r0,#0]        ; gpu_buf->ptr   -- NO null check
     *     movs r2,#1 ; strb r2,[r0,#16]
     *     adds r0,r3,r1         ; return ptr+offset -- NO bounds check
     *     bx lr
     * It hands back ptr+offset unconditionally. These four hooks were the thing
     * that ALLOCATED that buffer, so with them gone ptr is NULL on the first lock
     * and the skinner writes hundreds of KB to a near-null address.
     *
     * I verified the vitaGL SOURCE (which has the checks behind
     * #ifndef SKIP_ERROR_HANDLING) instead of the installed BINARY (which does
     * not). Check the shipped .a, not the repo, before trusting a vitaGL path.
     *
     * TO MAKE THIS WORK: keep a lock hook that guarantees the GL buffer exists at
     * the right size -- glBufferData(size, NULL) once per buffer and on any size
     * change -- and only then let the engine map it. That keeps the zero-copy win
     * (no per-frame respecify, no full-buffer memcpy) while removing the
     * unallocated-pointer hazard. Not attempted yet; do not enable this until it
     * is, and re-verify against the installed lib. */
    if (mcsm_cfg()->map_buffers) {
        /* Replace the staging-buffer hooks with pre-allocating lock hooks and let
         * the engine's own zero-copy glMapBuffer path do the rest. The Unlock
         * hooks are deliberately NOT installed so the engine's glUnmapBuffer runs.
         * See the block comment on vb_ensure_gl_storage for why the bare
         * "install nothing" version of this crashed on boot. */
        patch_vertexbuffer_prealloc_lock();
        patch_indexbuffer_prealloc_lock();
        l_info("PERF: map_buffers=1 — engine zero-copy glMapBuffer path active, "
               "with loader-side glBufferData pre-allocation (expect SKINPROBE "
               "respec/f -> ~0)");
    } else {
        patch_vertexbuffer_platform_lock();
        patch_vertexbuffer_platform_unlock();
        patch_indexbuffer_platform_lock();
        patch_indexbuffer_platform_unlock();
    }

    /* mRenderCaps is resolved here but READ LATER, from mcsm_skin_report.
     * Reading it at patch time reported 0x00000000 and I briefly took that as
     * proof the engine never sets the caps -- it isn't. so_patch runs BEFORE
     * RenderDevice::Initialize, which is the function that calls _SetCap, so the
     * word is legitimately still zero at this point. The boot log's
     * "GL extensions: OES_mapbuffer=1 EXT_map_buffer_range=1" is the real evidence
     * that caps 21 and 22 do get set a moment later. */
    g_render_caps_ptr = (const uint32_t *)so_symbol(&so_mod_gameengine,
                            "_ZN12RenderDevice11mRenderCapsE");
    l_info("RENDERCAPS: mRenderCaps at %p (value read later, once the engine has "
           "run RenderDevice::Initialize)", (const void *)g_render_caps_ptr);
}
