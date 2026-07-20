/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/log.h"
#include "utils/logger.h"
#include <psp2/kernel/clib.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define RECENT_ALOG_MAX 64
#define RECENT_ALOG_TAG_MAX 32
#define RECENT_ALOG_TEXT_MAX 224

typedef struct RecentAlog {
    int prio;
    char tag[RECENT_ALOG_TAG_MAX];
    char text[RECENT_ALOG_TEXT_MAX];
} RecentAlog;

static RecentAlog g_recent_alog[RECENT_ALOG_MAX];
static atomic_uint g_recent_alog_head = ATOMIC_VAR_INIT(0);
static atomic_uint g_recent_alog_count = ATOMIC_VAR_INIT(0);

static const char *prio_name(int prio) {
    switch (prio) {
        case ANDROID_LOG_VERBOSE: return "V";
        case ANDROID_LOG_DEBUG: return "D";
        case ANDROID_LOG_INFO: return "I";
        case ANDROID_LOG_WARN: return "W";
        case ANDROID_LOG_ERROR: return "E";
        case ANDROID_LOG_FATAL: return "F";
        default: return "?";
    }
}

static void alog_store(int prio, const char *tag, const char *text) {
    unsigned slot = atomic_fetch_add_explicit(&g_recent_alog_head, 1, memory_order_relaxed) % RECENT_ALOG_MAX;
    RecentAlog *e = &g_recent_alog[slot];
    e->prio = prio;
    sceClibSnprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "(null)");
    sceClibSnprintf(e->text, sizeof(e->text), "%s", text ? text : "(null)");

    unsigned count = atomic_load_explicit(&g_recent_alog_count, memory_order_relaxed);
    if (count < RECENT_ALOG_MAX) {
        atomic_store_explicit(&g_recent_alog_count, count + 1, memory_order_relaxed);
    }
}

static int should_suppress_alog_print(int prio, const char *tag, const char *text) {
    return prio <= ANDROID_LOG_INFO &&
           tag && text &&
           strncmp(tag, "SDL/APP", 7) == 0 &&
           strncmp(text, "Found file:", 11) == 0;
}

#define print_common \
    if (!should_suppress_alog_print(prio, tag, text)) { \
        switch (prio) { \
            case ANDROID_LOG_INFO: \
                l_info("[ALOG][%s] %s", tag, text); \
                break; \
            case ANDROID_LOG_WARN: \
                l_warn("[ALOG][%s] %s", tag, text); \
                break; \
            case ANDROID_LOG_ERROR: \
            case ANDROID_LOG_FATAL: \
                l_error("[ALOG][%s] %s", tag, text); \
                break; \
            case ANDROID_LOG_UNKNOWN: \
            case ANDROID_LOG_DEFAULT: \
            case ANDROID_LOG_VERBOSE: \
            case ANDROID_LOG_DEBUG: \
            case ANDROID_LOG_SILENT: \
            default: \
                l_debug("[ALOG][%s] %s", tag, text); \
                break; \
        } \
    } \
    alog_store(prio, tag, text);

int __android_log_write(int prio, const char* tag, const char* text) {
    print_common
    return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list list;
    char text[1024];

    va_start(list, fmt);
    sceClibVsnprintf(text, sizeof(text), fmt, list);
    va_end(list);

    print_common

    return 0;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char text[1024];

    sceClibVsnprintf(text, sizeof(text), fmt, ap);

    print_common

    return 0;
}

void android_log_dump_recent(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    unsigned count = atomic_load_explicit(&g_recent_alog_count, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&g_recent_alog_head, memory_order_relaxed);
    if (count == 0) {
        sceClibSnprintf(out, out_size, "none");
        return;
    }

    unsigned written = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned idx = (head + RECENT_ALOG_MAX - count + i) % RECENT_ALOG_MAX;
        RecentAlog *e = &g_recent_alog[idx];
        int n = sceClibSnprintf(
            out + written,
            (written < out_size) ? (out_size - written) : 0,
            "%s[%s][%s] %s",
            (i == 0) ? "" : "\n",
            prio_name(e->prio),
            e->tag,
            e->text);
        if (n <= 0) {
            break;
        }
        written += (unsigned)n;
        if (written + 1 >= out_size) {
            break;
        }
    }
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    if (fmt) {
        va_list list;
        char text[1024];

        va_start(list, fmt);
        sceClibVsnprintf(text, sizeof(text), fmt, list);
        va_end(list);

        l_fatal("[ALOG][ASSERT] %s", text);
    } else {
        if (cond) {
            l_fatal("[ALOG][ASSERT] Assertion failed: %s", cond);
        } else {
            l_fatal("[ALOG][ASSERT] Unspecified assertion failed");
        }
    }

    abort();
}
