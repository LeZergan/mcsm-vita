/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>
#include <utime.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// --- OBB file I/O diagnostics ---------------------------------------------
// The engine opens the .obb archive and reads script/resource entries from it
// via read()/lseek(). Track those fds and log the reads so we can see whether
// the OBB entry reads succeed, fail, or return short (the "Couldn't read file"
// boot failure). Throttled to avoid log spam.
#ifdef DEBUG_SOLOADER
static int g_obb_fds[8];
static int g_obb_fd_count = 0;
static void obb_track_fd(const char *path, int fd) {
    if (fd < 0 || !path) return;
    if (!strstr(path, ".obb")) return;
    if (g_obb_fd_count < (int)(sizeof(g_obb_fds) / sizeof(g_obb_fds[0]))) {
        g_obb_fds[g_obb_fd_count++] = fd;
        l_info("[OBBIO] tracking OBB fd=%d (%s)", fd, path);
    }
}
int obb_is_fd(int fd) {
    for (int i = 0; i < g_obb_fd_count; ++i) {
        if (g_obb_fds[i] == fd) return 1;
    }
    return 0;
}
#else
int obb_is_fd(int fd) { (void)fd; return 0; }
#endif

typedef struct SaveDataFd {
    int fd;
    int wrote;
    char path[1024];
} SaveDataFd;

static SaveDataFd g_savedata_fds[16];
static int g_savedata_fd_count = 0;

static int path_is_savedata_bundle(const char *path) {
    if (!path) {
        return 0;
    }
    return strstr(path, "save.bundle") ||
           strstr(path, "slot.bundle") ||
           strstr(path, "saveSlot") ||
           strstr(path, "saveslot") ||
           strstr(path, "autosave") ||
           strstr(path, "user.prop") ||
           strstr(path, "prefs.prop") ||
           /* Also trace the property sets that carry player selections (chosen
            * Jesse model, choice records). Without these in the filter their
            * writes were invisible in the log, which is why "SavePrefs ran but
            * nothing was written" took a full session to spot. */
           strstr(path, "choice.prop") ||
           strstr(path, "choicestats.prop");
}

static int savedata_find_fd(int fd) {
    for (int i = 0; i < g_savedata_fd_count; ++i) {
        if (g_savedata_fds[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static void savedata_track_fd(const char *path, int fd, int flags) {
    if (fd < 0 || !path_is_savedata_bundle(path)) {
        return;
    }
    int existing = savedata_find_fd(fd);
    if (existing >= 0) {
        g_savedata_fds[existing].wrote = 0;
        sceClibSnprintf(g_savedata_fds[existing].path, sizeof(g_savedata_fds[existing].path), "%s", path);
        l_info("SAVEIO open existing fd=%d flags=0x%X path=%s", fd, flags, path);
        return;
    }
    if (g_savedata_fd_count < (int)(sizeof(g_savedata_fds) / sizeof(g_savedata_fds[0]))) {
        SaveDataFd *slot = &g_savedata_fds[g_savedata_fd_count++];
        slot->fd = fd;
        slot->wrote = 0;
        sceClibSnprintf(slot->path, sizeof(slot->path), "%s", path);
    }
    l_info("SAVEIO open fd=%d flags=0x%X path=%s", fd, flags, path);
}

static int savedata_is_fd(int fd) {
    return savedata_find_fd(fd) >= 0;
}

static void savedata_note_write_fd(int fd) {
    int idx = savedata_find_fd(fd);
    if (idx >= 0) {
        g_savedata_fds[idx].wrote = 1;
    }
}

static int savedata_snapshot_fd(int fd, char *path, size_t path_size, int *did_write) {
    int idx = savedata_find_fd(fd);
    if (idx < 0) {
        return 0;
    }
    if (path && path_size > 0) {
        sceClibSnprintf(path, path_size, "%s", g_savedata_fds[idx].path);
    }
    if (did_write) {
        *did_write = g_savedata_fds[idx].wrote;
    }
    return 1;
}

static void savedata_untrack_fd(int fd) {
    for (int i = 0; i < g_savedata_fd_count; ++i) {
        if (g_savedata_fds[i].fd != fd) {
            continue;
        }
        g_savedata_fds[i] = g_savedata_fds[g_savedata_fd_count - 1];
        g_savedata_fd_count--;
        return;
    }
}

/* is_opensl_library_path() and stat_virtual_opensl_library() lived here to make
 * access()/stat() claim libOpenSLES.so existed. Both call sites went away with the
 * OpenSL removal on 2026-07-29 and the functions were left behind as dead code;
 * -Wall found them once warnings were finally switched on. */

static int write_text_file_if_missing(const char *path, const char *content) {
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        l_warn("Android virtual file create failed: %s", path ? path : "(null)");
        return 0;
    }
    fputs(content, fp);
    fclose(fp);
    l_info("Android virtual file created: %s", path);
    return 1;
}

static int write_auxv_file_if_missing(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }

    fp = fopen(path, "wb");
    if (!fp) {
        l_warn("Android virtual auxv create failed: %s", path ? path : "(null)");
        return 0;
    }

    /* 32-bit Linux auxv pairs. FMOD probes this for ARM HWCAP/NEON. */
    const uint32_t auxv[] = {
        16U, (1U << 6) | (1U << 12), /* AT_HWCAP: VFP | NEON */
        0U, 0U,                      /* AT_NULL */
    };
    fwrite(auxv, sizeof(auxv[0]), sizeof(auxv) / sizeof(auxv[0]), fp);
    fclose(fp);
    l_info("Android virtual file created: %s", path);
    return 1;
}

static const char *ensure_android_virtual_file(const char *path) {
    static char local_path[256];
    const char *name = NULL;

    if (!path) {
        return NULL;
    }
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        name = "cpuinfo";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path,
            "Processor\t: ARMv7 Processor rev 4 (v7l)\n"
            "processor\t: 0\n"
            "BogoMIPS\t: 1000.00\n"
            "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls\n"
            "CPU implementer\t: 0x41\n"
            "CPU architecture: 7\n"
            "CPU variant\t: 0x0\n"
            "CPU part\t: 0xc09\n"
            "CPU revision\t: 4\n"
            "Hardware\t: PlayStation Vita\n");
        return local_path;
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        name = "meminfo";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path,
            "MemTotal:         512000 kB\n"
            "MemFree:          256000 kB\n"
            "MemAvailable:     256000 kB\n");
        return local_path;
    }
    if (strcmp(path, "/sys/devices/system/cpu/present") == 0) {
        name = "cpu_present";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path, "0-3\n");
        return local_path;
    }
    if (strcmp(path, "/sys/devices/system/cpu/possible") == 0) {
        name = "cpu_possible";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path, "0-3\n");
        return local_path;
    }
    if (strcmp(path, "/sys/devices/system/cpu/online") == 0) {
        name = "cpu_online";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path, "0-3\n");
        return local_path;
    }
    if (strcmp(path, "/proc/self/auxv") == 0) {
        name = "auxv";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_auxv_file_if_missing(local_path);
        return local_path;
    }
    if (strcmp(path, "/usr/local/ssl/openssl.cnf") == 0 ||
        strcmp(path, "/etc/ssl/openssl.cnf") == 0) {
        name = "openssl.cnf";
        sceClibSnprintf(local_path, sizeof(local_path), DATA_PATH "%s", name);
        write_text_file_if_missing(local_path, "");
        return local_path;
    }
    return NULL;
}

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#define ASSET_VFD_BASE 0x40000000
#define ASSET_VFD_MAX 512
/* Cached raw ttarch fds kept open for pread reuse. RAISED 16->32->96 (2026-07-17):
 * the hardware log shows a PEAK of ~151 concurrently-open virtual archives, so a
 * 32-slot cache left ~119 archives on the slow open()+lseek()+read()+close() path
 * (4 SD syscalls per read). 96 keeps almost the whole steady-state working set on
 * the fast pread path. Combined with the slow-path fd PROMOTION below (a trimmed
 * archive re-caches its fd on the next read instead of paying open/close forever),
 * this removes essentially all redundant open/close churn during loads. The
 * RETRY_TARGET path still trims to 8 if the OS ever refuses a new fd (EMFILE), so
 * raising this is safe — worst case it degrades to the old behavior. */
#define ASSET_VFD_RAW_CACHE_SOFT_MAX 48u   /* LOWERED 96->48 (2026-07-22): 96 cached fds + the
                                            * game's own + concurrent reads overran the OS fd
                                            * limit once Chapter 2+ archives were added (EMFILE ->
                                            * infinite loading). 48 leaves headroom for all
                                            * chapters; the EMFILE trim+retry in the read paths is
                                            * the safety net. Slightly more re-open churn on CH1. */
#define ASSET_VFD_RAW_CACHE_RETRY_TARGET 8u

enum {
    ASSET_VFD_FREE = 0,
    ASSET_VFD_OPEN = 1,
    ASSET_VFD_CLOSING = 2,
};

typedef struct AssetVfd {
    int used;       /* ASSET_VFD_* state; CLOSING rejects new operations. */
    /* 512 (was PATH_MAX=4096) x ASSET_VFD_MAX slots. Real asset paths
     * (ux0:data/mcsm/Android/obb/<pkg>/NNN.ttarch2:<res>) are ~150 chars; 512 keeps a
     * 3x margin. All writes here use sizeof(path), so shrinking cannot overflow —
     * this reclaims ~1.8MB of BSS that was reserved for paths that never occur. */
    char path[512];
    off_t pos;
    off_t length;
    int raw_fd;   /* cached real fd: opened once, reused via pread (HUGE: avoids
                   * an open()+close() per read, which was the 20-min-load killer) */
    unsigned int busy;       /* in-flight open/read/pread operations */
    unsigned int generation; /* protects completions from slot reuse (ABA) */
    unsigned int last_used;
} AssetVfd;

static AssetVfd g_asset_vfds[ASSET_VFD_MAX];
/* Publish one process-wide kernel mutex on first use. A failed mutex allocation
 * permanently selects the yielding fallback, so lock/unlock can never switch
 * mechanisms halfway through a critical section. */
#define ASSET_VFD_MUTEX_UNINITIALIZED (-1)
#define ASSET_VFD_MUTEX_FALLBACK      (-2)
static atomic_int g_asset_vfd_mutex = ATOMIC_VAR_INIT(ASSET_VFD_MUTEX_UNINITIALIZED);
static atomic_flag g_asset_vfd_fallback_lock = ATOMIC_FLAG_INIT;
static unsigned int g_asset_vfd_live_raw = 0;
static unsigned int g_asset_vfd_clock = 0;
#ifdef DEBUG_SOLOADER
static unsigned int g_asset_vfd_open_count = 0;
static unsigned int g_asset_vfd_trim_log_count = 0;
#endif

static int asset_vfd_get_mutex(void) {
    int mutex = atomic_load_explicit(&g_asset_vfd_mutex, memory_order_acquire);
    if (mutex != ASSET_VFD_MUTEX_UNINITIALIZED) {
        return mutex;
    }

    const SceUID created = sceKernelCreateMutex("mcsm_asset_vfd", 0, 0, NULL);
    const int published = created >= 0 ? created : ASSET_VFD_MUTEX_FALLBACK;
    int expected = ASSET_VFD_MUTEX_UNINITIALIZED;
    if (!atomic_compare_exchange_strong_explicit(&g_asset_vfd_mutex,
                                                  &expected,
                                                  published,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        if (created >= 0) {
            sceKernelDeleteMutex(created);
        }
        return expected;
    }
    return published;
}

static void asset_vfd_lock(void) {
    const int mutex = asset_vfd_get_mutex();
    if (mutex >= 0) {
        /* A valid mutex should not fail. Yield and retry instead of proceeding
         * unlocked if the scheduler temporarily interrupts the call. */
        while (sceKernelLockMutex(mutex, 1, NULL) < 0) {
            sceKernelDelayThread(100);
        }
        return;
    }

    while (atomic_flag_test_and_set_explicit(&g_asset_vfd_fallback_lock,
                                              memory_order_acquire)) {
        /* Never burn an entire Vita core waiting for a short metadata section. */
        sceKernelDelayThread(100);
    }
}

static void asset_vfd_unlock(void) {
    const int mutex = atomic_load_explicit(&g_asset_vfd_mutex, memory_order_acquire);
    if (mutex >= 0) {
        sceKernelUnlockMutex(mutex, 1);
        return;
    }
    atomic_flag_clear_explicit(&g_asset_vfd_fallback_lock, memory_order_release);
}

/* Called with the table lock held after the final user of a closing slot leaves.
 * The raw fd is returned for close() outside the critical section. Generation is
 * intentionally retained and incremented on the next allocation. */
static int asset_vfd_finalize_close_locked(int slot) {
    AssetVfd *vfd = &g_asset_vfds[slot];
    if (vfd->used != ASSET_VFD_CLOSING || vfd->busy != 0) {
        return -1;
    }

    const int raw_fd = vfd->raw_fd;
    if (raw_fd >= 0 && g_asset_vfd_live_raw > 0) {
        g_asset_vfd_live_raw -= 1;
    }
    vfd->used = ASSET_VFD_FREE;
    vfd->path[0] = '\0';
    vfd->pos = 0;
    vfd->length = 0;
    vfd->raw_fd = -1;
    vfd->busy = 0;
    vfd->last_used = 0;
    return raw_fd;
}

static int asset_vfd_release_locked(int slot, unsigned int generation) {
    AssetVfd *vfd = &g_asset_vfds[slot];
    if (vfd->generation != generation || vfd->busy == 0) {
        return -1;
    }
    vfd->busy -= 1;
    return asset_vfd_finalize_close_locked(slot);
}

static int asset_vfd_slot(int fd) {
    if (fd < ASSET_VFD_BASE) {
        return -1;
    }
    int slot = fd - ASSET_VFD_BASE;
    if (slot < 0 || slot >= ASSET_VFD_MAX) {
        return -1;
    }
    return slot;
}

void asset_vfd_trim_cached_fds(unsigned int target_open) {
    for (;;) {
        int close_fd = -1;
#ifdef DEBUG_SOLOADER
        char close_path[PATH_MAX];
        close_path[0] = '\0';
#endif

        asset_vfd_lock();
        if (g_asset_vfd_live_raw <= target_open) {
            asset_vfd_unlock();
            return;
        }

        int best = -1;
        unsigned int best_use = UINT_MAX;
        for (int i = 0; i < ASSET_VFD_MAX; ++i) {
            if (g_asset_vfds[i].used != ASSET_VFD_OPEN ||
                g_asset_vfds[i].raw_fd < 0 ||
                g_asset_vfds[i].busy != 0) {
                continue;
            }
            if (g_asset_vfds[i].last_used < best_use) {
                best = i;
                best_use = g_asset_vfds[i].last_used;
            }
        }

        if (best < 0) {
            asset_vfd_unlock();
            return;
        }

        close_fd = g_asset_vfds[best].raw_fd;
#ifdef DEBUG_SOLOADER
        sceClibSnprintf(close_path, sizeof(close_path), "%s", g_asset_vfds[best].path);
#endif
        g_asset_vfds[best].raw_fd = -1;
        if (g_asset_vfd_live_raw > 0) {
            g_asset_vfd_live_raw -= 1;
        }
        asset_vfd_unlock();

        if (close_fd >= 0) {
            close(close_fd);
#ifdef DEBUG_SOLOADER
            if (g_asset_vfd_trim_log_count++ < 24u) {
                l_info("[ASSETVFD] trim cached raw fd=%d target=%u path=%s",
                       close_fd, target_open, close_path);
            }
#endif
        }
    }
}

static int asset_vfd_snapshot(int fd, char *path, size_t path_size, off_t *pos, off_t *length) {
    int slot = asset_vfd_slot(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

    asset_vfd_lock();
    if (g_asset_vfds[slot].used != ASSET_VFD_OPEN) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }

    if (path && path_size > 0) {
        sceClibSnprintf(path, path_size, "%s", g_asset_vfds[slot].path);
    }
    if (pos) {
        *pos = g_asset_vfds[slot].pos;
    }
    if (length) {
        *length = g_asset_vfds[slot].length;
    }
    asset_vfd_unlock();
    return slot;
}

int asset_vfd_open(const char *path, off_t length) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(path) >= sizeof(g_asset_vfds[0].path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);

    asset_vfd_lock();
    int slot = -1;
    for (int i = 0; i < ASSET_VFD_MAX; ++i) {
        if (g_asset_vfds[i].used == ASSET_VFD_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        asset_vfd_unlock();
        errno = EMFILE;
        l_error("[ASSETVFD] no free virtual fd slots for %s", path);
        return -1;
    }

    AssetVfd *vfd = &g_asset_vfds[slot];
    vfd->generation += 1;
    if (vfd->generation == 0) {
        vfd->generation = 1;
    }
    const unsigned int generation = vfd->generation;
    vfd->used = ASSET_VFD_OPEN;
    vfd->pos = 0;
    vfd->length = length >= 0 ? length : 0;
    sceClibSnprintf(vfd->path, sizeof(vfd->path), "%s", path);
    vfd->raw_fd = -1;
    /* Reserve one operation while the slow memory-card open runs unlocked. */
    vfd->busy = 1;
    vfd->last_used = ++g_asset_vfd_clock;
    int fd = ASSET_VFD_BASE + slot;
#ifdef DEBUG_SOLOADER
    unsigned int open_count = ++g_asset_vfd_open_count;
#endif
    asset_vfd_unlock();

    /* Never hold the global table lock across memory-card I/O. The generation
     * check below prevents a late result from attaching to a recycled slot. */
    int raw_fd = open(path, O_RDONLY);
    if (raw_fd < 0 && errno == EMFILE) {
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_RETRY_TARGET);
        raw_fd = open(path, O_RDONLY);
    }
    const int raw_errno = raw_fd < 0 ? errno : 0;
    int attached = 0;
    int deferred_close_fd = -1;
    asset_vfd_lock();
    vfd = &g_asset_vfds[slot];
    if (vfd->used == ASSET_VFD_OPEN &&
        vfd->generation == generation &&
        vfd->raw_fd < 0 &&
        raw_fd >= 0) {
        vfd->raw_fd = raw_fd;
        g_asset_vfd_live_raw += 1;
        vfd->last_used = ++g_asset_vfd_clock;
        attached = 1;
    }
    deferred_close_fd = asset_vfd_release_locked(slot, generation);
    asset_vfd_unlock();

    if (raw_fd >= 0 && !attached) {
        close(raw_fd);
    }
    if (deferred_close_fd >= 0) {
        close(deferred_close_fd);
    }

    if (raw_fd < 0) {
        l_warn("[ASSETVFD] raw open failed fd=%d errno=%d path=%s", fd, raw_errno, path);
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_RETRY_TARGET);
        errno = raw_errno;
    } else {
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }

    /* Gate to the first 64 opens (matching the read/pread logs below): during a
     * scene load hundreds of archives open and l_info is compiled-in for logging
     * builds, so an ungated vsnprintf+buffered write per open self-slows the load.
     * Failures are already surfaced by the l_warn above. */
#ifdef DEBUG_SOLOADER
    if (open_count <= 64u) {
        l_info("[ASSETVFD] open #%u fd=%d len=%lld path=%s",
               open_count, fd, (long long)length, path);
    }
#endif
    return fd;
}

int asset_vfd_is(int fd) {
    /* Virtual descriptors live in a range no native fd can reach. The operation
     * itself validates slot liveness, so dispatch does not need a spinlock too. */
    return asset_vfd_slot(fd) >= 0;
}

__attribute__((noinline))
static ssize_t asset_vfd_cold_pread(int slot, int fd, void *buf, size_t count,
                                    off_t offset, unsigned int generation,
                                    const char *path, const char *operation,
                                    int *promoted_out) {
    if (promoted_out) {
        *promoted_out = 0;
    }
    int tmp = open(path, O_RDONLY);
    if (tmp < 0 && errno == EMFILE) {
        /* Preserve the proven Chapter 2+ recovery path: make fd headroom and retry. */
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_RETRY_TARGET);
        tmp = open(path, O_RDONLY);
    }
    if (tmp < 0) {
        l_warn("[ASSETVFD] %s fd=%d open failed path=%s errno=%d",
               operation, fd, path, errno);
        return -1;
    }

    const ssize_t ret = pread(tmp, buf, count, offset);
    const int saved_errno = errno;
    int promoted = 0;
    asset_vfd_lock();
    if (g_asset_vfds[slot].used == ASSET_VFD_OPEN &&
        g_asset_vfds[slot].generation == generation &&
        g_asset_vfds[slot].raw_fd < 0) {
        g_asset_vfds[slot].raw_fd = tmp;
        g_asset_vfd_live_raw += 1;
        g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
        promoted = 1;
    }
    asset_vfd_unlock();
    if (!promoted) {
        close(tmp);
    }
    if (promoted_out) {
        *promoted_out = promoted;
    }
    errno = saved_errno;
    return ret;
}

ssize_t asset_vfd_read(int fd, void *buf, size_t count) {
    const int slot = asset_vfd_slot(fd);
    off_t pos = 0;
    off_t length = 0;
    unsigned int generation = 0;
    char path[sizeof(g_asset_vfds[0].path)];
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

    asset_vfd_lock();
    if (g_asset_vfds[slot].used != ASSET_VFD_OPEN) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }
    pos = g_asset_vfds[slot].pos;
    length = g_asset_vfds[slot].length;
    if (count == 0 || pos >= length) {
        asset_vfd_unlock();
        return 0;
    }
    if ((off_t)count > length - pos) {
        count = (size_t)(length - pos);
    }
    generation = g_asset_vfds[slot].generation;
    const int raw_fd = g_asset_vfds[slot].raw_fd;
    if (raw_fd < 0) {
        sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot].path);
    }
    /* Every path, including a cold open, owns a reference until completion. */
    g_asset_vfds[slot].busy += 1;
    g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
    asset_vfd_unlock();

    ssize_t ret;
    int promoted = 0;
    if (raw_fd >= 0) {
        ret = pread(raw_fd, buf, count, pos);   /* cached fd: no open/close/seek */
    } else {
        ret = asset_vfd_cold_pread(slot, fd, buf, count, pos, generation,
                                   path, "read", &promoted);
    }
    const int saved_errno = errno;

    int deferred_close_fd = -1;
    asset_vfd_lock();
    if (g_asset_vfds[slot].generation == generation) {
        if (ret > 0 && g_asset_vfds[slot].used == ASSET_VFD_OPEN) {
            g_asset_vfds[slot].pos = pos + ret;
        }
        deferred_close_fd = asset_vfd_release_locked(slot, generation);
    }
    asset_vfd_unlock();
    if (deferred_close_fd >= 0) {
        close(deferred_close_fd);
    }
    if (promoted) {
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }
    errno = saved_errno;

#ifdef DEBUG_SOLOADER
    static unsigned int log_count = 0;
    if (log_count++ < 64 || ret < 0) {
        l_info("[ASSETVFD] read fd=%d off=%lld count=%u -> %d",
               fd, (long long)pos, (unsigned)count, (int)ret);
    }
#endif
    return ret;
}

ssize_t asset_vfd_pread(int fd, void *buf, size_t count, off_t offset) {
    const int slot = asset_vfd_slot(fd);
    off_t length = 0;
    unsigned int generation = 0;
    char path[sizeof(g_asset_vfds[0].path)];
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

    asset_vfd_lock();
    if (g_asset_vfds[slot].used != ASSET_VFD_OPEN) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }
    length = g_asset_vfds[slot].length;
    if (count == 0 || offset >= length) {
        asset_vfd_unlock();
        return 0;
    }
    if ((off_t)count > length - offset) {
        count = (size_t)(length - offset);
    }
    generation = g_asset_vfds[slot].generation;
    const int raw_fd = g_asset_vfds[slot].raw_fd;
    if (raw_fd < 0) {
        sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot].path);
    }
    g_asset_vfds[slot].busy += 1;
    g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
    asset_vfd_unlock();

    ssize_t ret;
    int promoted = 0;
    if (raw_fd >= 0) {
        ret = pread(raw_fd, buf, count, offset);   /* cached fd: no open/close/seek */
    } else {
        ret = asset_vfd_cold_pread(slot, fd, buf, count, offset, generation,
                                   path, "pread", &promoted);
    }
    const int saved_errno = errno;

    int deferred_close_fd = -1;
    asset_vfd_lock();
    if (g_asset_vfds[slot].generation == generation) {
        deferred_close_fd = asset_vfd_release_locked(slot, generation);
    }
    asset_vfd_unlock();
    if (deferred_close_fd >= 0) {
        close(deferred_close_fd);
    }
    if (promoted) {
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }
    errno = saved_errno;

#ifdef DEBUG_SOLOADER
    static unsigned int log_count = 0;
    if (log_count++ < 64 || ret < 0) {
        l_info("[ASSETVFD] pread fd=%d off=%lld count=%u -> %d",
               fd, (long long)offset, (unsigned)count, (int)ret);
    }
#endif
    return ret;
}

off_t asset_vfd_lseek(int fd, off_t offset, int whence) {
    int slot = asset_vfd_slot(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

    asset_vfd_lock();
    if (g_asset_vfds[slot].used != ASSET_VFD_OPEN) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }

    off_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = g_asset_vfds[slot].pos;
    } else if (whence == SEEK_END) {
        base = g_asset_vfds[slot].length;
    } else {
        asset_vfd_unlock();
        errno = EINVAL;
        return -1;
    }

    off_t new_pos = base + offset;
    if (new_pos < 0) {
        asset_vfd_unlock();
        errno = EINVAL;
        return -1;
    }
    g_asset_vfds[slot].pos = new_pos;
    asset_vfd_unlock();
    return new_pos;
}

int asset_vfd_fstat(int fd, stat64_bionic *buf) {
    off_t length = 0;
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (asset_vfd_snapshot(fd, NULL, 0, NULL, &length) < 0) {
        return -1;
    }

    /* Fabricate directly from the cached length — no per-fstat path-based stat()
     * (a memory-card directory traversal). st_size is the only field the engine
     * needs, and it was always overwritten with `length` anyway. */
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = 0100000 | 0444;
    buf->st_nlink = 1;   /* a valid file has >=1 link; 0 can read as "unlinked" */
    buf->st_size = length;
    buf->st_blksize = 4096;
    buf->st_blocks = (length + 511) / 512;

    l_debug("[ASSETVFD] fstat fd=%d size=%lld", fd, (long long)buf->st_size);
    return 0;
}

int asset_vfd_close(int fd) {
    int slot = asset_vfd_slot(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

#ifdef DEBUG_SOLOADER
    char path[PATH_MAX];
#endif
    asset_vfd_lock();
    if (g_asset_vfds[slot].used != ASSET_VFD_OPEN) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }
#ifdef DEBUG_SOLOADER
    sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot].path);
#endif
    /* Reject new operations immediately, but keep the slot and its raw fd alive
     * until every in-flight pread finishes. This prevents close/reuse races. */
    g_asset_vfds[slot].used = ASSET_VFD_CLOSING;
    const int deferred = g_asset_vfds[slot].busy != 0;
    const int raw_fd = asset_vfd_finalize_close_locked(slot);
    asset_vfd_unlock();
    (void)deferred;

    int close_rc = 0;
    int close_errno = 0;
    if (raw_fd >= 0) {
        close_rc = close(raw_fd);   /* release the cached real fd */
        if (close_rc < 0) {
            close_errno = errno;
        }
    }

#ifdef DEBUG_SOLOADER
    l_info("[ASSETVFD] close fd=%d deferred=%d path=%s", fd, deferred, path);
#endif
    if (close_rc < 0) {
        errno = close_errno;
    }
    return close_rc;
}

static const char *remap_android_path(const char *path) {
    static char path_buf[1024];
    const char *prefixes[] = {
        "/sdcard/Android/obb/com.telltalegames.minecraft100/",
        "/storage/emulated/0/Android/obb/com.telltalegames.minecraft100/",
        "/mnt/sdcard/Android/obb/com.telltalegames.minecraft100/",
        "/sdcard/Android/data/com.telltalegames.minecraft100/files/",
        "/storage/emulated/0/Android/data/com.telltalegames.minecraft100/files/",
        "/data/data/com.telltalegames.minecraft100/files/",
    };

    if (!path) {
        return path;
    }

    for (int i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t prefix_len = strlen(prefixes[i]);
        if (strncmp(path, prefixes[i], prefix_len) == 0) {
            snprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + prefix_len);
            return path_buf;
        }
    }

    /* PERF (2026-07-17): these prefixes are compile-time constants — building
     * them with snprintf on EVERY remap (every fopen/open/stat/access during
     * scene loads) was pure waste. Hoist to static literals + sizeof length. */
    {
        static const char vita_obb_prefix[] = DATA_PATH "Android/obb/com.telltalegames.minecraft100/";
        size_t prefix_len = sizeof(vita_obb_prefix) - 1;
        if (strncmp(path, vita_obb_prefix, prefix_len) == 0) {
            sceClibSnprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + prefix_len);
            return path_buf;
        }
    }

    {
        static const char vita_data_prefix[] = DATA_PATH "Android/data/com.telltalegames.minecraft100/files/";
        size_t prefix_len = sizeof(vita_data_prefix) - 1;
        if (strncmp(path, vita_data_prefix, prefix_len) == 0) {
            sceClibSnprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + prefix_len);
            return path_buf;
        }
    }

    if (str_starts_with(path, "/sdcard/")) {
        snprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + strlen("/sdcard/"));
        return path_buf;
    }

    if (str_starts_with(path, "/storage/emulated/0/")) {
        snprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + strlen("/storage/emulated/0/"));
        return path_buf;
    }

    if (str_starts_with(path, "/mnt/sdcard/")) {
        snprintf(path_buf, sizeof(path_buf), "%s%s", DATA_PATH, path + strlen("/mnt/sdcard/"));
        return path_buf;
    }

    /* Collapse accidental "//" (the engine's <Temp> logical resolution emits e.g.
     * "ux0:data/mcsm//Temp/choice.prop"; sceIo does NOT normalize "//", so opening
     * OR stat/access'ing an existing file at that path returns -1. That broke the
     * crowd-choice read — ResourceExists('choice.prop') saw the double-slash path
     * as missing, so the "% of players chose" stats never loaded, even though the
     * real 114KB choice.prop is present under Temp/. Only rewrites when "//" is
     * actually present, so normal paths pay just one strstr. */
    if (strstr(path, "//")) {
        char *w = path_buf;
        char *end = path_buf + sizeof(path_buf) - 1;
        for (const char *r = path; *r && w < end; ++r) {
            if (r[0] == '/' && r[1] == '/') continue;   /* drop the first of each "//" */
            *w++ = *r;
        }
        *w = '\0';
        return path_buf;
    }

    return path;
}

static void ensure_parent_dirs_for_path(const char *path, mode_t mode) {
    if (!path || !*path) {
        return;
    }

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) {
        return;
    }
    *last_slash = '\0';

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    filename = remap_android_path(filename);

    if (mode && (strchr(mode, 'w') || strchr(mode, 'a'))) {
        ensure_parent_dirs_for_path(filename, 0777);
    }

    const char *virtual_file = ensure_android_virtual_file(filename);
    if (virtual_file) {
        filename = virtual_file;
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    path = remap_android_path(path);

    const char *virtual_file = ensure_android_virtual_file(path);
    if (virtual_file) {
        path = virtual_file;
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        ensure_parent_dirs_for_path(path, 0777);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret < 0 && errno == EMFILE) {
        /* fd table exhausted by the hot ttarch archive fd-cache. Free some cached
         * archive fds and retry, so a non-archive open — e.g. the crowd choice.prop
         * read that the "% of players chose" screen needs — doesn't just fail. */
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_RETRY_TARGET);
        ret = open(path, oflag, mode);
    }
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i errno=%d", path, oflag, ret, errno);
#ifdef DEBUG_SOLOADER
    obb_track_fd(path, ret);
#endif
    savedata_track_fd(path, ret, oflag);
    return ret;
}

/* SAVE-FILE FIX (2026-06-23): mkdir/rename/unlink/remove/access were bound to raw
 * libc in dynlib.c, so they ran on the engine's Android paths
 * (/data/data/com.telltalegames.minecraft100/files/..., /sdcard/...) which do NOT
 * exist on Vita. Result: the save directory was never created and the
 * write-temp-then-rename save commit failed, so starting a new episode could not
 * persist its session and the engine aborted back to the menu (the confirm->load->
 * back-to-character-select loop). These wrappers remap the path exactly like
 * open_soloader; mkdir also creates missing parent dirs (Vita won't auto-create). */
int mkdir_soloader(const char *path, mode_t mode) {
    const char *rp = remap_android_path(path);
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", rp);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    /* mkdir -p: create each parent component (errors like EEXIST are ignored). */
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    int r = mkdir(tmp, mode);
    if (r != 0 && errno == EEXIST) {
        r = 0;
    }
    if (r == 0) {
        l_debug("mkdir(%s) ok", tmp);
    } else {
        l_warn("mkdir(%s) failed errno=%d", tmp, errno);
    }
    return r;
}

int rename_soloader(const char *oldp, const char *newp) {
    /* remap_android_path returns a shared static buffer: copy the first result
     * before remapping the second, or both args would alias. */
    char a[1024];
    snprintf(a, sizeof(a), "%s", remap_android_path(oldp));
    const char *b = remap_android_path(newp);
    ensure_parent_dirs_for_path(b, 0777);
    int r = rename(a, b);
    if (r == 0) {
        l_debug("rename(%s -> %s) ok", a, b);
        if (path_is_savedata_bundle(a) || path_is_savedata_bundle(b)) {
            l_info("SAVEIO rename commit %s -> %s", a, b);
        }
    } else {
        l_warn("rename(%s -> %s) failed errno=%d", a, b, errno);
    }
    return r;
}

int unlink_soloader(const char *path) {
    const char *rp = remap_android_path(path);
    int r = unlink(rp);
    if (r != 0) {
        l_debug("unlink(%s) errno=%d", rp, errno);
    }
    return r;
}

int remove_soloader(const char *path) {
    const char *rp = remap_android_path(path);
    int r = remove(rp);
    if (r != 0) {
        l_debug("remove(%s) errno=%d", rp, errno);
    }
    return r;
}

int rmdir_soloader(const char *path) {
    return rmdir(remap_android_path(path));
}

int access_soloader(const char *path, int amode) {
    /* OpenSL removed 2026-07-29: dlopen("libOpenSLES.so") now returns NULL, so
     * access()/stat() must agree. Reporting the file as present here and then
     * failing the dlopen is worse than either answer alone -- it lets a probe
     * select a backend that cannot then be loaded. */
    const char *virtual_file = ensure_android_virtual_file(path);
    if (virtual_file) {
        return access(virtual_file, amode);
    }
    return access(remap_android_path(path), amode);
}

int chmod_soloader(const char *path, mode_t mode) {
    const char *rp = remap_android_path(path);
    int r = chmod(rp, mode);
    if (r != 0) {
        l_debug("chmod(%s) errno=%d", rp, errno);
    }
    return r;
}

int truncate_soloader(const char *path, off_t length) {
    const char *rp = remap_android_path(path);
    int r = truncate(rp, length);
    if (r != 0) {
        l_warn("truncate(%s, %lld) failed errno=%d", rp, (long long)length, errno);
    } else if (path_is_savedata_bundle(rp)) {
        l_info("SAVEIO truncate path=%s len=%lld", rp, (long long)length);
    }
    return r;
}

int ftruncate_soloader(int fd, off_t length) {
    int r = ftruncate(fd, length);
    if (savedata_is_fd(fd)) {
        if (r == 0) {
            savedata_note_write_fd(fd);
            l_info("SAVEIO ftruncate fd=%d len=%lld", fd, (long long)length);
        } else {
            l_warn("SAVEIO ftruncate fd=%d len=%lld failed errno=%d",
                   fd, (long long)length, errno);
        }
    }
    return r;
}

int lstat_soloader(const char *path, stat64_bionic *buf) {
    path = remap_android_path(path);

    struct stat st;
    int res = lstat(path, &st);
    if (res == 0) {
        stat_newlib_to_bionic(&st, buf);
    }
    l_debug("lstat(%s): %i size=%lld", path, res, res == 0 ? (long long)buf->st_size : -1LL);
    return res;
}

char *realpath_soloader(const char *path, char *resolved_path) {
    const char *rp = remap_android_path(path);
    char *ret = realpath(rp, resolved_path);
    if (!ret && resolved_path && rp) {
        snprintf(resolved_path, PATH_MAX, "%s", rp);
        ret = resolved_path;
    }
    if (!ret) {
        l_debug("realpath(%s) failed errno=%d", rp ? rp : "(null)", errno);
    }
    return ret;
}

int chdir_soloader(const char *path) {
    const char *rp = remap_android_path(path);
    int r = chdir(rp);
    if (r != 0) {
        l_debug("chdir(%s) errno=%d", rp ? rp : "(null)", errno);
    }
    return r;
}

int utime_soloader(const char *path, const struct utimbuf *times) {
    const char *rp = remap_android_path(path);
    int r = utime(rp, times);
    if (r != 0) {
        l_debug("utime(%s) errno=%d", rp ? rp : "(null)", errno);
    } else if (path_is_savedata_bundle(rp)) {
        l_info("SAVEIO utime path=%s", rp);
    }
    return r;
}

ssize_t read_soloader(int fd, void *buf, size_t count) {
    if (asset_vfd_is(fd)) {
        return asset_vfd_read(fd, buf, count);
    }

    ssize_t r = read(fd, buf, count);
#ifdef DEBUG_SOLOADER
    if (obb_is_fd(fd)) {
        static int n = 0;
        // Always log large reads (entry data, not just header/dir parsing); throttle small ones.
        if (count > 64 || n++ < 100) {
            l_info("[OBBIO] read(fd=%d, count=%u) -> %d", fd, (unsigned)count, (int)r);
        }
    }
#endif
    return r;
}

ssize_t write_soloader(int fd, const void *buf, size_t count) {
    ssize_t r = write(fd, buf, count);
    if (savedata_is_fd(fd)) {
        if (r >= 0) {
            if (r > 0) {
                savedata_note_write_fd(fd);
            }
            l_info("SAVEIO write fd=%d count=%u -> %d", fd, (unsigned)count, (int)r);
        } else {
            l_warn("SAVEIO write fd=%d count=%u failed errno=%d", fd, (unsigned)count, errno);
        }
    }
    return r;
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    if (asset_vfd_is(fd)) {
        return asset_vfd_lseek(fd, offset, whence);
    }

    off_t r = lseek(fd, offset, whence);
#ifdef DEBUG_SOLOADER
    if (obb_is_fd(fd)) {
        static int n = 0;
        // Always log deep seeks (into the 813MB body where entries live).
        if (offset > 8192 || n++ < 100) {
            l_info("[OBBIO] lseek(fd=%d, off=%lld, whence=%d) -> %lld",
                   fd, (long long)offset, whence, (long long)r);
        }
    }
#endif
    return r;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    if (asset_vfd_is(fd)) {
        return asset_vfd_fstat(fd, buf);
    }

    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i size=%lld", fd, res, res == 0 ? (long long)buf->st_size : -1LL);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {

    path = remap_android_path(path);

    const char *virtual_file = ensure_android_virtual_file(path);
    if (virtual_file) {
        path = virtual_file;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i size=%lld", path, res, res == 0 ? (long long)buf->st_size : -1LL);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    if (asset_vfd_is(fd)) {
        return asset_vfd_close(fd);
    }

    char savedata_path[1024];
    int did_write = 0;
    int is_savedata = savedata_snapshot_fd(fd, savedata_path, sizeof(savedata_path), &did_write);
    if (is_savedata) {
        /* Untrack before close: once close returns another thread may immediately
         * reuse this integer fd, and late cleanup would delete the new record. */
        savedata_untrack_fd(fd);
    }
    /* ★ 2026-08-06 — FSYNC RESTORED.
     *
     * 1.10 dropped this on the grounds that Telltale commits saves through a
     * temp-file/rename flow and close() already releases the file. On a console
     * that is only true if the process gets to exit cleanly. This one does not:
     * players are force-quitting from the LiveArea/PS menu whenever the game
     * wedges, which is precisely when unflushed memory-card writes are lost.
     * That is the "choices survive two or three reboots and then vanish, then
     * the next set of choices does the same" report -- writes landing sometimes
     * and not others, exactly the signature of a missing durability barrier.
     *
     * This is NOT the unconditional barrier the removal describes: it fires only
     * for a fd that (a) is tracked savedata and (b) actually wrote. A read-only
     * open, and the resource probes the game does constantly, never reach it. */
    if (is_savedata && did_write) {
        int frc = fsync(fd);
        l_info("SAVEIO fsync before close fd=%d rc=%d errno=%d", fd, frc, errno);
    }
    int ret = close(fd);
    if (is_savedata) {
        l_info("SAVEIO close fd=%d rc=%d errno=%d", fd, ret, errno);
        /* 2026-07-02: the "visible mirror" (copying the raw save bundle over
         * slot.bundle / saveSlot1.bundle) is REMOVED. slot bundles are tiny
         * metadata bundles (metadata_slot.prop), not save data; the mirror
         * corrupted them. With the Licensed fix SaveLoad.lua maintains the
         * real <User>/saveSlot1.bundle + sub-bundles itself. */
        (void)did_write;
        (void)savedata_path;
    }
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    const char *rp = remap_android_path(_pathname);
    DIR* ret = opendir(rp);
    l_debug("opendir(\"%s\"): %p", rp, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
