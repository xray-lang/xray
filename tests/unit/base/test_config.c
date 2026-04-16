/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_config.c - Unit tests for global configuration system
 *
 * KEY CONCEPT:
 *   Tests XrayConfig default values and config initialization
 *   for Isolate integration.
 */

#include "../test_framework.h"
#include "base/xconfig.h"
#include <stdio.h>

/* ========== Default Config ========== */

TEST(config_default_values) {
    XrayConfig cfg = xray_config_default();

    ASSERT_NOT_NULL(cfg.version);
    ASSERT_TRUE(cfg.debug);
    ASSERT_FALSE(cfg.verbose);

    // GC defaults
    ASSERT_EQ_UINT(cfg.gc.heap_initial, 4 * 1024 * 1024);
    ASSERT_EQ_UINT(cfg.gc.heap_min, 1 * 1024 * 1024);
    ASSERT_EQ_UINT(cfg.gc.heap_max, 1024UL * 1024 * 1024);
    ASSERT_FLOAT_EQ(cfg.gc.heap_growth_factor, 2.0, 0.01);
    ASSERT_TRUE(cfg.gc.debug);
    ASSERT_TRUE(cfg.gc.enable_stats);
    ASSERT_EQ_UINT(cfg.gc.step_size, 1000);
    ASSERT_EQ_UINT(cfg.gc.pause_target_us, 2000);

    // Compiler defaults
    ASSERT_TRUE(cfg.compiler.enable_optimization);
    ASSERT_TRUE(cfg.compiler.enable_peephole);
    ASSERT_TRUE(cfg.compiler.enable_inline_cache);
    ASSERT_TRUE(cfg.compiler.enable_fusion);
    ASSERT_EQ_INT(cfg.compiler.max_inline_depth, 3);

    // VM defaults
    ASSERT_EQ_UINT(cfg.vm.stack_size, 65536);
}

/* ========== Config Init ========== */

TEST(config_init) {
    XrayConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    xr_config_init(&cfg);

    // After init, should have default values
    ASSERT_NOT_NULL(cfg.version);
    ASSERT_TRUE(cfg.debug);
    ASSERT_EQ_UINT(cfg.gc.heap_initial, 4 * 1024 * 1024);
    ASSERT_TRUE(cfg.compiler.enable_optimization);
}

TEST(config_init_null) {
    // Should not crash
    xr_config_init(NULL);
    ASSERT_TRUE(1);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("Config - Default Values");
    RUN_TEST(config_default_values);

    RUN_TEST_SUITE("Config - Init");
    RUN_TEST(config_init);
    RUN_TEST(config_init_null);

TEST_MAIN_END()
