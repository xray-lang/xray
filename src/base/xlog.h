/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlog.h - Structured diagnostic logging
 *
 * KEY CONCEPT:
 *   4-level logging system replacing scattered fprintf(stderr) calls.
 *   DEBUG level compiles to no-op in Release builds.
 *   Thread-safe output via atomic flag guard.
 *
 * LOG LEVELS:
 *   Level 0 (DEBUG):   Development diagnostics (stripped in Release)
 *   Level 1 (VERBOSE): Detailed runtime information
 *   Level 2 (NOTICE):  Important operational events
 *   Level 3 (WARNING): Problems that need attention
 *
 * USAGE:
 *   xr_log_debug("gc", "sweep phase: %d blocks freed", count);
 *   xr_log_notice("module", "loaded '%s' in %dms", name, ms);
 *   xr_log_warning("vm", "stack near limit: %d/%d", used, max);
 *
 * WHY THIS DESIGN:
 *   - Macro short-circuit: zero overhead when level is below threshold
 *   - Module tag: enables filtering by subsystem
 *   - No heap allocation: uses stack buffer + single write
 *   - Compatible with XR_CHECK: fatal errors still use abort()
 */

#ifndef XLOG_H
#define XLOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include "xdefs.h"
#include "xchecks.h"

/* ========== Log Levels ========== */

typedef enum {
    XR_LOG_DEBUG   = 0,   // Development only (stripped in Release)
    XR_LOG_VERBOSE = 1,   // Detailed runtime info
    XR_LOG_NOTICE  = 2,   // Important events
    XR_LOG_WARNING = 3,   // Needs attention
    XR_LOG_SILENT  = 4    // Suppress all output
} XrLogLevel;

/* ========== Runtime Configuration ========== */

// Global minimum log level (default: NOTICE in Release, DEBUG in Debug)
XR_DATA XrLogLevel xr_log_min_level;

// Set minimum log level
static inline void xr_log_set_level(XrLogLevel level) {
    xr_log_min_level = level;
}

// Get current minimum log level
static inline XrLogLevel xr_log_get_level(void) {
    return xr_log_min_level;
}

/* ========== Core Logging Function ========== */

// Low-level log function (use macros below instead)
XR_FUNC void xr_log_impl(XrLogLevel level, const char *module,
                          const char *file, int line,
                          const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* ========== Logging Macros ========== */

// Debug: stripped in Release builds
#if XR_DEBUG
    #define xr_log_debug(module, fmt, ...) \
        do { \
            if (xr_log_min_level <= XR_LOG_DEBUG) \
                xr_log_impl(XR_LOG_DEBUG, module, __FILE__, __LINE__, \
                            fmt, ##__VA_ARGS__); \
        } while(0)
#else
    #define xr_log_debug(module, fmt, ...) ((void)0)
#endif

// Verbose: always compiled, short-circuit check
#define xr_log_verbose(module, fmt, ...) \
    do { \
        if (xr_log_min_level <= XR_LOG_VERBOSE) \
            xr_log_impl(XR_LOG_VERBOSE, module, __FILE__, __LINE__, \
                        fmt, ##__VA_ARGS__); \
    } while(0)

// Notice: important operational events
#define xr_log_notice(module, fmt, ...) \
    do { \
        if (xr_log_min_level <= XR_LOG_NOTICE) \
            xr_log_impl(XR_LOG_NOTICE, module, __FILE__, __LINE__, \
                        fmt, ##__VA_ARGS__); \
    } while(0)

// Warning: problems that need attention
#define xr_log_warning(module, fmt, ...) \
    do { \
        if (xr_log_min_level <= XR_LOG_WARNING) \
            xr_log_impl(XR_LOG_WARNING, module, __FILE__, __LINE__, \
                        fmt, ##__VA_ARGS__); \
    } while(0)

/* ========== Convenience: Log + Return Pattern ========== */

// Log warning and return a value (for error paths)
#define XR_LOG_RETURN(module, retval, fmt, ...) \
    do { \
        xr_log_warning(module, fmt, ##__VA_ARGS__); \
        return (retval); \
    } while(0)

// Log warning and return void
#define XR_LOG_RETURN_VOID(module, fmt, ...) \
    do { \
        xr_log_warning(module, fmt, ##__VA_ARGS__); \
        return; \
    } while(0)

#endif // XLOG_H
