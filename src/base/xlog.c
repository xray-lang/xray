/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlog.c - Structured diagnostic logging implementation
 */

#include "xdefs.h"
#include "xplatform.h"
#include "xlog.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdatomic.h>

#ifdef XR_OS_POSIX
#include <unistd.h>
#elif defined(XR_OS_WINDOWS)
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#endif

/* ========== Global State ========== */

/* Default to NOTICE in all builds.  Debug/verbose output is opt-in
 * via xr_log_set_level() or the --verbose CLI flag. */
XrLogLevel xr_log_min_level = XR_LOG_NOTICE;

// ========== Level Names ==========

static const char *level_names[] = {"DEBUG", "VERBOSE", "NOTICE", "WARNING"};

static const char *level_colors[] = {
    "\033[36m",    // DEBUG: cyan
    "\033[37m",    // VERBOSE: white
    "\033[32m",    // NOTICE: green
    "\033[1;33m",  // WARNING: bold yellow
};

/* ========== Write Guard (prevent interleaved output) ========== */

static _Atomic int log_lock = 0;

static inline void log_acquire(void) {
    int expected = 0;
    while (!atomic_compare_exchange_weak(&log_lock, &expected, 1)) {
        expected = 0;
        XR_CPU_PAUSE();
    }
}

static inline void log_release(void) {
    atomic_store(&log_lock, 0);
}

/* ========== Core Implementation ========== */

void xr_log_impl(XrLogLevel level, const char *module, const char *file, int line, const char *fmt,
                 ...) {
    if (level < xr_log_min_level)
        return;
    if (level > XR_LOG_WARNING)
        return;

    // Extract filename from path
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    // Format message into stack buffer
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    (void) len;

    // Detect color support once (atomic for thread safety)
    static _Atomic int use_color = -1;
    int color = atomic_load_explicit(&use_color, memory_order_relaxed);
    if (color == -1) {
        color = isatty(fileno(stderr));
        atomic_store_explicit(&use_color, color, memory_order_relaxed);
    }

    // Single atomic write to stderr
    log_acquire();

    if (color) {
        fprintf(stderr, "%s[%s]%s [%s] %s (%s:%d)\n", level_colors[level], level_names[level],
                "\033[0m", module, buf, basename, line);
    } else {
        fprintf(stderr, "[%s] [%s] %s (%s:%d)\n", level_names[level], module, buf, basename, line);
    }

    log_release();
}
