#include "utils/launch_state.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <limits.h>
#include <stdatomic.h>

#define LS_HISTORY_MAX 32

typedef struct StageHistoryEntry {
    uint64_t at_ms;
    int stage;
} StageHistoryEntry;

static atomic_int g_stage = ATOMIC_VAR_INIT((int)LS_BOOT);
static atomic_uint g_poll_count = ATOMIC_VAR_INIT(0);
static atomic_ullong g_last_poll_ms = ATOMIC_VAR_INIT(0);
static atomic_uint g_progress_count = ATOMIC_VAR_INIT(0);
static atomic_ullong g_last_progress_ms = ATOMIC_VAR_INIT(0);
static atomic_uint g_present_count = ATOMIC_VAR_INIT(0);
static atomic_ullong g_last_present_ms = ATOMIC_VAR_INIT(0);
static atomic_int g_scene_active = ATOMIC_VAR_INIT(0);
static atomic_ullong g_stage_enter_ms = ATOMIC_VAR_INIT(0);
static atomic_ullong g_start_ms = ATOMIC_VAR_INIT(0);
static atomic_uint g_hist_head = ATOMIC_VAR_INIT(0);
static atomic_uint g_hist_count = ATOMIC_VAR_INIT(0);
static StageHistoryEntry g_hist[LS_HISTORY_MAX];

static atomic_int g_min_user = ATOMIC_VAR_INIT(INT_MAX);
static atomic_int g_min_cdram = ATOMIC_VAR_INIT(INT_MAX);
static atomic_int g_min_phy = ATOMIC_VAR_INIT(INT_MAX);
static atomic_int g_last_mem_rc = ATOMIC_VAR_INIT(0);

/* GL-phase tracker: which blocking GL call (if any) is in progress, so the
 * watchdog snapshot can pinpoint a render-thread freeze (swap/present stall vs
 * shader link/compile hang). 0=idle 1=vglSwapBuffers 2=glLinkProgram 3=glCompileShader */
static atomic_int g_gl_phase = ATOMIC_VAR_INIT(0);
static atomic_uint g_gl_phase_tid = ATOMIC_VAR_INIT(0);
static atomic_ullong g_gl_phase_ms = ATOMIC_VAR_INIT(0);
/* Last/in-flight draw call params, so a GPU-hang that blocks a draw (glphase=3)
 * is pinpointed: which program/geometry size/index type stalled the GPU. */
static atomic_uint g_draw_mode = ATOMIC_VAR_INIT(0);
static atomic_int  g_draw_count = ATOMIC_VAR_INIT(0);
static atomic_uint g_draw_type = ATOMIC_VAR_INIT(0);
static atomic_int  g_draw_prog = ATOMIC_VAR_INIT(0);
static atomic_uint g_draw_serial = ATOMIC_VAR_INIT(0);

static uint64_t now_ms(void) {
    return sceKernelGetSystemTimeWide() / 1000ULL;
}

static void history_push(LaunchStage stage, uint64_t ms) {
    const unsigned slot = atomic_fetch_add_explicit(&g_hist_head, 1, memory_order_relaxed) % LS_HISTORY_MAX;
    g_hist[slot].at_ms = ms;
    g_hist[slot].stage = (int)stage;

    unsigned count = atomic_load_explicit(&g_hist_count, memory_order_relaxed);
    if (count < LS_HISTORY_MAX) {
        atomic_store_explicit(&g_hist_count, count + 1, memory_order_relaxed);
    }
}

static void update_min(atomic_int *dst, int value) {
    int cur = atomic_load_explicit(dst, memory_order_relaxed);
    while (value < cur && !atomic_compare_exchange_weak_explicit(
            dst, &cur, value, memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void update_mem_minima(int mem_rc, int user, int cdram, int phy) {
    atomic_store_explicit(&g_last_mem_rc, mem_rc, memory_order_relaxed);
    if (mem_rc >= 0) {
        update_min(&g_min_user, user);
        update_min(&g_min_cdram, cdram);
        update_min(&g_min_phy, phy);
    }
}

const char *launch_state_get_stage_name(LaunchStage stage) {
    switch (stage) {
        case LS_BOOT: return "BOOT";
        case LS_SOLOADER_INIT: return "SOLOADER_INIT";
        case LS_RUN_THREAD_START: return "RUN_THREAD_START";
        case LS_JNI_ONLOAD_FMOD: return "JNI_ONLOAD_FMOD";
        case LS_JNI_ONLOAD_FMODSTUDIO: return "JNI_ONLOAD_FMODSTUDIO";
        case LS_JNI_ONLOAD_SDL2: return "JNI_ONLOAD_SDL2";
        case LS_NATIVE_RESIZE: return "NATIVE_RESIZE";
        case LS_SURFACE_CHANGED: return "SURFACE_CHANGED";
        case LS_NATIVEINIT_CALL: return "NATIVEINIT_CALL";
        case LS_NATIVEINIT_RETURN: return "NATIVEINIT_RETURN";
        case LS_SDL_MAIN_CALL: return "SDL_MAIN_CALL";
        case LS_SDL_MAIN_RETURN: return "SDL_MAIN_RETURN";
        case LS_IDLE_LOOP: return "IDLE_LOOP";
        default: return "UNKNOWN";
    }
}

void launch_state_set_stage(LaunchStage stage) {
    uint64_t ms = now_ms();
    if (atomic_load_explicit(&g_start_ms, memory_order_relaxed) == 0) {
        atomic_store_explicit(&g_start_ms, ms, memory_order_relaxed);
    }
    atomic_store_explicit(&g_stage, (int)stage, memory_order_relaxed);
    atomic_store_explicit(&g_stage_enter_ms, ms, memory_order_relaxed);
    history_push(stage, ms);
}

LaunchStage launch_state_get_stage(void) {
    return (LaunchStage)atomic_load_explicit(&g_stage, memory_order_relaxed);
}

uint64_t launch_state_stage_age_ms(void) {
    uint64_t entered = atomic_load_explicit(&g_stage_enter_ms, memory_order_relaxed);
    uint64_t now = now_ms();
    return (entered == 0 || now < entered) ? 0 : (now - entered);
}

uint64_t launch_state_uptime_ms(void) {
    uint64_t start = atomic_load_explicit(&g_start_ms, memory_order_relaxed);
    uint64_t now = now_ms();
    return (start == 0 || now < start) ? 0 : (now - start);
}

void launch_state_mark_poll(void) {
    atomic_store_explicit(&g_last_poll_ms, now_ms(), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_poll_count, 1, memory_order_relaxed);
}

uint32_t launch_state_get_poll_count(void) {
    return atomic_load_explicit(&g_poll_count, memory_order_relaxed);
}

uint64_t launch_state_last_poll_age_ms(void) {
    const uint64_t last_poll_ms = atomic_load_explicit(&g_last_poll_ms, memory_order_relaxed);
    if (last_poll_ms == 0) {
        return UINT64_MAX;
    }

    const uint64_t now = now_ms();
    return now < last_poll_ms ? 0 : (now - last_poll_ms);
}

void launch_state_mark_progress(void) {
    atomic_store_explicit(&g_last_progress_ms, now_ms(), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_progress_count, 1, memory_order_relaxed);
}

uint32_t launch_state_get_progress_count(void) {
    return atomic_load_explicit(&g_progress_count, memory_order_relaxed);
}

uint64_t launch_state_last_progress_age_ms(void) {
    const uint64_t last_progress_ms = atomic_load_explicit(&g_last_progress_ms, memory_order_relaxed);
    if (last_progress_ms == 0) {
        return UINT64_MAX;
    }

    const uint64_t now = now_ms();
    return now < last_progress_ms ? 0 : (now - last_progress_ms);
}

void launch_state_mark_present(void) {
    atomic_store_explicit(&g_last_present_ms, now_ms(), memory_order_relaxed);
    atomic_fetch_add_explicit(&g_present_count, 1, memory_order_relaxed);
}

uint32_t launch_state_get_present_count(void) {
    return atomic_load_explicit(&g_present_count, memory_order_relaxed);
}

uint64_t launch_state_last_present_age_ms(void) {
    const uint64_t last_present_ms = atomic_load_explicit(&g_last_present_ms, memory_order_relaxed);
    if (last_present_ms == 0) {
        return UINT64_MAX;
    }

    const uint64_t now = now_ms();
    return now < last_present_ms ? 0 : (now - last_present_ms);
}

void launch_state_mark_gl_phase(int phase) {
    atomic_store_explicit(&g_gl_phase, phase, memory_order_relaxed);
    atomic_store_explicit(&g_gl_phase_tid, (unsigned)sceKernelGetThreadId(), memory_order_relaxed);
    atomic_store_explicit(&g_gl_phase_ms, now_ms(), memory_order_relaxed);
}

void launch_state_mark_draw(unsigned mode, int count, unsigned type, int program) {
    atomic_store_explicit(&g_draw_mode, mode, memory_order_relaxed);
    atomic_store_explicit(&g_draw_count, count, memory_order_relaxed);
    atomic_store_explicit(&g_draw_type, type, memory_order_relaxed);
    atomic_store_explicit(&g_draw_prog, program, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_draw_serial, 1, memory_order_relaxed);
    atomic_store_explicit(&g_gl_phase, 3, memory_order_relaxed); /* in draw */
    /* No per-draw timer/tid syscalls (2026-07-17): launch_state_mark_gl_phase()
     * (called each present in gl_swap) already refreshes g_gl_phase_ms + tid. The
     * draw serial signals per-draw progress, so a mid-frame wedge still reads as a
     * stale phase_ms + a stalled serial. Drops sceKernelGetSystemTimeWide +
     * sceKernelGetThreadId from EVERY draw (~900/frame). */
}

void launch_state_mark_scene_active(void) {
    atomic_store_explicit(&g_scene_active, 1, memory_order_relaxed);
    launch_state_mark_progress();
}

int launch_state_scene_active(void) {
    return atomic_load_explicit(&g_scene_active, memory_order_relaxed) != 0;
}

void launch_state_snapshot(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    SceKernelFreeMemorySizeInfo mem;
    sceClibMemset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    int mem_rc = sceKernelGetFreeMemorySize(&mem);
    update_mem_minima(mem_rc, mem.size_user, mem.size_cdram, mem.size_phycont);

    const LaunchStage stage = launch_state_get_stage();
    const uint64_t uptime_ms = launch_state_uptime_ms();
    const uint64_t stage_age_ms = launch_state_stage_age_ms();
    const uint32_t polls = launch_state_get_poll_count();
    const uint64_t poll_age_ms = launch_state_last_poll_age_ms();
    const long long poll_age_log = (poll_age_ms == UINT64_MAX) ? -1LL : (long long)poll_age_ms;
    const uint32_t progress = launch_state_get_progress_count();
    const uint64_t progress_age_ms = launch_state_last_progress_age_ms();
    const long long progress_age_log = (progress_age_ms == UINT64_MAX) ? -1LL : (long long)progress_age_ms;
    const uint32_t frames = launch_state_get_present_count();
    const int scene_active = launch_state_scene_active();
    const uint64_t present_age_ms = launch_state_last_present_age_ms();
    const long long present_age_log = (present_age_ms == UINT64_MAX) ? -1LL : (long long)present_age_ms;
    const int min_user = atomic_load_explicit(&g_min_user, memory_order_relaxed);
    const int min_cdram = atomic_load_explicit(&g_min_cdram, memory_order_relaxed);
    const int min_phy = atomic_load_explicit(&g_min_phy, memory_order_relaxed);
    const int gl_phase = atomic_load_explicit(&g_gl_phase, memory_order_relaxed);
    const unsigned gl_phase_tid = atomic_load_explicit(&g_gl_phase_tid, memory_order_relaxed);
    const uint64_t gl_phase_ms = atomic_load_explicit(&g_gl_phase_ms, memory_order_relaxed);
    const long long gl_phase_age = (gl_phase_ms == 0) ? -1LL : (long long)(now_ms() - gl_phase_ms);
    const unsigned draw_mode = atomic_load_explicit(&g_draw_mode, memory_order_relaxed);
    const int draw_count = atomic_load_explicit(&g_draw_count, memory_order_relaxed);
    const unsigned draw_type = atomic_load_explicit(&g_draw_type, memory_order_relaxed);
    const int draw_prog = atomic_load_explicit(&g_draw_prog, memory_order_relaxed);
    const unsigned draw_serial = atomic_load_explicit(&g_draw_serial, memory_order_relaxed);

    if (mem_rc >= 0) {
        sceClibSnprintf(out, out_size,
            "stage=%s scene=%d age=%llums uptime=%llums poll=%u poll_age=%lldms "
            "progress=%u progress_age=%lldms frames=%u present_age=%lldms "
            "glphase=%d glphase_tid=0x%X glphase_age=%lldms "
            "lastdraw[#%u mode=0x%X count=%d type=0x%X prog=%d] mem_rc=0x%08X "
            "free_user=%d(%dMB) free_cdram=%dKB free_phy=%dKB "
            "min_user=%d(%dMB) min_cdram=%dKB min_phy=%dKB",
            launch_state_get_stage_name(stage),
            scene_active,
            (unsigned long long)stage_age_ms,
            (unsigned long long)uptime_ms,
            polls,
            poll_age_log,
            progress,
            progress_age_log,
            frames,
            present_age_log,
            gl_phase,
            gl_phase_tid,
            gl_phase_age,
            draw_serial,
            draw_mode,
            draw_count,
            draw_type,
            draw_prog,
            mem_rc,
            mem.size_user,
            mem.size_user / (1024 * 1024),
            mem.size_cdram / 1024,
            mem.size_phycont / 1024,
            min_user == INT_MAX ? -1 : min_user,
            min_user == INT_MAX ? -1 : (min_user / (1024 * 1024)),
            min_cdram == INT_MAX ? -1 : (min_cdram / 1024),
            min_phy == INT_MAX ? -1 : (min_phy / 1024));
    } else {
        sceClibSnprintf(out, out_size,
            "stage=%s scene=%d age=%llums uptime=%llums poll=%u frames=%u "
            "progress=%u free_mem=unavailable(mem_rc=0x%08X)",
            launch_state_get_stage_name(stage),
            scene_active,
            (unsigned long long)stage_age_ms,
            (unsigned long long)uptime_ms,
            polls,
            frames,
            progress,
            mem_rc);
    }
}

void launch_state_dump_history(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    const unsigned count = atomic_load_explicit(&g_hist_count, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&g_hist_head, memory_order_relaxed);

    if (count == 0) {
        sceClibSnprintf(out, out_size, "none");
        return;
    }

    const uint64_t start = atomic_load_explicit(&g_start_ms, memory_order_relaxed);
    unsigned written = 0;

    // Iterate from oldest to newest entry.
    for (unsigned i = 0; i < count; ++i) {
        unsigned idx = (head + LS_HISTORY_MAX - count + i) % LS_HISTORY_MAX;
        StageHistoryEntry e = g_hist[idx];
        uint64_t rel_ms = (start && e.at_ms >= start) ? (e.at_ms - start) : 0;
        int n = sceClibSnprintf(out + written,
                                (written < out_size) ? (out_size - written) : 0,
                                "%s%s@%llums",
                                (i == 0) ? "" : " -> ",
                                launch_state_get_stage_name((LaunchStage)e.stage),
                                (unsigned long long)rel_ms);
        if (n <= 0) {
            break;
        }
        written += (unsigned)n;
        if (written + 1 >= out_size) {
            break;
        }
    }
}
