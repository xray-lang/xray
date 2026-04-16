/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xconfig.h - Global configuration system
 *
 * KEY CONCEPT:
 *   Centralized configuration for all modules.
 *   Config stored per-Isolate, not as global variables.
 */

#ifndef XCONFIG_H
#define XCONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "xdefs.h"

/* ========== GC Config ========== */

typedef struct XrayGCConfig {
    // Heap config
    size_t heap_initial;
    size_t heap_min;
    size_t heap_max;
    double heap_growth_factor;
    
    // Debug options
    bool debug;
    bool enable_stats;
    
    // Incremental GC
    size_t step_size;
    uint64_t pause_target_us;
    
} XrayGCConfig;

/* ========== Compiler Config ========== */

typedef struct XrayCompilerConfig {
    bool enable_optimization;
    bool enable_peephole;
    bool enable_inline_cache;
    bool enable_fusion;
    int max_inline_depth;
} XrayCompilerConfig;

/* ========== VM Config ========== */

typedef struct XrayVMConfig {
    size_t stack_size;
} XrayVMConfig;

/* ========== Global Config ========== */

typedef struct XrayConfig {
    // General
    const char *version;
    bool debug;
    bool verbose;
    
    // Module configs
    XrayGCConfig gc;
    XrayCompilerConfig compiler;
    XrayVMConfig vm;
    
} XrayConfig;

/* ========== Config API ========== */

XRAY_API XrayConfig xray_config_default(void);

/* ========== Isolate Integration ========== */

XR_FUNC void xr_config_init(void *isolate_config);

#endif // XCONFIG_H
