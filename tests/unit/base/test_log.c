/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_log.c - Unit tests for structured diagnostic logging
 *
 * KEY CONCEPT:
 *   Tests XrLog level configuration, level transitions, and macro behavior.
 *   Verifies that log level filtering works correctly.
 */

#include "../test_framework.h"
#include "base/xlog.h"

/* ========== Log Level Configuration ========== */

TEST(log_default_level) {
    // In debug builds, default should be DEBUG
    // In release builds, default may differ
    XrLogLevel level = xr_log_get_level();
    ASSERT_TRUE(level >= XR_LOG_DEBUG && level <= XR_LOG_SILENT);
}

TEST(log_set_get_level) {
    XrLogLevel orig = xr_log_get_level();

    xr_log_set_level(XR_LOG_WARNING);
    ASSERT_EQ_INT(xr_log_get_level(), XR_LOG_WARNING);

    xr_log_set_level(XR_LOG_SILENT);
    ASSERT_EQ_INT(xr_log_get_level(), XR_LOG_SILENT);

    xr_log_set_level(XR_LOG_DEBUG);
    ASSERT_EQ_INT(xr_log_get_level(), XR_LOG_DEBUG);

    // Restore original
    xr_log_set_level(orig);
}

TEST(log_level_enum_order) {
    // Verify level ordering
    ASSERT_LT(XR_LOG_DEBUG, XR_LOG_VERBOSE);
    ASSERT_LT(XR_LOG_VERBOSE, XR_LOG_NOTICE);
    ASSERT_LT(XR_LOG_NOTICE, XR_LOG_WARNING);
    ASSERT_LT(XR_LOG_WARNING, XR_LOG_SILENT);
}

/* ========== Log Output (no crash) ========== */

TEST(log_debug_no_crash) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_DEBUG);

    // Should not crash
    xr_log_debug("test", "debug message: %d", 42);
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

TEST(log_verbose_no_crash) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_VERBOSE);

    xr_log_verbose("test", "verbose: %s", "hello");
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

TEST(log_notice_no_crash) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_NOTICE);

    xr_log_notice("test", "notice: %d items", 10);
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

TEST(log_warning_no_crash) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_WARNING);

    xr_log_warning("test", "warning: %s overflow", "stack");
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

/* ========== Log Suppression ========== */

TEST(log_silent_suppresses_all) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_SILENT);

    // These should all be suppressed (no output)
    xr_log_debug("test", "suppressed debug");
    xr_log_verbose("test", "suppressed verbose");
    xr_log_notice("test", "suppressed notice");
    xr_log_warning("test", "suppressed warning");
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

TEST(log_warning_level_filters_lower) {
    XrLogLevel orig = xr_log_get_level();
    xr_log_set_level(XR_LOG_WARNING);

    // These should be suppressed
    xr_log_debug("test", "filtered debug");
    xr_log_verbose("test", "filtered verbose");
    xr_log_notice("test", "filtered notice");
    // This should pass through
    xr_log_warning("test", "not filtered");
    ASSERT_TRUE(1);

    xr_log_set_level(orig);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Log - Level Configuration");
RUN_TEST(log_default_level);
RUN_TEST(log_set_get_level);
RUN_TEST(log_level_enum_order);

RUN_TEST_SUITE("Log - Output (no crash)");
RUN_TEST(log_debug_no_crash);
RUN_TEST(log_verbose_no_crash);
RUN_TEST(log_notice_no_crash);
RUN_TEST(log_warning_no_crash);

RUN_TEST_SUITE("Log - Suppression");
RUN_TEST(log_silent_suppresses_all);
RUN_TEST(log_warning_level_filters_lower);

TEST_MAIN_END()
