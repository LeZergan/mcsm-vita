/*
 * Copyright (C) 2026 Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SOLOADER_TELEMETRY_H
#define SOLOADER_TELEMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

void telemetry_reset(void);

/* Write ONE line to loader.log regardless of ENABLE_TELEMETRY_LOGGING.
 *
 * This exists for exactly one thing: the build stamp. telemetry_log() compiles to
 * nothing in a production build, which also silently removed the only line that says
 * WHICH eboot is running -- on a project whose most repeated failure is a tester
 * measuring a stale build. Build identity is not diagnostic chatter, so it is not
 * gated with the diagnostics. Do not use this for anything else. */
void telemetry_stamp(const char *line);

void telemetry_log_impl(const char *tag, const char *fmt, ...)
                        __attribute__((format(printf, 2, 3)));

#ifdef DEBUG_SOLOADER
#define telemetry_log(...) telemetry_log_impl(__VA_ARGS__)
#else
/* Keep compiler format checking, but erase the call and all argument evaluation
 * from the production object. An out-of-line early-return stub still costs a BL
 * (and can evaluate expensive arguments) without LTO. */
#define telemetry_log(...) \
    do { if (0) telemetry_log_impl(__VA_ARGS__); } while (0)
#endif
int telemetry_success_count(void);
const char *telemetry_last_path(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_TELEMETRY_H
