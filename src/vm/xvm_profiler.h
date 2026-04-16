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
 *   Instruction execution counter and timing statistics.
 *   Set XR_ENABLE_VM_PROFILER=1 to enable, call vm_profiler_report() at exit.
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
#endif // ========== Profiler Implementation ==========

#if XR_ENABLE_VM_PROFILER

#if XR_PROFILE_TIMING
#include <time.h>
#include "../base/xdefs.h"
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

extern VMProfiler g_vm_profiler;

static inline uint64_t vm_profiler_get_ms(void) {
#if XR_PROFILE_TIMING
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#else
    return 0;
#endif
}

#if XR_PROFILE_TIMING
static inline uint64_t vm_profiler_get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

static inline void vm_profiler_init(void) {
    memset(&g_vm_profiler, 0, sizeof(VMProfiler));
    g_vm_profiler.start_time_ms = vm_profiler_get_ms();
#if XR_PROFILE_TIMING
    for (int i = 0; i < 256; i++) {
        g_vm_profiler.op_stats[i].min_ns = UINT64_MAX;
    }
#endif
}

static inline void vm_profiler_count(int op) {
    g_vm_profiler.op_stats[op].count++;
    g_vm_profiler.total_instructions++;
}

#if XR_PROFILE_TIMING
static inline void vm_profiler_record(int op, uint64_t elapsed_ns) {
    g_vm_profiler.op_stats[op].count++;
    g_vm_profiler.op_stats[op].total_ns += elapsed_ns;
    if (elapsed_ns < g_vm_profiler.op_stats[op].min_ns) {
        g_vm_profiler.op_stats[op].min_ns = elapsed_ns;
    }
    if (elapsed_ns > g_vm_profiler.op_stats[op].max_ns) {
        g_vm_profiler.op_stats[op].max_ns = elapsed_ns;
    }
    g_vm_profiler.total_instructions++;
}
#endif

XR_FUNC void vm_profiler_report(void);

/* ========== Profiler Macros ========== */

#if XR_PROFILE_TIMING

#define VM_PROFILE_VARS() \
    uint64_t _prof_start_ns = 0; \
    int _prof_last_op = -1

#define VM_PROFILE_COUNT(op) \
    do { \
        uint64_t _now = vm_profiler_get_ns(); \
        if (_prof_last_op >= 0) { \
            vm_profiler_record(_prof_last_op, _now - _prof_start_ns); \
        } \
        _prof_last_op = (op); \
        _prof_start_ns = _now; \
    } while(0)

#define VM_PROFILE_FINISH() \
    do { \
        if (_prof_last_op >= 0) { \
            vm_profiler_record(_prof_last_op, vm_profiler_get_ns() - _prof_start_ns); \
        } \
    } while(0)

#else

#define VM_PROFILE_VARS() ((void)0)
#define VM_PROFILE_COUNT(op) vm_profiler_count(op)
#define VM_PROFILE_FINISH() ((void)0)

#endif

#else

// Disabled: zero overhead
#define vm_profiler_init()      ((void)0)
#define vm_profiler_report()    ((void)0)
#define VM_PROFILE_VARS()       ((void)0)
#define VM_PROFILE_COUNT(op)    ((void)0)
#define VM_PROFILE_FINISH()     ((void)0)

#endif

#endif // XVM_PROFILER_H
