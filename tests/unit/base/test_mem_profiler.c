/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mem_profiler.c - Unit tests for memory profiler stats
 *
 * KEY CONCEPT:
 *   Tests XrProfileStats init, alloc tracking, free tracking,
 *   peak tracking, and accumulation behavior.
 */

#include "../test_framework.h"
#include "base/xmem_profiler.h"

/* ========== Stats Init ========== */

TEST(profiler_stats_init) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);
    ASSERT_EQ_UINT(stats.alloc_count, 0);
    ASSERT_EQ_UINT(stats.free_count, 0);
    ASSERT_EQ_UINT(stats.alloc_bytes, 0);
    ASSERT_EQ_UINT(stats.free_bytes, 0);
    ASSERT_EQ_UINT(stats.peak_bytes, 0);
    ASSERT_EQ_UINT(stats.current_bytes, 0);
}

/* ========== Alloc Tracking ========== */

TEST(profiler_stats_alloc) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);

    xr_profile_stats_alloc(&stats, 100);
    ASSERT_EQ_UINT(stats.alloc_count, 1);
    ASSERT_EQ_UINT(stats.alloc_bytes, 100);
    ASSERT_EQ_UINT(stats.current_bytes, 100);
    ASSERT_EQ_UINT(stats.peak_bytes, 100);

    xr_profile_stats_alloc(&stats, 200);
    ASSERT_EQ_UINT(stats.alloc_count, 2);
    ASSERT_EQ_UINT(stats.alloc_bytes, 300);
    ASSERT_EQ_UINT(stats.current_bytes, 300);
    ASSERT_EQ_UINT(stats.peak_bytes, 300);
}

/* ========== Free Tracking ========== */

TEST(profiler_stats_free) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);

    xr_profile_stats_alloc(&stats, 500);
    xr_profile_stats_free(&stats, 200);

    ASSERT_EQ_UINT(stats.free_count, 1);
    ASSERT_EQ_UINT(stats.free_bytes, 200);
    ASSERT_EQ_UINT(stats.current_bytes, 300);
    ASSERT_EQ_UINT(stats.peak_bytes, 500);  // peak unchanged
}

/* ========== Peak Tracking ========== */

TEST(profiler_stats_peak) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);

    xr_profile_stats_alloc(&stats, 1000);  // current=1000, peak=1000
    xr_profile_stats_free(&stats, 600);    // current=400, peak=1000
    xr_profile_stats_alloc(&stats, 300);   // current=700, peak=1000
    xr_profile_stats_alloc(&stats, 500);   // current=1200, peak=1200

    ASSERT_EQ_UINT(stats.current_bytes, 1200);
    ASSERT_EQ_UINT(stats.peak_bytes, 1200);

    xr_profile_stats_free(&stats, 1200);  // current=0, peak=1200
    ASSERT_EQ_UINT(stats.current_bytes, 0);
    ASSERT_EQ_UINT(stats.peak_bytes, 1200);  // peak preserved
}

/* ========== Multiple Alloc/Free Cycles ========== */

TEST(profiler_stats_cycles) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);

    for (int i = 0; i < 100; i++) {
        xr_profile_stats_alloc(&stats, 64);
    }
    ASSERT_EQ_UINT(stats.alloc_count, 100);
    ASSERT_EQ_UINT(stats.alloc_bytes, 6400);

    for (int i = 0; i < 50; i++) {
        xr_profile_stats_free(&stats, 64);
    }
    ASSERT_EQ_UINT(stats.free_count, 50);
    ASSERT_EQ_UINT(stats.current_bytes, 3200);
    ASSERT_EQ_UINT(stats.peak_bytes, 6400);
}

/* ========== Free More Than Current (edge case) ========== */

TEST(profiler_stats_free_underflow) {
    XrProfileStats stats;
    xr_profile_stats_init(&stats);

    xr_profile_stats_alloc(&stats, 100);
    xr_profile_stats_free(&stats, 200);  // free more than current

    // Implementation skips subtract when current < bytes (no underflow)
    // current_bytes stays at 100 since 100 < 200
    ASSERT_EQ_UINT(stats.current_bytes, 100);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("MemProfiler - Init");
RUN_TEST(profiler_stats_init);

RUN_TEST_SUITE("MemProfiler - Alloc");
RUN_TEST(profiler_stats_alloc);

RUN_TEST_SUITE("MemProfiler - Free");
RUN_TEST(profiler_stats_free);

RUN_TEST_SUITE("MemProfiler - Peak");
RUN_TEST(profiler_stats_peak);

RUN_TEST_SUITE("MemProfiler - Cycles");
RUN_TEST(profiler_stats_cycles);

RUN_TEST_SUITE("MemProfiler - Edge Cases");
RUN_TEST(profiler_stats_free_underflow);

TEST_MAIN_END()
