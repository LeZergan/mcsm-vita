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

void telemetry_log(const char *tag, const char *fmt, ...)
                   __attribute__((format(printf, 2, 3)));
int telemetry_success_count(void);
const char *telemetry_last_path(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_TELEMETRY_H
