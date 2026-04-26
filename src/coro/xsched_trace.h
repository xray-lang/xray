/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsched_trace.h - Optional scheduler tracing infrastructure
 *
 * KEY CONCEPT:
 *   Compile-time enabled scheduler tracing for debugging concurrency
 *   issues. Zero overhead when disabled (default).
 *
 *   Enable: cmake -DXR_TRACE_SCHED=ON
 *   Or: add_compile_definitions(XR_TRACE_SCHED) in CMakeLists.txt
 */

#ifndef XSCHED_TRACE_H
#define XSCHED_TRACE_H

#include <stdio.h>
#include <stdint.h>

#ifdef XR_TRACE_SCHED

#define SCHED_TRACE(worker, fmt, ...)                                                              \
    fprintf(stderr, "[SCHED W%d] " fmt "\n", (worker)->p.id, ##__VA_ARGS__)

#define SCHED_TRACE_CORO(worker, coro, event)                                                      \
    fprintf(stderr, "[SCHED W%d] %s: coro=%p name=%s flags=0x%x\n", (worker)->p.id, (event),       \
            (void *) (coro), ((coro) && (coro)->name) ? (coro)->name : "?",                        \
            (coro) ? xr_coro_flags_load(coro) : 0)

#define SCHED_TRACE_IO(worker, fd, coro, event)                                                    \
    fprintf(stderr, "[SCHED W%d] %s: fd=%d coro=%p\n", (worker)->p.id, (event), (fd),              \
            (void *) (coro))

#define SCHED_TRACE_PARK(worker, reason)                                                           \
    fprintf(stderr, "[SCHED W%d] park: %s\n", (worker)->p.id, (reason))

#define SCHED_TRACE_STEAL(worker, victim_id, count)                                                \
    fprintf(stderr, "[SCHED W%d] steal: from=W%d count=%d\n", (worker)->p.id, (victim_id), (count))

#else

#define SCHED_TRACE(worker, fmt, ...) ((void) 0)
#define SCHED_TRACE_CORO(worker, coro, event) ((void) 0)
#define SCHED_TRACE_IO(worker, fd, coro, event) ((void) 0)
#define SCHED_TRACE_PARK(worker, reason) ((void) 0)
#define SCHED_TRACE_STEAL(worker, victim, cnt) ((void) 0)

#endif  // XR_TRACE_SCHED

#endif  // XSCHED_TRACE_H
