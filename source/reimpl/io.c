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

typedef struct SaveDataFd {
    int fd;
    int flags;
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
           strstr(path, "prefs.prop");
}

static int savedata_flags_can_write(int flags) {
    return (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
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
        g_savedata_fds[existing].flags = flags;
        g_savedata_fds[existing].wrote = 0;
        sceClibSnprintf(g_savedata_fds[existing].path, sizeof(g_savedata_fds[existing].path), "%s", path);
        l_info("SAVEIO open existing fd=%d flags=0x%X path=%s", fd, flags, path);
        return;
    }
    if (g_savedata_fd_count < (int)(sizeof(g_savedata_fds) / sizeof(g_savedata_fds[0]))) {
        SaveDataFd *slot = &g_savedata_fds[g_savedata_fd_count++];
        slot->fd = fd;
        slot->flags = flags;
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

static int savedata_snapshot_fd(int fd, char *path, size_t path_size, int *should_flush, int *did_write) {
    int idx = savedata_find_fd(fd);
    if (idx < 0) {
        return 0;
    }
    if (path && path_size > 0) {
        sceClibSnprintf(path, path_size, "%s", g_savedata_fds[idx].path);
    }
    if (should_flush) {
        *should_flush = g_savedata_fds[idx].wrote || savedata_flags_can_write(g_savedata_fds[idx].flags);
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

static int is_opensl_library_path(const char *path) {
    if (!path) {
        return 0;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "libOpenSLES.so") == 0;
}

static int stat_virtual_opensl_library(stat64_bionic *buf) {
    if (buf) {
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFREG | 0444;
        buf->st_nlink = 1;
        buf->st_size = 1;
    }
    return 0;
}

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
#define ASSET_VFD_RAW_CACHE_SOFT_MAX 96u
#define ASSET_VFD_RAW_CACHE_RETRY_TARGET 8u

typedef struct AssetVfd {
    int used;
    /* 512 (was PATH_MAX=4096) x ASSET_VFD_MAX slots. Real asset paths
     * (ux0:data/mcsm/Android/obb/.../*.ttarch2:<res>) are ~150 chars; 512 keeps a
     * 3x margin. All writes here use sizeof(path), so shrinking cannot overflow —
     * this reclaims ~1.8MB of BSS that was reserved for paths that never occur. */
    char path[512];
    off_t pos;
    off_t length;
    int raw_fd;   /* cached real fd: opened once, reused via pread (HUGE: avoids
                   * an open()+close() per read, which was the 20-min-load killer) */
    unsigned int busy;
    unsigned int last_used;
} AssetVfd;

static AssetVfd g_asset_vfds[ASSET_VFD_MAX];
static atomic_flag g_asset_vfd_lock = ATOMIC_FLAG_INIT;
static unsigned int g_asset_vfd_open_count = 0;
static unsigned int g_asset_vfd_live_raw = 0;
static unsigned int g_asset_vfd_clock = 0;
static unsigned int g_asset_vfd_trim_log_count = 0;

static void asset_vfd_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_asset_vfd_lock, memory_order_acquire)) {
    }
}

static void asset_vfd_unlock(void) {
    atomic_flag_clear_explicit(&g_asset_vfd_lock, memory_order_release);
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
        char close_path[PATH_MAX];
        close_path[0] = '\0';

        asset_vfd_lock();
        if (g_asset_vfd_live_raw <= target_open) {
            asset_vfd_unlock();
            return;
        }

        int best = -1;
        unsigned int best_use = UINT_MAX;
        for (int i = 0; i < ASSET_VFD_MAX; ++i) {
            if (!g_asset_vfds[i].used || g_asset_vfds[i].raw_fd < 0 || g_asset_vfds[i].busy != 0) {
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
        sceClibSnprintf(close_path, sizeof(close_path), "%s", g_asset_vfds[best].path);
        g_asset_vfds[best].raw_fd = -1;
        if (g_asset_vfd_live_raw > 0) {
            g_asset_vfd_live_raw -= 1;
        }
        asset_vfd_unlock();

        if (close_fd >= 0) {
            close(close_fd);
            if (g_asset_vfd_trim_log_count++ < 24u) {
                l_info("[ASSETVFD] trim cached raw fd=%d target=%u path=%s",
                       close_fd, target_open, close_path);
            }
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
    if (!g_asset_vfds[slot].used) {
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
    if (strlen(path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);

    asset_vfd_lock();
    int slot = -1;
    for (int i = 0; i < ASSET_VFD_MAX; ++i) {
        if (!g_asset_vfds[i].used) {
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

    g_asset_vfds[slot].used = 1;
    g_asset_vfds[slot].pos = 0;
    g_asset_vfds[slot].length = length >= 0 ? length : 0;
    sceClibSnprintf(g_asset_vfds[slot].path, sizeof(g_asset_vfds[slot].path), "%s", path);
    /* Open the real file ONCE here and keep it; reads use pread on this cached fd
     * instead of open()+close() per read (that was making loads take ~20 min). */
    g_asset_vfds[slot].raw_fd = open(path, O_RDONLY);
    g_asset_vfds[slot].busy = 0;
    g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
    int raw_fd = g_asset_vfds[slot].raw_fd;
    if (raw_fd >= 0) {
        g_asset_vfd_live_raw += 1;
    }
    int fd = ASSET_VFD_BASE + slot;
    unsigned int open_count = ++g_asset_vfd_open_count;
    asset_vfd_unlock();

    if (raw_fd < 0) {
        int err = errno;
        l_warn("[ASSETVFD] raw open failed fd=%d errno=%d path=%s", fd, err, path);
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_RETRY_TARGET);
        errno = err;
    } else {
        asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }

    /* Gate to the first 64 opens (matching the read/pread logs below): during a
     * scene load hundreds of archives open and l_info is compiled-in for logging
     * builds, so an ungated vsnprintf+buffered write per open self-slows the load.
     * Failures are already surfaced by the l_warn above. */
    if (open_count <= 64u) {
        l_info("[ASSETVFD] open #%u fd=%d len=%lld path=%s",
               open_count, fd, (long long)length, path);
    }
    return fd;
}

int asset_vfd_is(int fd) {
    int slot = asset_vfd_slot(fd);
    if (slot < 0) {
        return 0;
    }

    asset_vfd_lock();
    int used = g_asset_vfds[slot].used;
    asset_vfd_unlock();
    return used;
}

ssize_t asset_vfd_read(int fd, void *buf, size_t count) {
    char path[PATH_MAX];
    off_t pos = 0;
    off_t length = 0;
    int slot = asset_vfd_snapshot(fd, NULL, 0, &pos, &length);
    if (slot < 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (pos >= length) {
        return 0;
    }
    if ((off_t)count > length - pos) {
        count = (size_t)(length - pos);
    }

    asset_vfd_lock();
    int raw_fd = -1;
    if (g_asset_vfds[slot].used && g_asset_vfds[slot].raw_fd >= 0) {
        raw_fd = g_asset_vfds[slot].raw_fd;
        g_asset_vfds[slot].busy += 1;
        g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
    }
    asset_vfd_unlock();

    ssize_t ret;
    if (raw_fd >= 0) {
        ret = pread(raw_fd, buf, count, pos);   /* cached fd: no open/close/seek */
    } else {
        /* Cold path only: the fast pread path above never needs the archive path,
         * so the snapshot skipped the snprintf — fetch it here just before open(). */
        asset_vfd_lock();
        int ok = g_asset_vfds[slot].used;
        if (ok) sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot].path);
        asset_vfd_unlock();
        if (!ok) { errno = EBADF; return -1; }
        int tmp = open(path, O_RDONLY);
        if (tmp < 0) {
            l_warn("[ASSETVFD] read fd=%d open failed path=%s errno=%d", fd, path, errno);
            return -1;
        }
        ret = pread(tmp, buf, count, pos);
        int e = errno;
        /* PROMOTE (2026-07-17): keep this fd cached in the slot so EVERY subsequent
         * read hits the fast pread path — previously a trimmed archive paid
         * open()+close() on every read for the rest of the session. */
        int promoted = 0;
        asset_vfd_lock();
        if (g_asset_vfds[slot].used && g_asset_vfds[slot].raw_fd < 0) {
            g_asset_vfds[slot].raw_fd = tmp;
            g_asset_vfd_live_raw += 1;
            g_asset_vfds[slot].last_used = ++g_asset_vfd_clock;
            promoted = 1;
        }
        asset_vfd_unlock();
        if (!promoted) close(tmp);
        errno = e;
        if (promoted && g_asset_vfd_live_raw > ASSET_VFD_RAW_CACHE_SOFT_MAX)
            asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }

    if (ret > 0 || raw_fd >= 0) {
        int saved_errno = errno;
        asset_vfd_lock();
        if (g_asset_vfds[slot].used) {
            if (ret > 0) {
                g_asset_vfds[slot].pos = pos + ret;
            }
            if (raw_fd >= 0 && g_asset_vfds[slot].busy > 0) {
                g_asset_vfds[slot].busy -= 1;
            }
        }
        asset_vfd_unlock();
        errno = saved_errno;
    }

    static unsigned int log_count = 0;
    if (log_count++ < 64 || ret < 0) {
        l_info("[ASSETVFD] read fd=%d off=%lld count=%u -> %d",
               fd, (long long)pos, (unsigned)count, (int)ret);
    }
    return ret;
}

ssize_t asset_vfd_pread(int fd, void *buf, size_t count, off_t offset) {
    char path[PATH_MAX];
    off_t length = 0;
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (asset_vfd_snapshot(fd, NULL, 0, NULL, &length) < 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (offset >= length) {
        return 0;
    }
    if ((off_t)count > length - offset) {
        count = (size_t)(length - offset);
    }

    int slot2 = asset_vfd_slot(fd);
    asset_vfd_lock();
    int raw_fd = -1;
    if (slot2 >= 0 && g_asset_vfds[slot2].used && g_asset_vfds[slot2].raw_fd >= 0) {
        raw_fd = g_asset_vfds[slot2].raw_fd;
        g_asset_vfds[slot2].busy += 1;
        g_asset_vfds[slot2].last_used = ++g_asset_vfd_clock;
    }
    asset_vfd_unlock();

    ssize_t ret;
    if (raw_fd >= 0) {
        ret = pread(raw_fd, buf, count, offset);   /* cached fd: no open/close/seek */
    } else {
        /* Cold path only (see asset_vfd_read): fetch the path here, not per-read. */
        asset_vfd_lock();
        int ok = slot2 >= 0 && g_asset_vfds[slot2].used;
        if (ok) sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot2].path);
        asset_vfd_unlock();
        if (!ok) { errno = EBADF; return -1; }
        int tmp = open(path, O_RDONLY);
        if (tmp < 0) {
            l_warn("[ASSETVFD] pread fd=%d open failed path=%s errno=%d", fd, path, errno);
            return -1;
        }
        ret = pread(tmp, buf, count, offset);
        int e = errno;
        /* PROMOTE (2026-07-17): cache this fd on the slot (see asset_vfd_read). */
        int promoted = 0;
        asset_vfd_lock();
        if (slot2 >= 0 && g_asset_vfds[slot2].used && g_asset_vfds[slot2].raw_fd < 0) {
            g_asset_vfds[slot2].raw_fd = tmp;
            g_asset_vfd_live_raw += 1;
            g_asset_vfds[slot2].last_used = ++g_asset_vfd_clock;
            promoted = 1;
        }
        asset_vfd_unlock();
        if (!promoted) close(tmp);
        errno = e;
        if (promoted && g_asset_vfd_live_raw > ASSET_VFD_RAW_CACHE_SOFT_MAX)
            asset_vfd_trim_cached_fds(ASSET_VFD_RAW_CACHE_SOFT_MAX);
    }

    if (raw_fd >= 0) {
        int saved_errno = errno;
        asset_vfd_lock();
        if (slot2 >= 0 && g_asset_vfds[slot2].used && g_asset_vfds[slot2].busy > 0) {
            g_asset_vfds[slot2].busy -= 1;
        }
        asset_vfd_unlock();
        errno = saved_errno;
    }

    static unsigned int log_count = 0;
    if (log_count++ < 64 || ret < 0) {
        l_info("[ASSETVFD] pread fd=%d off=%lld count=%u -> %d",
               fd, (long long)offset, (unsigned)count, (int)ret);
    }
    return ret;
}

off_t asset_vfd_lseek(int fd, off_t offset, int whence) {
    int slot = asset_vfd_slot(fd);
    if (slot < 0) {
        errno = EBADF;
        return -1;
    }

    asset_vfd_lock();
    if (!g_asset_vfds[slot].used) {
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

    char path[PATH_MAX];
    asset_vfd_lock();
    if (!g_asset_vfds[slot].used) {
        asset_vfd_unlock();
        errno = EBADF;
        return -1;
    }
    sceClibSnprintf(path, sizeof(path), "%s", g_asset_vfds[slot].path);
    int raw_fd = g_asset_vfds[slot].raw_fd;
    g_asset_vfds[slot].raw_fd = -1;
    if (raw_fd >= 0 && g_asset_vfd_live_raw > 0) {
        g_asset_vfd_live_raw -= 1;
    }
    g_asset_vfds[slot].used = 0;
    g_asset_vfds[slot].path[0] = '\0';
    g_asset_vfds[slot].pos = 0;
    g_asset_vfds[slot].length = 0;
    g_asset_vfds[slot].busy = 0;
    g_asset_vfds[slot].last_used = 0;
    asset_vfd_unlock();

    if (raw_fd >= 0) {
        close(raw_fd);   /* release the cached real fd */
    }

    l_info("[ASSETVFD] close fd=%d path=%s", fd, path);
    return 0;
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
    obb_track_fd(path, ret);
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
    if (is_opensl_library_path(path)) {
        static unsigned s_logged = 0;
        if (s_logged++ < 4U) {
            l_info("access(%s, %d): OpenSL bridge available", path ? path : "(null)", amode);
        }
        return 0;
    }
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
    if (obb_is_fd(fd)) {
        static int n = 0;
        // Always log large reads (entry data, not just header/dir parsing); throttle small ones.
        if (count > 64 || n++ < 100) {
            l_info("[OBBIO] read(fd=%d, count=%u) -> %d", fd, (unsigned)count, (int)r);
        }
    }
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
    if (obb_is_fd(fd)) {
        static int n = 0;
        // Always log deep seeks (into the 813MB body where entries live).
        if (offset > 8192 || n++ < 100) {
            l_info("[OBBIO] lseek(fd=%d, off=%lld, whence=%d) -> %lld",
                   fd, (long long)offset, whence, (long long)r);
        }
    }
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
    if (is_opensl_library_path(path)) {
        static unsigned s_logged = 0;
        if (s_logged++ < 4U) {
            l_info("stat(%s): OpenSL bridge available", path ? path : "(null)");
        }
        return stat_virtual_opensl_library(buf);
    }

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
    int should_flush = 0;
    int did_write = 0;
    int is_savedata = savedata_snapshot_fd(fd, savedata_path, sizeof(savedata_path), &should_flush, &did_write);
    if (is_savedata && should_flush) {
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
        savedata_untrack_fd(fd);
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
