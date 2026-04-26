/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_debug.h - Compile-time debug logging macros
 *
 * KEY CONCEPT:
 *   Zero-overhead debug logging with per-module and per-level control.
 *   All debug code is completely removed in Release builds.
 *
 * USAGE:
 *   - Default: all debug off (zero overhead)
 *   - cmake -DXR_DEBUG_CORO=1 ..  # Enable coroutine debug
 *   - cmake -DXR_DEBUG_ALL=1 ..   # Enable all debug
 *   - Levels: 1=critical, 2=state changes, 3=verbose
 */

#ifndef XRAY_DEBUG_H
#define XRAY_DEBUG_H

#include <stdio.h>

/*
 * Debug modules (6 core modules):
 *   CORO   - Coroutine lifecycle
 *   GC     - Memory allocation/collection
 *   TIMER  - Timer wheel, sleep
 *   ASYNC  - Async I/O, netpoll
 *   MODULE - Import/export
 *   FRAME  - Stack frame management
 *
 * Levels: 0=off, 1=critical, 2=state changes, 3=verbose
 */

// Global debug level
#ifndef XR_DEBUG_ALL
#define XR_DEBUG_ALL 0
#endif

#ifndef XR_DEBUG_CORO
#define XR_DEBUG_CORO XR_DEBUG_ALL
#endif

#ifndef XR_DEBUG_GC
#define XR_DEBUG_GC XR_DEBUG_ALL
#endif

#ifndef XR_DEBUG_TIMER
#define XR_DEBUG_TIMER XR_DEBUG_ALL
#endif

#ifndef XR_DEBUG_ASYNC
#define XR_DEBUG_ASYNC XR_DEBUG_ALL
#endif

#ifndef XR_DEBUG_MODULE
#define XR_DEBUG_MODULE XR_DEBUG_ALL
#endif

#ifndef XR_DEBUG_FRAME
#define XR_DEBUG_FRAME XR_DEBUG_ALL
#endif

/*
 * Core debug macro: XR_DLOG(module, level, tag, fmt, ...)
 * Outputs only when XR_DEBUG_##module >= level
 */

#define XR_DLOG(module, level, tag, fmt, ...)                                                      \
    do {                                                                                           \
        if (XR_DEBUG_##module >= (level)) {                                                        \
            fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__);                                 \
        }                                                                                          \
    } while (0)

#define XR_DLOG_IF(module, level, cond, tag, fmt, ...)                                             \
    do {                                                                                           \
        if ((XR_DEBUG_##module >= (level)) && (cond)) {                                            \
            fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__);                                 \
        }                                                                                          \
    } while (0)

/*
 * Per-module macros: XR_DBG_XXX (level 1), XR_DBG_XXX2 (level 2), XR_DBG_XXX3 (level 3)
 */

// Coroutine
#define XR_DBG_CORO(fmt, ...) XR_DLOG(CORO, 1, "CORO", fmt, ##__VA_ARGS__)
#define XR_DBG_CORO2(fmt, ...) XR_DLOG(CORO, 2, "CORO", fmt, ##__VA_ARGS__)
#define XR_DBG_CORO3(fmt, ...) XR_DLOG(CORO, 3, "CORO", fmt, ##__VA_ARGS__)

// GC
#define XR_DBG_GC(fmt, ...) XR_DLOG(GC, 1, "GC", fmt, ##__VA_ARGS__)
#define XR_DBG_GC2(fmt, ...) XR_DLOG(GC, 2, "GC", fmt, ##__VA_ARGS__)
#define XR_DBG_GC3(fmt, ...) XR_DLOG(GC, 3, "GC", fmt, ##__VA_ARGS__)

// Timer
#define XR_DBG_TIMER(fmt, ...) XR_DLOG(TIMER, 1, "TIMER", fmt, ##__VA_ARGS__)
#define XR_DBG_TIMER2(fmt, ...) XR_DLOG(TIMER, 2, "TIMER", fmt, ##__VA_ARGS__)
#define XR_DBG_TIMER3(fmt, ...) XR_DLOG(TIMER, 3, "TIMER", fmt, ##__VA_ARGS__)

// Async I/O
#define XR_DBG_ASYNC(fmt, ...) XR_DLOG(ASYNC, 1, "ASYNC", fmt, ##__VA_ARGS__)
#define XR_DBG_ASYNC2(fmt, ...) XR_DLOG(ASYNC, 2, "ASYNC", fmt, ##__VA_ARGS__)
#define XR_DBG_ASYNC3(fmt, ...) XR_DLOG(ASYNC, 3, "ASYNC", fmt, ##__VA_ARGS__)

// Module
#define XR_DBG_MODULE(fmt, ...) XR_DLOG(MODULE, 1, "MODULE", fmt, ##__VA_ARGS__)
#define XR_DBG_MODULE2(fmt, ...) XR_DLOG(MODULE, 2, "MODULE", fmt, ##__VA_ARGS__)
#define XR_DBG_MODULE3(fmt, ...) XR_DLOG(MODULE, 3, "MODULE", fmt, ##__VA_ARGS__)

// Frame
#define XR_DBG_FRAME(fmt, ...) XR_DLOG(FRAME, 1, "FRAME", fmt, ##__VA_ARGS__)
#define XR_DBG_FRAME2(fmt, ...) XR_DLOG(FRAME, 2, "FRAME", fmt, ##__VA_ARGS__)
#define XR_DBG_FRAME3(fmt, ...) XR_DLOG(FRAME, 3, "FRAME", fmt, ##__VA_ARGS__)

// Level check macros

#define XR_DBG_CORO_ON(level) (XR_DEBUG_CORO >= (level))
#define XR_DBG_GC_ON(level) (XR_DEBUG_GC >= (level))
#define XR_DBG_TIMER_ON(level) (XR_DEBUG_TIMER >= (level))
#define XR_DBG_ASYNC_ON(level) (XR_DEBUG_ASYNC >= (level))
#define XR_DBG_MODULE_ON(level) (XR_DEBUG_MODULE >= (level))
#define XR_DBG_FRAME_ON(level) (XR_DEBUG_FRAME >= (level))

// Compile-time check

#define XR_DEBUG_ENABLED                                                                           \
    (XR_DEBUG_CORO || XR_DEBUG_GC || XR_DEBUG_TIMER || XR_DEBUG_ASYNC || XR_DEBUG_MODULE ||        \
     XR_DEBUG_FRAME)

#endif  // XRAY_DEBUG_H
