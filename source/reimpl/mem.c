/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "reimpl/io.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <psp2/kernel/clib.h>

#define MMAP_MAX_BYTES (24 * 1024 * 1024)
#define MEMALIGN_MAX_BYTES (24 * 1024 * 1024)
/* 1M entries (8MB BSS). The engine keeps ~80k+ allocations LIVE; a 32k table was
 * 2.5x over-full, so every alloc scanned all 32768 slots under a spinlock across
 * all threads (O(n) per alloc -> the 20-minute load). 1M keeps the load factor
 * low (~0.08) so probes are O(1). ALLOC_PROBE_MAX caps the worst case so a dense
 * table (tombstone buildup over a long session) can never re-trigger a full scan. */
#define ALLOC_TABLE_SIZE 1048576
#define ALLOC_TABLE_MASK (ALLOC_TABLE_SIZE - 1)
#define ALLOC_PROBE_MAX 256u
#define PTR_TOMBSTONE ((void *)1)
#define LARGE_ALLOC_BYTES (1024 * 1024)

typedef struct AllocEntry {
    void *ptr;
    size_t size;
} AllocEntry;

static AllocEntry g_alloc_table[ALLOC_TABLE_SIZE];
static atomic_flag g_alloc_lock = ATOMIC_FLAG_INIT;

static unsigned long long g_alloc_calls = 0;
static unsigned long long g_calloc_calls = 0;
static unsigned long long g_realloc_calls = 0;
static unsigned long long g_memalign_calls = 0;
static unsigned long long g_valloc_calls = 0;
static unsigned long long g_free_calls = 0;
static unsigned long long g_alloc_fail = 0;
static unsigned long long g_memalign_denied = 0;
static unsigned long long g_requested_bytes = 0;
static unsigned long long g_live_bytes = 0;
static unsigned long long g_peak_live_bytes = 0;
static unsigned long long g_untracked_ptrs = 0;

static void alloc_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_alloc_lock, memory_order_acquire)) {
    }
}

static void alloc_unlock(void) {
    atomic_flag_clear_explicit(&g_alloc_lock, memory_order_release);
}

static inline uint32_t ptr_hash(const void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    v >>= 4;
    v ^= (v >> 17);
    return (uint32_t)v;
}

static int table_insert_locked(void *ptr, size_t size) {
    int tomb = -1;
    uint32_t idx = ptr_hash(ptr) & ALLOC_TABLE_MASK;

    for (uint32_t i = 0; i < ALLOC_PROBE_MAX; ++i) {
        uint32_t s = (idx + i) & ALLOC_TABLE_MASK;
        void *cur = g_alloc_table[s].ptr;

        if (cur == ptr) {
            g_alloc_table[s].size = size;
            return 1;
        }
        if (cur == PTR_TOMBSTONE) {
            if (tomb < 0) {
                tomb = (int)s;
            }
            continue;
        }
        if (cur == NULL) {
            if (tomb >= 0) {
                s = (uint32_t)tomb;
            }
            g_alloc_table[s].ptr = ptr;
            g_alloc_table[s].size = size;
            return 1;
        }
    }

    if (tomb >= 0) {
        g_alloc_table[(uint32_t)tomb].ptr = ptr;
        g_alloc_table[(uint32_t)tomb].size = size;
        return 1;
    }

    return 0;
}

static size_t table_remove_locked(void *ptr) {
    uint32_t idx = ptr_hash(ptr) & ALLOC_TABLE_MASK;
    for (uint32_t i = 0; i < ALLOC_PROBE_MAX; ++i) {
        uint32_t s = (idx + i) & ALLOC_TABLE_MASK;
        void *cur = g_alloc_table[s].ptr;
        if (cur == NULL) {
            return 0;
        }
        if (cur == ptr) {
            size_t old = g_alloc_table[s].size;
            g_alloc_table[s].ptr = PTR_TOMBSTONE;
            g_alloc_table[s].size = 0;
            return old;
        }
    }
    return 0;
}

static void update_peak_locked(void) {
    if (g_live_bytes > g_peak_live_bytes) {
        g_peak_live_bytes = g_live_bytes;
    }
}

static void on_alloc_locked(void *ptr, size_t size, unsigned long long *live, unsigned long long *peak) {
    g_requested_bytes += (unsigned long long)size;
    if (!ptr) {
        g_alloc_fail++;
        if (live) *live = g_live_bytes;
        if (peak) *peak = g_peak_live_bytes;
        return;
    }

    if (!table_insert_locked(ptr, size)) {
        g_untracked_ptrs++;
    } else {
        g_live_bytes += (unsigned long long)size;
        update_peak_locked();
    }
    if (live) *live = g_live_bytes;
    if (peak) *peak = g_peak_live_bytes;
}

static void on_free_locked(void *ptr, unsigned long long *live, unsigned long long *peak, size_t *freed_size) {
    size_t old = 0;
    if (ptr) {
        old = table_remove_locked(ptr);
        if (old > 0) {
            if (g_live_bytes >= (unsigned long long)old) {
                g_live_bytes -= (unsigned long long)old;
            } else {
                g_live_bytes = 0;
            }
        }
    }
    if (freed_size) *freed_size = old;
    if (live) *live = g_live_bytes;
    if (peak) *peak = g_peak_live_bytes;
}

/* PERF 2026-06-30: MEMTRACE wraps EVERY alloc/free with a mutex lock + a
 * tracking-hashtable insert/remove. The engine does thousands of allocations
 * per scene load -> this was a major chunk of the "engine CPU" load freezes and
 * per-frame cost. Compile it OUT for the shipping build (const 0 -> the branch
 * and all tracking code below is dead-code-eliminated, leaving a raw libc call).
 * Flip to 1 only when debugging a memory leak/OOM. */
static const int g_memtrace_on = 0;

void *malloc_soloader(size_t size) {
    if (!g_memtrace_on) return malloc(size);
    void *ptr = malloc(size);
    unsigned long long live = 0, peak = 0;

    alloc_lock();
    g_alloc_calls++;
    on_alloc_locked(ptr, size, &live, &peak);
    alloc_unlock();

    if (!ptr) {
        l_error("[MEMTRACE] malloc(%u) failed live=%llu peak=%llu", (unsigned)size, live, peak);
    } else if (size >= LARGE_ALLOC_BYTES) {
        l_warn("[MEMTRACE] malloc(%u) -> %p live=%llu peak=%llu", (unsigned)size, ptr, live, peak);
    }
    return ptr;
}

void *calloc_soloader(size_t nmemb, size_t size) {
    if (nmemb && size > ((size_t)-1 / nmemb)) {
        errno = ENOMEM;
        return NULL;
    }

    if (!g_memtrace_on) return calloc(nmemb, size);
    const size_t total = nmemb * size;
    void *ptr = calloc(nmemb, size);
    unsigned long long live = 0, peak = 0;

    alloc_lock();
    g_calloc_calls++;
    on_alloc_locked(ptr, total, &live, &peak);
    alloc_unlock();

    if (!ptr) {
        l_error("[MEMTRACE] calloc(%u,%u) failed live=%llu peak=%llu",
                (unsigned)nmemb, (unsigned)size, live, peak);
    } else if (total >= LARGE_ALLOC_BYTES) {
        l_warn("[MEMTRACE] calloc(%u,%u) -> %p live=%llu peak=%llu",
               (unsigned)nmemb, (unsigned)size, ptr, live, peak);
    }
    return ptr;
}

void *realloc_soloader(void *ptr, size_t size) {
    if (!g_memtrace_on) return realloc(ptr, size);
    size_t old_size = 0;
    unsigned long long live = 0, peak = 0;

    alloc_lock();
    g_realloc_calls++;
    if (ptr) {
        old_size = table_remove_locked(ptr);
        if (old_size > 0) {
            if (g_live_bytes >= (unsigned long long)old_size) {
                g_live_bytes -= (unsigned long long)old_size;
            } else {
                g_live_bytes = 0;
            }
        }
    }
    alloc_unlock();

    void *new_ptr = realloc(ptr, size);

    alloc_lock();
    if (!new_ptr) {
        g_alloc_fail++;
        // realloc failure preserves old pointer contents; restore tracking.
        if (ptr && old_size > 0) {
            if (!table_insert_locked(ptr, old_size)) {
                g_untracked_ptrs++;
            } else {
                g_live_bytes += (unsigned long long)old_size;
                update_peak_locked();
            }
        }
    } else {
        on_alloc_locked(new_ptr, size, NULL, NULL);
    }
    live = g_live_bytes;
    peak = g_peak_live_bytes;
    alloc_unlock();

    if (!new_ptr) {
        l_error("[MEMTRACE] realloc(%p,%u) failed old=%u live=%llu peak=%llu",
                ptr, (unsigned)size, (unsigned)old_size, live, peak);
    } else if (size >= LARGE_ALLOC_BYTES || old_size >= LARGE_ALLOC_BYTES || new_ptr != ptr) {
        l_warn("[MEMTRACE] realloc(%p,%u) -> %p old=%u live=%llu peak=%llu",
               ptr, (unsigned)size, new_ptr, (unsigned)old_size, live, peak);
    }

    return new_ptr;
}

void *memalign_soloader(size_t alignment, size_t size) {
    if (!g_memtrace_on) return memalign(alignment, size);
    void *caller = __builtin_return_address(0);
    const int oversize = (size > MEMALIGN_MAX_BYTES);
    if (oversize) {
        // Hard-denying this request caused immediate NULL-deref crashes in title code.
        // Keep it visible in logs but allow one allocator attempt.
        l_warn("[MEMTRACE] memalign(%u,%u) caller=%p exceeds limit=%u; allowing attempt",
               (unsigned)alignment, (unsigned)size, caller, (unsigned)MEMALIGN_MAX_BYTES);
    }

    void *ptr = memalign(alignment, size);
    unsigned long long live = 0, peak = 0;

    alloc_lock();
    g_memalign_calls++;
    on_alloc_locked(ptr, size, &live, &peak);
    if (oversize && !ptr) {
        g_memalign_denied++;
    }
    alloc_unlock();

    if (!ptr) {
        l_error("[MEMTRACE] memalign(%u,%u) caller=%p failed live=%llu peak=%llu",
                (unsigned)alignment, (unsigned)size, caller, live, peak);
    } else if (size >= LARGE_ALLOC_BYTES) {
        l_warn("[MEMTRACE] memalign(%u,%u) caller=%p -> %p live=%llu peak=%llu",
               (unsigned)alignment, (unsigned)size, caller, ptr, live, peak);
    }
    return ptr;
}

void *valloc_soloader(size_t size) {
    if (!g_memtrace_on) return valloc(size);
    void *ptr = valloc(size);
    unsigned long long live = 0, peak = 0;

    alloc_lock();
    g_valloc_calls++;
    on_alloc_locked(ptr, size, &live, &peak);
    alloc_unlock();

    if (!ptr) {
        l_error("[MEMTRACE] valloc(%u) failed live=%llu peak=%llu", (unsigned)size, live, peak);
    } else if (size >= LARGE_ALLOC_BYTES) {
        l_warn("[MEMTRACE] valloc(%u) -> %p live=%llu peak=%llu", (unsigned)size, ptr, live, peak);
    }
    return ptr;
}

void free_soloader(void *ptr) {
    if (!g_memtrace_on) { free(ptr); return; }
    unsigned long long live = 0, peak = 0;
    size_t old_size = 0;

    alloc_lock();
    g_free_calls++;
    on_free_locked(ptr, &live, &peak, &old_size);
    alloc_unlock();

    if (ptr && old_size >= LARGE_ALLOC_BYTES) {
        l_warn("[MEMTRACE] free(%p) size=%u live=%llu peak=%llu", ptr, (unsigned)old_size, live, peak);
    }

    free(ptr);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    // Prevent giant upfront allocations that can starve nativeInit.
    if (length > MMAP_MAX_BYTES) {
        l_warn("mmap denied: requested %u bytes (limit %u).", (unsigned)length, (unsigned)MMAP_MAX_BYTES);
        errno = ENOMEM;
        return MAP_FAILED;
    }

    void *ret = malloc_soloader(length);
    if (!ret) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    if (fd >= 0) {
        const off_t old = lseek_soloader(fd, 0, SEEK_CUR);
        if (old < 0 || lseek_soloader(fd, offs, SEEK_SET) < 0) {
            free_soloader(ret);
            errno = EINVAL;
            return MAP_FAILED;
        }

        const ssize_t r = read_soloader(fd, ret, length);
        lseek_soloader(fd, old, SEEK_SET);
        if (r < 0) {
            free_soloader(ret);
            errno = EIO;
            return MAP_FAILED;
        }
        if ((size_t)r < length) {
            memset((uint8_t *)ret + r, 0, length - (size_t)r);
        }
    } else {
        memset(ret, 0, length);
    }

    return ret;
}

int munmap(void *addr, size_t length) {
    (void)length;
    if (addr) free_soloader(addr);
    return 0;
}

void mem_stats_snapshot(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    alloc_lock();
    sceClibSnprintf(
        out,
        out_size,
        "alloc=%llu calloc=%llu realloc=%llu memalign=%llu valloc=%llu free=%llu "
        "fail=%llu denied_memalign=%llu req=%llumb live=%llumb peak=%llumb untracked=%llu",
        g_alloc_calls,
        g_calloc_calls,
        g_realloc_calls,
        g_memalign_calls,
        g_valloc_calls,
        g_free_calls,
        g_alloc_fail,
        g_memalign_denied,
        g_requested_bytes / (1024ULL * 1024ULL),
        g_live_bytes / (1024ULL * 1024ULL),
        g_peak_live_bytes / (1024ULL * 1024ULL),
        g_untracked_ptrs
    );
    alloc_unlock();
}
