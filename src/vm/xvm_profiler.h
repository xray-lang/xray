/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_profiler.h - VM performance profiler
 *
 * KEY CONCEPT:
 *   Instruction execution counter and timing statistics. The profiler is
 *   isolate-local: each XrayIsolate owns a VMProfiler in its `profiler`
 *   slot, allocated on isolate construction when the build was configured
 *   with -DXR_ENABLE_VM_PROFILER=ON. Concurrent isolates therefore never
 *   share counters, which is critical for embedding scenarios (LSP, DAP,
 *   parallel script hosts).
 *
 *   Set XR_ENABLE_VM_PROFILER=1 to enable; the dispatch loop resolves the
 *   active profiler once per top-level entry and threads it through the
 *   VM_PROFILE_* macros via a hidden local. Call vm_profiler_report() at
 *   isolate teardown.
 */

#ifndef XVM_PROFILER_H
#define XVM_PROFILER_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/value/xchunk.h"

/* ========== Configuration ========== */

/*
 * VM Profiler is controlled via CMake options:
 *   cmake -DXR_ENABLE_VM_PROFILER=ON   Enable instruction counting
 *   cmake -DXR_PROFILE_TIMING=ON       Enable per-instruction timing (~4x overhead)
 *
 * These can also be enabled by defining the macros before including this header.
 */

#ifndef XR_ENABLE_VM_PROFILER
#define XR_ENABLE_VM_PROFILER 0
#endif

#ifndef XR_PROFILE_TIMING
#define XR_PROFILE_TIMING 0
#endif

/* Forward declaration kept visible in every build so dispatch-loop
 * locals like `VMProfiler *_vm_prof = ...` compile even when the
 * profiler is disabled (and the macros below expand to no-ops that
 * just `(void)` the unused pointer). */
typedef struct VMProfiler VMProfiler;

/* ========== Profiler Implementation ========== */

#if XR_ENABLE_VM_PROFILER

#if XR_PROFILE_TIMING
#include "../base/xdefs.h"
#include "../base/xtime.h"
#endif

typedef struct VMOpStats {
    uint64_t count;
#if XR_PROFILE_TIMING
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
#endif
} VMOpStats;

typedef struct VMProfiler {
    VMOpStats op_stats[256];
    uint64_t total_instructions;
    uint64_t start_time_ms;
} VMProfiler;

static inline uint64_t vm_profiler_get_ms(void) {
#if XR_PROFILE_TIMING
    return xr_time_monotonic_ms();
#else
    return 0;
#endif
}

#if XR_PROFILE_TIMING
static inline uint64_t vm_profiler_get_ns(void) {
    return xr_time_monotonic_ns();
}
#endif

/* All accessors below tolerate p == NULL and become no-ops. NULL is the
 * normal state when an isolate was created without a profiler slot or
 * when the dispatch entry resolved the profiler from a half-initialised
 * ctx. The hot path checks the pointer once and the branch predictor
 * pins it firmly to one direction per process. */
static inline void vm_profiler_init(VMProfiler *p) {
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->start_time_ms = vm_profiler_get_ms();
#if XR_PROFILE_TIMING
    for (int i = 0; i < 256; i++) {
        p->op_stats[i].min_ns = UINT64_MAX;
    }
#endif
}

static inline void vm_profiler_count(VMProfiler *p, int op) {
    if (!p)
        return;
    p->op_stats[op].count++;
    p->total_instructions++;
}

#if XR_PROFILE_TIMING
static inline void vm_profiler_record(VMProfiler *p, int op, uint64_t elapsed_ns) {
    if (!p)
        return;
    p->op_stats[op].count++;
    p->op_stats[op].total_ns += elapsed_ns;
    if (elapsed_ns < p->op_stats[op].min_ns) {
        p->op_stats[op].min_ns = elapsed_ns;
    }
    if (elapsed_ns > p->op_stats[op].max_ns) {
        p->op_stats[op].max_ns = elapsed_ns;
    }
    p->total_instructions++;
}
#endif

XR_FUNC void vm_profiler_report(const VMProfiler *p);

/* ========== Profiler Macros ==========
 *
 * The dispatch loop in xvm.c declares one local VMProfiler pointer
 * (`_vm_prof`) at entry and the macros below thread it implicitly into
 * every per-instruction call. That keeps the existing macro call-sites
 * (VM_PROFILE_COUNT(op) inside vmfetch / switch dispatch) untouched
 * while still letting concurrent isolates each have their own counters. */

#if XR_PROFILE_TIMING

#define VM_PROFILE_VARS()                                                                          \
    uint64_t _prof_start_ns = 0;                                                                   \
    int _prof_last_op = -1

#define VM_PROFILE_COUNT(op)                                                                       \
    do {                                                                                           \
        uint64_t _now = vm_profiler_get_ns();                                                      \
        if (_prof_last_op >= 0) {                                                                  \
            vm_profiler_record(_vm_prof, _prof_last_op, _now - _prof_start_ns);                    \
        }                                                                                          \
        _prof_last_op = (op);                                                                      \
        _prof_start_ns = _now;                                                                     \
    } while (0)

#define VM_PROFILE_FINISH()                                                                        \
    do {                                                                                           \
        if (_prof_last_op >= 0) {                                                                  \
            vm_profiler_record(_vm_prof, _prof_last_op, vm_profiler_get_ns() - _prof_start_ns);    \
        }                                                                                          \
    } while (0)

#else

#define VM_PROFILE_VARS() ((void) 0)
#define VM_PROFILE_COUNT(op) vm_profiler_count(_vm_prof, op)
#define VM_PROFILE_FINISH() ((void) 0)

#endif

#else

/* Disabled: zero overhead. The macros silently swallow any local
 * `_vm_prof` declaration so the dispatch loop compiles identically
 * to a profiler-free build. */
#define vm_profiler_init(p) ((void) (p))
#define vm_profiler_report(p) ((void) (p))
#define VM_PROFILE_VARS() ((void) 0)
#define VM_PROFILE_COUNT(op) ((void) 0)
#define VM_PROFILE_FINISH() ((void) 0)

#endif

#endif  // XVM_PROFILER_H
