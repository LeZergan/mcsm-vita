#ifndef SOLOADER_LAUNCH_STATE_H
#define SOLOADER_LAUNCH_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LaunchStage {
    LS_BOOT = 0,
    LS_SOLOADER_INIT,
    LS_RUN_THREAD_START,
    LS_JNI_ONLOAD_FMOD,
    LS_JNI_ONLOAD_FMODSTUDIO,
    LS_JNI_ONLOAD_SDL2,
    LS_NATIVE_RESIZE,
    LS_SURFACE_CHANGED,
    LS_NATIVEINIT_CALL,
    LS_NATIVEINIT_RETURN,
    LS_SDL_MAIN_CALL,
    LS_SDL_MAIN_RETURN,
    LS_IDLE_LOOP
} LaunchStage;

void launch_state_set_stage(LaunchStage stage);
LaunchStage launch_state_get_stage(void);
const char *launch_state_get_stage_name(LaunchStage stage);
uint64_t launch_state_stage_age_ms(void);
uint64_t launch_state_uptime_ms(void);

void launch_state_mark_poll(void);
uint32_t launch_state_get_poll_count(void);
uint64_t launch_state_last_poll_age_ms(void);

void launch_state_mark_progress(void);
uint32_t launch_state_get_progress_count(void);
uint64_t launch_state_last_progress_age_ms(void);

// Real frame-present heartbeat. Marked from gl_swap() on every actual
// vglSwapBuffers, so this reflects frames truly pushed to the display
// (ground truth for "is the game rendering"), unlike the input poll above
// which SDL ticks even while the screen stays black.
void launch_state_mark_present(void);
uint32_t launch_state_get_present_count(void);
uint64_t launch_state_last_present_age_ms(void);

void launch_state_mark_scene_active(void);
int launch_state_scene_active(void);

/* Marks which (potentially blocking) GL call is in progress so a render-thread
 * freeze can be pinpointed from the watchdog snapshot.
 * 0=idle 1=vglSwapBuffers 2=glLinkProgram 3=glCompileShader */
void launch_state_mark_gl_phase(int phase);

/* Records the in-flight draw (sets phase=3). If a GPU hang makes the draw block,
 * the watchdog snapshot shows which program/geometry/index-type stalled it. */
void launch_state_mark_draw(unsigned mode, int count, unsigned type, int program);

void launch_state_snapshot(char *out, size_t out_size);
void launch_state_dump_history(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
