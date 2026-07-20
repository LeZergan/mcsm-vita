/*
 * Copyright (C) 2026 Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

static const char *k_trace_paths[] = {
    DATA_PATH "loader.log",
    "ux0:data/mcsm_loader.log",
    "ux0:data/loader.log",
    "ux0:/data/mcsm/loader.log",
    "ux0:/data/mcsm_loader.log",
    "ux0:/data/loader.log",
    "ur0:data/mcsm_loader.log",
    "ur0:data/loader.log",
    "ur0:/data/mcsm_loader.log",
    "ur0:/data/loader.log",
};

static int g_success_count = 0;
static const char *g_last_path = NULL;
static const char *g_active_path = NULL;

static int trace_write_one(const char *path, const char *line, int flags) {
    int wrote = 0;
    SceUID fd = sceIoOpen(path, flags, 0644);
    if (fd >= 0) {
        if (line) {
            sceIoWrite(fd, line, sceClibStrnlen(line, 2048));
        }
        sceIoClose(fd);
        wrote = 1;
    } else if (line && (flags & SCE_O_APPEND)) {
        FILE *f = fopen(path, "ab");
        if (f) {
            fwrite(line, 1, strlen(line), f);
            fclose(f);
            wrote = 1;
        }
    } else if (!line && (flags & SCE_O_TRUNC)) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fclose(f);
            wrote = 1;
        }
    }

    if (wrote) {
        g_success_count++;
        g_last_path = path;
    }
    return wrote;
}

static int trace_write_active(const char *line, int flags) {
    if (g_active_path) {
        if (trace_write_one(g_active_path, line, flags)) {
            return 1;
        }
        g_active_path = NULL;
    }

    for (int i = 0; i < (int)(sizeof(k_trace_paths) / sizeof(k_trace_paths[0])); ++i) {
        if (trace_write_one(k_trace_paths[i], line, flags)) {
            g_active_path = k_trace_paths[i];
            return 1;
        }
    }

    return 0;
}

void telemetry_reset(void) {
    g_success_count = 0;
    g_last_path = NULL;
    g_active_path = NULL;
    trace_write_active(NULL, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC);
    trace_write_active("[BOOT] telemetry reset\n", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND);
}

void telemetry_log(const char *tag, const char *fmt, ...) {
#ifndef DEBUG_SOLOADER
    /* Production: telemetry logging (the [WATCH] watchdog to loader.log) fully OFF
     * — no sceIoOpen/Write per tick. Compiled out with ENABLE_TELEMETRY_LOGGING=OFF. */
    (void)tag; (void)fmt;
    return;
#else
    char msg[1536];
    char line[1700];

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);

    sceClibSnprintf(line, sizeof(line), "[%s] %s\n", tag ? tag : "TRACE", msg);
    sceClibPrintf("%s", line);
    trace_write_active(line, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND);
#endif
}

int telemetry_success_count(void) {
    return g_success_count;
}

const char *telemetry_last_path(void) {
    return g_last_path;
}
