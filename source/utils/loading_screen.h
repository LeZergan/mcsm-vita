/**
 * @file loading_screen.h
 * @brief GLES2-rendered loading screen overlay for MCSM Vita port.
 *
 * Call loading_screen_init() after the PVR EGL context is ready.
 * Then call loading_screen_set_status() to update the message,
 * loading_screen_set_progress() for the progress bar (0.0 - 1.0),
 * and loading_screen_render() after each update.
 */
#ifndef SOLOADER_LOADING_SCREEN_H
#define SOLOADER_LOADING_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the loading screen GL resources. Call once after EGL init. */
void loading_screen_init(int fb_width, int fb_height);

/** Update the status message shown on screen. printf-style formatting. */
void loading_screen_set_status(const char *fmt, ...);

/** Set the progress bar fill (0.0 = empty, 1.0 = full). */
void loading_screen_set_progress(float progress);

/** Draw the loading screen and swap buffers. */
void loading_screen_render(void);

/** Draw only when the PVR context is already free/current for this thread. */
int loading_screen_try_render(void);

/** Start the asset-load timer (call once when loading begins). */
void loading_screen_start_timer(void);

/** Report the asset currently being loaded (full path; basename is shown). */
void loading_screen_set_asset(const char *name);

/** Throttled re-render: safe to call very frequently from load hooks; only
 *  actually redraws every ~150ms and stops once loading is marked done. */
void loading_screen_tick(void);

/** Mark asset loading complete: stops the timer, logs total, freezes the
 *  loading screen (the game's own rendering takes over from here). */
void loading_screen_mark_loaded(void);

/** Free GL resources. */

/** Non-zero if init succeeded (shaders built) and the screen can render. */
int loading_screen_is_ready(void);

/** Reset the per-load timer + animation at the start of a new scene load. */
void loading_screen_begin(void);

#ifdef __cplusplus
}
#endif

#endif /* SOLOADER_LOADING_SCREEN_H */
