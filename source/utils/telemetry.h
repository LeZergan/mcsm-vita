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
void telemetry_log(const char *tag, const char *fmt, ...)
                   __attribute__((format(printf, 2, 3)));
int telemetry_success_count(void);
const char *telemetry_last_path(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_TELEMETRY_H
