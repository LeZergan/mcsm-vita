/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  logger.h
 * @brief Logging utilities.
 */

#ifndef SOLOADER_LOGGER_H
#define SOLOADER_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#define LT_DEBUG   0
#define LT_INFO    1
#define LT_WARN    2
#define LT_ERROR   3
#define LT_FATAL   4
#define LT_SUCCESS 5
#define LT_WAIT    6

/* ★ A COMPILED-OUT LOG LINE STILL HAS TO BE A STATEMENT THAT USES ITS ARGUMENTS.
 *
 * These macros used to expand to NOTHING when logging was compiled out, which broke
 * the build in two ways that only showed up in the configuration nobody watches:
 *
 *   `if (rc < 0) l_warn("...");`  became  `if (rc < 0) ;`      -> -Wempty-body
 *   every variable read only by a log line became unused       -> -Wunused-variable
 *
 * so ENABLE_TELEMETRY_LOGGING=OFF -- the PRODUCTION build -- emitted 25+ warnings
 * while the logging build was clean, and "0 warnings" was quietly only ever true for
 * half of what ships.
 *
 * Worse than the noise: with the arguments gone, so was the printf format checking.
 * A wrong conversion or a missing argument (the kind that faults inside vsnprintf --
 * this project has already shipped one, in an out-of-memory report) was diagnosed
 * ONLY in the logging build.
 *
 * `if (0) _log_print(...)` fixes both: it is a real statement, the compiler type- and
 * format-checks the arguments, and it is unreachable so nothing is emitted. Argument
 * side effects are not evaluated -- exactly as before, when the whole call vanished. */
#define MCSM_LOG_DISCARD(...) do { if (0) _log_print(LT_DEBUG, __VA_ARGS__); } while (0)

#ifdef DEBUG_SOLOADER
/* PRODUCTION (2026-07-02): DEBUG-level lines are per-syscall spam (every
 * stat/open/fstat/readdir/AAsset op formats a line -- thousands during
 * scene loads, on the sim/render threads). Pure load-hitch overhead now
 * that the save/CH2 systems work. Compiled out unless MCSM_VERBOSE_DEBUG
 * is defined; INFO and up stay for diagnosability. */
#ifdef MCSM_VERBOSE_DEBUG
#define l_debug(...)   _log_print(LT_DEBUG,   __VA_ARGS__)
#else
#define l_debug(...)   MCSM_LOG_DISCARD(__VA_ARGS__)
#endif
#define l_info(...)    _log_print(LT_INFO,    __VA_ARGS__)
#define l_warn(...)    _log_print(LT_WARN,    __VA_ARGS__)
#define l_success(...) _log_print(LT_SUCCESS, __VA_ARGS__)
#define l_wait(...)    _log_print(LT_WAIT,    __VA_ARGS__)
#else
#define l_debug(...)   MCSM_LOG_DISCARD(__VA_ARGS__)
#define l_info(...)    MCSM_LOG_DISCARD(__VA_ARGS__)
#define l_warn(...)    MCSM_LOG_DISCARD(__VA_ARGS__)
#define l_success(...) MCSM_LOG_DISCARD(__VA_ARGS__)
#define l_wait(...)    MCSM_LOG_DISCARD(__VA_ARGS__)
#endif

#define l_error(...)   _log_print(LT_ERROR,   __VA_ARGS__)
#define l_fatal(...)   _log_print(LT_FATAL,   __VA_ARGS__)

void _log_print(int t, const char* fmt, ...)
                __attribute__ ((format (printf, 2, 3)));

void log_reset_file(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_LOGGER_H
