/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"
#include "utils/utils.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdbool.h>
#include <stdatomic.h>

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);
static SceUID _log_fd = -1;

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

/* Complete logging kill-switch: if ux0:data/mcsm/nolog.txt exists, ALL logging
 * (file + debug console) is suppressed so we can definitively rule logging out as
 * a performance factor. Checked once, lazily, under the log mutex. */
static int _log_enabled = -1; /* -1 unchecked, 1 enabled, 0 disabled */
static int log_enabled_locked(void) {
    if (_log_enabled < 0) {
        SceUID f = sceIoOpen(DATA_PATH "nolog.txt", SCE_O_RDONLY, 0);
        if (f < 0) f = sceIoOpen("ux0:data/mcsm/nolog.txt", SCE_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); _log_enabled = 0; }
        else _log_enabled = 1;
    }
    return _log_enabled;
}

/* Diagnostic flush-every-line mode: if ux0:data/mcsm/logsync.txt exists, EVERY
 * log line is flushed to disk immediately (no RAM batching). Lets us see the
 * exact pre-hang/pre-crash tail when a boot hangs (which never flushes the
 * accumulated buffer). Costs write latency, so only for debugging — not the
 * shipping default. Checked once, lazily, under the log mutex. */
static int _log_sync = -1; /* -1 unchecked, 1 sync-every-line, 0 batched */
static int log_sync_locked(void) {
    if (_log_sync < 0) {
        SceUID f = sceIoOpen(DATA_PATH "logsync.txt", SCE_O_RDONLY, 0);
        if (f < 0) f = sceIoOpen("ux0:data/mcsm/logsync.txt", SCE_O_RDONLY, 0);
        if (f >= 0) { sceIoClose(f); _log_sync = 1; }
        else _log_sync = 0;
    }
    return _log_sync;
}

static void log_open_file_locked(void) {
    if (_log_fd >= 0) {
        return;
    }

    _log_fd = sceIoOpen(DATA_PATH "loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0644);
    if (_log_fd < 0) {
        _log_fd = sceIoOpen("ux0:data/loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0644);
    }
    if (_log_fd < 0) {
        _log_fd = sceIoOpen("ux0:/data/loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0644);
    }
}

/* PERF 2026-06-29: previously every l_info did a synchronous sceIoWrite to the
 * ux0 memory card from the render/sim thread. Memory-card write latency spikes
 * intermittently to tens of ms -> periodic frame dips (the gameplay stutter).
 * Batch log text into a RAM buffer and flush in bulk: one sceIoWrite per ~28KB
 * instead of per line. Important events (WARN/ERROR/FATAL) force a flush so the
 * pre-crash tail is preserved. Buffer + fd are only touched under _log_mutex. */
#define LOG_ACCUM_CAP 32768
static char _log_accum[LOG_ACCUM_CAP];
static size_t _log_accum_len = 0;

static void log_flush_locked(void) {
    if (_log_fd >= 0 && _log_accum_len > 0) {
        sceIoWrite(_log_fd, _log_accum, _log_accum_len);
        _log_accum_len = 0;
    }
}

static void log_write_file_locked(const char *line, int force_flush) {
    log_open_file_locked();
    if (_log_fd < 0 || !line) {
        return;
    }

    size_t n = sceClibStrnlen(line, sizeof(buffer_b));
    if (n == 0) {
        return;
    }
    /* A single line larger than the buffer: flush then write it directly. */
    if (n >= LOG_ACCUM_CAP) {
        log_flush_locked();
        sceIoWrite(_log_fd, line, n);
        return;
    }
    if (_log_accum_len + n > LOG_ACCUM_CAP) {
        log_flush_locked();
    }
    sceClibMemcpy(_log_accum + _log_accum_len, line, n);
    _log_accum_len += n;

    if (force_flush || _log_accum_len >= (LOG_ACCUM_CAP - 2048)) {
        log_flush_locked();
    }
}

void log_reset_file(void) {
    if (_log_fd >= 0) {
        log_flush_locked();
        sceIoClose(_log_fd);
        _log_fd = -1;
    }
    _log_accum_len = 0;

    SceUID fd = sceIoOpen(DATA_PATH "loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0644);
    if (fd < 0) {
        fd = sceIoOpen("ux0:data/loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0644);
    }
    if (fd < 0) {
        fd = sceIoOpen("ux0:/data/loader.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0644);
    }
    if (fd >= 0) {
        sceIoClose(fd);
    }

}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (!log_enabled_locked()) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
        return;
    }

    const char *tag = "INFO";
    switch (t) {
        case LT_DEBUG:
            tag = "DEBUG";
            break;
        case LT_INFO:
            tag = "INFO";
            break;
        case LT_WARN:
            tag = "WARN";
            break;
        case LT_ERROR:
            tag = "ERROR";
            break;
        case LT_FATAL:
            tag = "FATAL";
            break;
        case LT_SUCCESS:
            tag = "OK";
            break;
        case LT_WAIT:
            tag = "WAIT";
            break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    sceClibSnprintf(buffer_a, sizeof(buffer_a), "[%s] %s\n", tag, fmt);

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);

    /* Flush to disk immediately only for important events; INFO/DEBUG just
     * accumulate in RAM (flushed in bulk). Skip the per-line sceClibPrintf for
     * routine logs — it goes to the kernel debug console and adds hot-path cost
     * for no benefit on a retail unit; keep it for errors. */
    const int important = (t == LT_WARN || t == LT_ERROR || t == LT_FATAL);
    if (important) {
        sceClibPrintf("%s", buffer_b);
    }
    log_write_file_locked(buffer_b, important || log_sync_locked());

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
