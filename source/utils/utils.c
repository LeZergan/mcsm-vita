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

    if (!file_save(destination, buffer, size)) {
        l_error("file_copy: Failed to write data to the specified "
                "destination path \"%s\".", destination);
        return false;
    }

    free(buffer);
    return true;
}

bool file_exists(const char * path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

FILE * mcsm_open_setting(const char * basename, const char * mode) {
    char path[256];
    /* Prefer the tidy settings/ subfolder; fall back to the data root so an
     * existing flat layout keeps working unchanged. */
    snprintf(path, sizeof(path), DATA_PATH "settings/%s", basename);
    FILE * f = fopen(path, mode);
    if (f) return f;
    snprintf(path, sizeof(path), DATA_PATH "%s", basename);
    return fopen(path, mode);
}

void mcsm_read_clock_cfg(McsmClockCfg * cfg) {
    if (!cfg) return;
    /* Pinned 444 by default (arm_min == arm_max -> governor idle -> steadiest
     * fps). The battery profile sets clock=adaptive, dropping the floor to 266 so
     * the governor can downclock light frames to save power. */
    cfg->governor_off = 0;
    cfg->gpu = 222;
    if (mcsm_cfg()->clock_adaptive) { cfg->arm_min = 266; cfg->arm_max = 444; }
    else                            { cfg->arm_min = 444; cfg->arm_max = 444; }
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
        l_error("file_load: Unable to allocate %d bytes of memory to load "
                "the specified source file \"%s\".", path);
    #ifdef USE_SCELIBC_IO
        sceLibcBridge_fclose(f);
    #else
        fclose(f);
    #endif
        return false;
    }

#ifdef USE_SCELIBC_IO
    sceLibcBridge_fread(*buffer, 1, *size, f);
    sceLibcBridge_fclose(f);
#else
    fread(*buffer, 1, *size, f);
    fclose(f);
#endif

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

#ifdef USE_SCELIBC_IO
    sceLibcBridge_fwrite(buffer, size, 1, f);
    sceLibcBridge_fclose(f);
#else
    fwrite(buffer, size, 1, f);
    fclose(f);
#endif

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
