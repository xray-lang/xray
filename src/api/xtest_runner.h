/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtest_runner.h - Built-in test framework runner
 *
 * KEY CONCEPT:
 *   Test discovery, execution, and reporting for xray scripts.
 *   Supports @test, @before_each, @after_each, @before_all, @after_all hooks.
 */

#ifndef XTEST_RUNNER_H
#define XTEST_RUNNER_H

#include <xray.h>
#include "../runtime/value/xchunk.h"
#include "../frontend/parser/xast.h"  // AttributeKind
#include <stdbool.h>

/* ========== Test Result ========== */

typedef enum {
    TEST_PASSED,
    TEST_FAILED,
    TEST_ERROR,
    TEST_SKIPPED,
    TEST_TIMEOUT
} XrTestStatus;

typedef struct XrTestResult {
    const char *name;
    XrTestStatus status;
    const char *message;
    double duration_ms;
} XrTestResult;

/* ========== Test Function ========== */

typedef struct XrTestFunc {
    XrProto *proto;
    int closure_idx;  // Index in parent proto's constant table
    AttributeKind attr;
    int timeout;  // Timeout in seconds
} XrTestFunc;

/* ========== Test Suite ========== */

typedef struct XrTestSuite {
    const char *name;

    // Test functions
    XrTestFunc *tests;
    int test_count;
    int test_capacity;

    // Hook functions
    XrTestFunc *before_each;
    int before_each_count;
    XrTestFunc *after_each;
    int after_each_count;
    XrTestFunc *before_all;
    int before_all_count;
    XrTestFunc *after_all;
    int after_all_count;

    // Results
    XrTestResult *results;
    int result_count;
} XrTestSuite;

/* ========== Test Runner ========== */

typedef struct XrTestConfig {
    bool verbose;
    bool fail_fast;
    const char *filter;  // NULL = run all
} XrTestConfig;

// Failure record for end-of-run summary
typedef struct XrTestFailureRecord {
    char *file;       // owned copy
    char *test_name;  // owned copy
    char *message;    // owned copy
    XrTestStatus status;
} XrTestFailureRecord;

typedef struct XrTestRunner {
    XrTestConfig config;

    // Statistics
    int file_count;
    int total_tests;
    int passed_tests;
    int failed_tests;
    int skipped_tests;
    int error_tests;
    int timeout_tests;
    double total_time_ms;

    // Display alignment (max filepath length for dot-padding)
    int align_width;

    // Failure records for end-of-run summary
    XrTestFailureRecord *failure_records;
    int failure_record_count;
    int failure_record_capacity;
} XrTestRunner;

/* ========== API ========== */

XR_FUNC XrTestRunner *xr_test_runner_new(void);
XR_FUNC void xr_test_runner_free(XrTestRunner *runner);
XR_FUNC void xr_test_runner_configure(XrTestRunner *runner, XrTestConfig *config);

// Discover test functions from compiled proto
XR_FUNC XrTestSuite *xr_test_discover(XrProto *proto, const char *suite_name);

XR_FUNC void xr_test_runner_add_failure(XrTestRunner *runner, const char *file,
                                        const char *test_name, const char *message,
                                        XrTestStatus status);
XR_FUNC void xr_test_print_report(XrTestRunner *runner);
XR_FUNC void xr_test_suite_free(XrTestSuite *suite);

#endif  // XTEST_RUNNER_H
