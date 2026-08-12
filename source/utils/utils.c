/*
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/utils.h"
#include "utils/logger.h"
#include "utils/config.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <sha1/sha1.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

uint64_t current_timestamp_ms() {
    struct timeval te;
    gettimeofday(&te, NULL);
    return (te.tv_sec * 1000LL + te.tv_usec / 1000);
}

static bool path_is_dir(const char *path) {
    if (!path || !*path) {
        return false;
    }

    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    return sceIoGetstat(path, &stat) >= 0 && SCE_S_ISDIR(stat.st_mode);
}

static int make_dir_if_missing(const char *path, mode_t mode, int *mkdir_errno) {
    if (path_is_dir(path)) {
        return 0;
    }

    errno = 0;
    if (mkdir(path, mode) == 0 || path_is_dir(path)) {
        return 0;
    }

    if (mkdir_errno) {
        *mkdir_errno = errno;
    }

    int sce_rc = sceIoMkdir(path, (SceMode)mode);
    if (sce_rc >= 0 || path_is_dir(path)) {
        return 0;
    }

    return sce_rc;
}

/* ★ 2026-08-11 — file_copy IS A SAVE-DATA PATH, SO IT HAD TO STOP BEING DESTRUCTIVE.
 *
 * This function is not a convenience any more. It mirrors prefs.prop after every
 * SavePrefs and it restores save files from backup at boot, so its failure modes are
 * the player's failure modes. It had three, all of which end in a destroyed file
 * rather than a failed copy:
 *
 *   1. file_save() truncated the destination and THEN wrote. Interrupt it -- and this
 *      port is interrupted constantly, because players force-quit from the LiveArea
 *      whenever a scene load looks like a hang -- and the destination is left
 *      truncated. The good copy is gone and the new one was never finished.
 *   2. Neither fwrite nor fclose was checked, so a short write on a full or flaky
 *      memory card returned `true`. A truncated .prop that reports success is exactly
 *      "my choices came back corrupted".
 *   3. file_load did not check fread either, so a partial read produced a full-size
 *      buffer with an uninitialised tail, which was then written out as if it were
 *      the file.
 *
 * Now: read verified, write verified, and committed by rename so the destination is
 * only ever the old file or the whole new one. */
bool file_copy(const char * path, const char * destination) {
    if (!file_exists(path)) {
        l_error("file_copy: Specified source path \"%s\" "
                "does not exist.", path);
        return false;
    }

    if (!file_mkpath(destination, 0755)) {
        l_error("file_copy: Could not create parent directories for the "
                "specified destination path \"%s\".", destination);
        return false;
    }

    uint8_t * buffer;
    size_t    size;

    if (!file_load(path, &buffer, &size)) {
        l_error("file_copy: Failed to read data from "
                "the source path \"%s\".", path);
        return false;
    }

    if (!file_save_atomic(destination, buffer, size)) {
        l_error("file_copy: Failed to write data to the specified "
                "destination path \"%s\".", destination);
        free(buffer);   /* the failure path leaked the whole source file */
        return false;
    }

    free(buffer);
    return true;
}

/* Commit-by-rename. The destination is never observed half-written: either the
 * previous contents are intact or the complete new contents are in place.
 *
 * The rename is attempted BEFORE any remove(), so the window in which the
 * destination does not exist at all is entered only on a filesystem that refuses to
 * rename over an existing name -- and even then the finished data is already sitting
 * in the sidecar, so the boot-time recovery pass can still find it. */
bool file_save_atomic(const char * path, const uint8_t * buffer, size_t size) {
    if (!path || (!buffer && size > 0)) {
        l_error("file_save_atomic: Invalid argument(s).");
        return false;
    }

    char tmp[512];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp)) {
        /* Path too long for a sidecar: a direct write is worse than no write for a
         * save file, but refusing outright would silently drop the data. Say so. */
        l_warn("file_save_atomic: \"%s\" is too long for a .new sidecar — writing "
               "in place, which is NOT crash-safe", path);
        return file_save(path, buffer, size);
    }

    remove(tmp);
    if (!file_save(tmp, buffer, size)) {
        l_error("file_save_atomic: could not stage \"%s\".", tmp);
        remove(tmp);
        return false;
    }

    /* Verify the staged copy on the card rather than trusting the write calls: this
     * is the last point at which a short write can still be caught for free. */
    const long long staged = file_size(tmp);
    if (staged != (long long)size) {
        l_error("file_save_atomic: staged \"%s\" is %lld bytes, expected %u — "
                "discarding rather than committing a truncated file",
                tmp, staged, (unsigned)size);
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) == 0) {
        return true;
    }

    /* Vita's rename refuses an existing destination. Take the old file out of the
     * way and retry; the complete data is already staged, so this is the only step
     * that can be interrupted and it leaves .new behind to recover from. */
    remove(path);
    if (rename(tmp, path) == 0) {
        return true;
    }

    l_warn("file_save_atomic: rename(%s -> %s) failed errno=%d — falling back to a "
           "direct write", tmp, path, errno);
    const bool ok = file_save(path, buffer, size);
    remove(tmp);
    return ok;
}

bool file_exists(const char * path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

long long file_size(const char * path) {
    SceIoStat stat;
    if (sceIoGetstat(path, &stat) < 0) {
        return -1;
    }
    return (long long)stat.st_size;
}

FILE * mcsm_open_setting(const char * basename, const char * mode) {
    char path[256];
    /* All user configuration has one canonical home. Do not fall back to the data
     * root: duplicate flat files create invisible precedence and make a builder-made
     * profile appear to ignore the user's edits. */
    snprintf(path, sizeof(path), DATA_PATH "settings/%s", basename);
    return fopen(path, mode);
}

void mcsm_read_clock_cfg(McsmClockCfg * cfg) {
    if (!cfg) return;
    /* Pinned 444 by default (arm_min == arm_max -> governor idle -> steadiest
     * fps). The battery profile sets clock=adaptive, dropping the floor to 266 so
     * the governor can downclock light frames to save power. */
    cfg->gpu = 222;   /* no config knob exists for the GPU clock; see utils.h */
    /* OVERCLOCK SUPPORT (2026-07-25). This used to hard-code 444 (stock max), so a
     * console running a CPU-overclock plugin gained nothing: the game simply never
     * asked for more. That mattered because the workload is ~70% SIM-bound, where
     * ARM MHz is close to a linear multiplier -- it is the single biggest remaining
     * performance lever, and it was capped by us rather than by the hardware.
     * `clock = <MHz>` in graphics.txt is now honoured (clamped to a sane 111..600).
     * With no OC plugin the kernel just clamps the request back to 444, so asking
     * for more is harmless -- the CLOCK: log line reports what was actually granted
     * ("got=") so it is obvious whether the plugin took effect. */
    const int want = mcsm_cfg()->clock_mhz;
    const int arm  = (want >= 111 && want <= 600) ? want : 444;
    if (mcsm_cfg()->clock_adaptive) { cfg->arm_min = 266; cfg->arm_max = arm; }
    else                            { cfg->arm_min = arm; cfg->arm_max = arm; }
}

bool file_load(const char * path, uint8_t ** buffer, size_t * size) {
    if (!buffer || !size) {
        l_error("file_load: Invalid argument(s).");
        return false;
    }

    if (!file_exists(path)) {
        l_error("file_load: Specified source path \"%s\" "
                "does not exist.", path);
        return false;
    }

#ifdef USE_SCELIBC_IO
    FILE * f = sceLibcBridge_fopen(path, "rb");
#else
    FILE * f = fopen(path, "rb");
#endif

    if (!f) {
        l_error("file_load: Could not open the specified "
                "source path \"%s\".", path);
        return false;
    }

#ifdef USE_SCELIBC_IO
    sceLibcBridge_fseek(f, 0, SEEK_END);
    *size = sceLibcBridge_ftell(f);
    sceLibcBridge_fseek(f, 0, SEEK_SET);
#else
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
#endif

    if (*size <= 0) {
        l_error("file_load: The specified source file \"%s\" is empty.", path);
    #ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(f);
    #else
        fclose(f);
    #endif
        return false;
    }

    *buffer = malloc(*size);

    if (!*buffer) {
        /* The size argument was MISSING here: two conversions, one argument, so
         * %d printed `path` as an integer and %s then dereferenced whatever was in
         * the next vararg slot. That faults in the one situation this line exists
         * to describe -- malloc failure -- so the loader's OOM report was itself
         * liable to crash and destroy the evidence. `*size` is size_t, hence %u
         * with an explicit cast rather than %d. */
        l_error("file_load: Unable to allocate %u bytes of memory to load "
                "the specified source file \"%s\".", (unsigned)*size, path);
    #ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(f);
    #else
        fclose(f);
    #endif
        return false;
    }

#ifdef USE_SCELIBC_IO
    const size_t got = sceLibcBridge_fread(*buffer, 1, *size, f);
    sceLibcBridge_fclose(f);
#else
    const size_t got = fread(*buffer, 1, *size, f);
    fclose(f);
#endif

    /* A short read used to pass silently: the buffer stayed *size bytes long with an
     * uninitialised tail, and file_copy would then write that tail out as if it were
     * file content. Refuse the load instead -- a caller that gets `false` keeps the
     * destination it already had. */
    if (got != *size) {
        l_error("file_load: short read on \"%s\" (%u of %u bytes) — refusing to "
                "hand back a partially filled buffer", path, (unsigned)got, (unsigned)*size);
        free(*buffer);
        *buffer = NULL;
        *size = 0;
        return false;
    }

    return true;
}

bool file_mkpath(const char * path, mode_t mode) {
    if (!path || !*path) {
        l_error("file_mkpath: Invalid argument.");
        return false;
    }

    char * file_path = strdup(path);
    if (!file_path) {
        l_error("file_mkpath: Unable to duplicate path \"%s\".", path);
        return false;
    }

    for (char* p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        int mkdir_errno = 0;
        int sce_rc = make_dir_if_missing(file_path, mode, &mkdir_errno);
        if (sce_rc < 0) {
            l_error("file_mkpath: Unable to create directory \"%s\" "
                    "(mkdir errno=%d/%s, sceIoMkdir=0x%08X).",
                    file_path, mkdir_errno, strerror(mkdir_errno), (unsigned)sce_rc);
            free(file_path);
            return false;
        }
        *p = '/';
    }

    free(file_path);
    return true;
}

static bool file_save_sceio(const char *path, const uint8_t *buffer, size_t size) {
    if (!path || (!buffer && size > 0)) {
        l_error("file_save_sceio: Invalid argument(s).");
        return false;
    }

    char alt_path[512];
    const char *paths[2] = { path, NULL };
    if ((strncmp(path, "ux0:/", 5) == 0 || strncmp(path, "ur0:/", 5) == 0) && strlen(path) < sizeof(alt_path)) {
        snprintf(alt_path, sizeof(alt_path), "%.4s%s", path, path + 5);
        paths[1] = alt_path;
    } else if ((strncmp(path, "ux0:", 4) == 0 || strncmp(path, "ur0:", 4) == 0) &&
               path[4] != '/' &&
               strlen(path) + 1 < sizeof(alt_path)) {
        snprintf(alt_path, sizeof(alt_path), "%.4s/%s", path, path + 4);
        paths[1] = alt_path;
    }

    for (int path_index = 0; path_index < 2 && paths[path_index]; ++path_index) {
        const char *open_path = paths[path_index];
        if (!file_mkpath(open_path, 0755)) {
            l_error("file_save_sceio: Could not create parent directories for \"%s\".", open_path);
            continue;
        }

        SceUID fd = sceIoOpen(open_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0644);
        if (fd < 0) {
            l_error("file_save_sceio: Could not open \"%s\" (0x%08X).", open_path, (unsigned)fd);
            continue;
        }

        size_t written = 0;
        while (written < size) {
            int chunk = sceIoWrite(fd, buffer + written, size - written);
            if (chunk < 0) {
                l_error("file_save_sceio: Write failed for \"%s\" at %u/%u (0x%08X).",
                        open_path,
                        (unsigned)written,
                        (unsigned)size,
                        (unsigned)chunk);
                sceIoClose(fd);
                return false;
            }
            if (chunk == 0) {
                l_error("file_save_sceio: Short write for \"%s\" at %u/%u.",
                        open_path,
                        (unsigned)written,
                        (unsigned)size);
                sceIoClose(fd);
                return false;
            }
            written += (size_t)chunk;
        }

        sceIoClose(fd);
        if (open_path != path) {
            l_info("file_save_sceio: wrote \"%s\" via alternate path for \"%s\".", open_path, path);
        }
        return true;
    }

    return false;
}

bool file_save(const char * path, const uint8_t * buffer, size_t size) {
    if (!file_mkpath(path, 0755)) {
        l_error("file_save: Could not create parent directories for \"%s\".", path);
        return false;
    }

#ifdef USE_SCELIBC_IO
    FILE * f = sceLibcBridge_fopen(path, "wb");
#else
    FILE * f = fopen(path, "wb");
#endif

    if (!f) {
        if (file_save_sceio(path, buffer, size)) {
            l_info("file_save: wrote \"%s\" via sceIo fallback.", path);
            return true;
        }
        l_error("file_save: Could not open the specified target path \"%s\".", path);
        return false;
    }

    /* fwrite(buffer, size, 1, f) counts ITEMS, so a partial write of a single big
     * item returns 0 and the old code discarded that. Write byte-wise so a short
     * write is a number we can compare, and check fclose too -- the buffered tail is
     * flushed there, so a card that fills up fails at fclose and nowhere else. */
    const size_t want = size;
#ifdef USE_SCELIBC_IO
    const size_t put = want ? sceLibcBridge_fwrite(buffer, 1, want, f) : 0;
    const int close_rc = sceLibcBridge_fclose(f);
#else
    const size_t put = want ? fwrite(buffer, 1, want, f) : 0;
    /* Durability barrier before the handle goes away: this port is force-quit
     * routinely, and an unflushed memory-card write does not survive that. */
    fflush(f);
    fsync(fileno(f));
    const int close_rc = fclose(f);
#endif

    if (put != want || close_rc != 0) {
        l_error("file_save: short write on \"%s\" (%u of %u bytes, fclose=%d) — the "
                "file on the card is incomplete",
                path, (unsigned)put, (unsigned)want, close_rc);
        return false;
    }

    return true;
}

bool is_dir(const char * path) {
    if (path_is_dir(path)) {
        return true;
    }

    DIR* d = opendir(path);

    if (d != NULL) {
        closedir(d);
        return true;
    }

    return false;
}

SceUID _vshKernelSearchModuleByName(const char *, int *);

bool module_loaded(const char * name) {
    int search_unk[2];
    return _vshKernelSearchModuleByName(name, search_unk) >= 0;
}

int ret0(void) {
    return 0;
}

bool str_starts_with(const char * str, const char * prefix) {
    char cp;
    char cs;

    if (!*prefix)
        return true;

    while ((cp = *prefix++) && (cs = *str++)) {
        if (cp != cs)
            return false;
    }

    if (!cs)
        return false;

    return true;
}

char * str_sha1sum(const char * str, size_t size) {
    if (size == 0) {
        size = strlen(str);
    }

    uint8_t sha1[20];
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (uint8_t *)str, size);
    sha1_final(&ctx, (uint8_t *)sha1);

    char hash[42];
    memset(hash, 0, sizeof(hash));

    for (int i = 0; i < 20; i++) {
        char string[4];
        sprintf(string, "%02X", sha1[i]);
        strcat(hash, string);
    }

    hash[41] = '\0';
    return strdup(hash);
}
