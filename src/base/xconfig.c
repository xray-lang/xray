/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xconfig.c - Global configuration system implementation
 *
 * KEY CONCEPT:
 *   Provides default configuration values.
 *   Config stored per-Isolate, not as global variables.
 */

#include "xconfig.h"
#include "xchecks.h"
#include <string.h>

/* ========== Default Config ========== */

XrayConfig xray_config_default(void) {
    XrayConfig config = {
        .version = "0.32.4",
        .debug = true,
        .verbose = false,
        
        .gc = {
            .heap_initial = 4 * 1024 * 1024,      // 4MB
            .heap_min = 1 * 1024 * 1024,          // 1MB
            .heap_max = 1024 * 1024 * 1024,       // 1GB
            .heap_growth_factor = 2.0,
            .debug = true,
            .enable_stats = true,
            .step_size = 1000,
            .pause_target_us = 2000,
        },
        
        .compiler = {
            .enable_optimization = true,
            .enable_peephole = true,
            .enable_inline_cache = true,
            .enable_fusion = true,
            .max_inline_depth = 3,
        },
        
        .vm = {
            .stack_size = 65536,
        },
    };
    
    return config;
}

/* ========== Config Initialization ========== */

// Initialize config in Isolate
void xr_config_init(void *isolate_config) {
    if (isolate_config == NULL) return;
    XrayConfig *config = (XrayConfig *)isolate_config;
    *config = xray_config_default();
}

