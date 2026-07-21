/*
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  utils.h
 * @brief Common helper utilities.
 */

#ifndef SOLOADER_UTILS_H
#define SOLOADER_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Get Unix timestamp in milliseconds.
 *
 * @return Number of milliseconds that have elapsed since January 1, 1970.
 */
uint64_t current_timestamp_ms();

/**
 * Create a copy of a file.
 *
 * If the file specified by `destination` already exists, it will be
 * overwritten. If a parent directory or directories of the file specified by
 * `destination` do not exist, they will be created automatically.
 *
 * @warning The function will fail if the size of the source file specified by
 *          `path` exceeds the amount of free memory available.
 *
 * @param[in] path        Full path of the source file.
 * @param[in] destination Full path of the destination file.
 *
 * @return `true` on success, `false` otherwise.
 */
bool file_copy(const char * path, const char * destination);

/**
 * Check whether a file exists.
 *
 * @param path Full path of the file to look for.
 *
 * @return `true` if file exists, `false` otherwise.
 */
bool file_exists(const char * path);

/**
 * Open a user tunable, preferring the tidy ux0:data/mcsm/settings/ subfolder
 * and falling back to the data root for backward compatibility.
 *
 * @param basename Bare file name, e.g. "fps_cap.txt".
 * @param mode     fopen mode (these tunables are read-only in practice).
 *
 * @return open FILE* (settings/ copy wins, else root copy), or NULL if neither.
 */
FILE * mcsm_open_setting(const char * basename, const char * mode);

/*
 * Consolidated clock + governor configuration, all from ONE tunable:
 * ux0:data/mcsm/settings/clock.txt (or the data root). Lines are order-free and
 * forgiving; recognised tokens:
 *   off            -> disable the adaptive governor (pin ARM to `arm_max`)
 *   min <MHz>      -> governor floor      (default 444 = pinned; lower it, e.g.
 *                     "min 266", to enable adaptive battery scaling)
 *   max <MHz>      -> governor ceiling + boot ARM clock (default 444, 222..500)
 *   gpu <MHz>      -> GPU clock           (default 222, clamped 111..266)
 * An absent file (or absent line) yields the defaults below. This ONE doc is
 * read by BOTH the boot clock (init.c) and the in-game governor (patch.c), so
 * there is a single place to configure clocks with a built-in opt-out.
 */
typedef struct {
    int governor_off;   /* 1 = governor disabled (ARM pinned to arm_max) */
    int arm_min;        /* governor floor, MHz */
    int arm_max;        /* governor ceiling AND boot ARM clock, MHz */
    int gpu;            /* GPU clock, MHz */
} McsmClockCfg;

void mcsm_read_clock_cfg(McsmClockCfg * cfg);

/**
 * Load file contents into memory.
 *
 * @param[in]  path   Full path of the source file.
 * @param[out] buffer Output buffer address, allocated by the function. Must be
 *                    freed by the caller if the function returns `true`.
 * @param[out] size   Output buffer size.
 *
 * @return `true` on success, `false` otherwise.
 */
bool file_load(const char * path, uint8_t ** buffer, size_t * size);

/**
 * Create directories leading to file.
 *
 * @param[in] path Full path of the target file.
 * @param[in] mode Permissions to set for new directories (if any).
 *
 * @return `true` on success, `false` otherwise.
 */
bool file_mkpath(const char * path, mode_t mode);

/**
 * Save buffer contents into a file.
 *
 * @param[in] path   Full path of the target file.
 * @param[in] buffer Buffer containing data to save.
 * @param[in] size   Size of the buffer (in bytes).
 *
 * @return `true` on success, `false` otherwise.
 */
bool file_save(const char * path, const uint8_t * buffer, size_t size);

/**
 * Check whether specified path is a directory.
 *
 * @param[in] path Target path.
 *
 * @return `true` if path is a directory, `false` otherwise.
 */
bool is_dir(const char * path);

/**
 * Check whether system module is loaded.
 *
 * @param[in] name Name of the system module to look for.
 *
 * @return `true` if the module is loaded, `false` otherwise.
 */
bool module_loaded(const char * name);

/**
 * Do nothing, return 0. Useful for stubbing.
 * @return 0
 */
int ret0(void);

/**
 * Check whether a string starts with a substring.
 *
 * @param[in] str    Target string.
 * @param[in] prefix Substring to look for.
 *
 * @return `true` if the string starts with the substring, `false` otherwise.
 */
bool str_starts_with(const char * str, const char * prefix);

/**
 * Get SHA1 hash of a string or byte array.
 *
 * @param[in] str  Source string or byte array.
 * @param[in] size Length of the source string or byte array. If `0` is
 *                 specified, `str` is treated as a null-terminated string.
 *
 * @return 40-char long null-terminated string containing SHA1 hash. Can be
 *         NULL in case of an error. Must be freed by the caller.
 */
char * str_sha1sum(const char * str, size_t size);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_UTILS_H
