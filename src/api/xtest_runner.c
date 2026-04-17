/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtest_runner.c - Built-in test framework implementation
 */

#include "xtest_runner.h"
#include "../base/xchecks.h"
#include "../vm/xvm.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue.h"
#include "../app/cli/xcli_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../base/xmalloc.h"

/* ========== Internal Helpers ========== */

static void suite_add_test(XrTestSuite *suite, XrProto *proto, int idx, AttributeKind attr, int timeout) {
    // Expand capacity
    if (suite->test_count >= suite->test_capacity) {
        suite->test_capacity = suite->test_capacity == 0 ? 8 : suite->test_capacity * 2;
        suite->tests = (XrTestFunc *)xr_realloc(suite->tests, sizeof(XrTestFunc) * suite->test_capacity);
    }

    XrTestFunc *func = &suite->tests[suite->test_count++];
    func->proto = proto;
    func->closure_idx = idx;
    func->attr = attr;
    func->timeout = timeout;
}

static void suite_add_hook(XrTestFunc **hooks, int *count, XrProto *proto, int idx) {
    *hooks = (XrTestFunc *)xr_realloc(*hooks, sizeof(XrTestFunc) * (*count + 1));
    XrTestFunc *func = &(*hooks)[*count];
    func->proto = proto;
    func->closure_idx = idx;
    func->attr = ATTR_NONE;
    func->timeout = 0;
    (*count)++;
}

/* ========== Test Runner API ========== */

XrTestRunner* xr_test_runner_new(void) {
    XrTestRunner *runner = (XrTestRunner *)xr_malloc(sizeof(XrTestRunner));
    memset(runner, 0, sizeof(XrTestRunner));
    return runner;
}

void xr_test_runner_free(XrTestRunner *runner) {
    if (!runner) return;
    for (int i = 0; i < runner->failure_record_count; i++) {
        xr_free(runner->failure_records[i].file);
        xr_free(runner->failure_records[i].test_name);
        xr_free(runner->failure_records[i].message);
    }
    xr_free(runner->failure_records);
    xr_free(runner);
}

void xr_test_runner_configure(XrTestRunner *runner, XrTestConfig *config) {
    XR_DCHECK(runner != NULL, "test_runner_configure: NULL runner");
    if (config) {
        runner->config = *config;
    }
}

/* ========== Test Discovery ========== */

XrTestSuite* xr_test_discover(XrProto *proto, const char *suite_name) {
    XR_DCHECK(proto != NULL, "test_discover: NULL proto");
    XrTestSuite *suite = (XrTestSuite *)xr_malloc(sizeof(XrTestSuite));
    memset(suite, 0, sizeof(XrTestSuite));
    suite->name = suite_name;

    // Scan nested protos (function definitions)
    int proto_count = PROTO_PROTO_COUNT(proto);
    for (int i = 0; i < proto_count; i++) {
        XrProto *child = PROTO_PROTO(proto, i);

        if (child->test_attr != ATTR_NONE) {
            AttributeKind attr = (AttributeKind)child->test_attr;

            switch (attr) {
                case ATTR_TEST:
                case ATTR_TEST_SKIP:
                case ATTR_TEST_TIMEOUT:
                    suite_add_test(suite, child, i, attr, child->test_timeout);
                    break;

                case ATTR_BEFORE_EACH:
                    suite_add_hook(&suite->before_each, &suite->before_each_count, child, i);
                    break;

                case ATTR_AFTER_EACH:
                    suite_add_hook(&suite->after_each, &suite->after_each_count, child, i);
                    break;

                case ATTR_BEFORE_ALL:
                    suite_add_hook(&suite->before_all, &suite->before_all_count, child, i);
                    break;

                case ATTR_AFTER_ALL:
                    suite_add_hook(&suite->after_all, &suite->after_all_count, child, i);
                    break;

                default:
                    break;
            }
        }
    }

    // Allocate results array
    if (suite->test_count > 0) {
        size_t size = suite->test_count * sizeof(XrTestResult);
        suite->results = (XrTestResult *)xr_malloc(size);
        memset(suite->results, 0, size);
    }

    return suite;
}

/* ========== Failure Tracking ========== */

void xr_test_runner_add_failure(XrTestRunner *runner, const char *file,
                                const char *test_name, const char *message,
                                XrTestStatus status) {
    XR_DCHECK(runner != NULL, "test_runner_add_failure: NULL runner");
    if (runner->failure_record_count >= runner->failure_record_capacity) {
        runner->failure_record_capacity = runner->failure_record_capacity == 0
            ? 8 : runner->failure_record_capacity * 2;
        XR_REALLOC_OR_ABORT(runner->failure_records,
            runner->failure_record_capacity * sizeof(XrTestFailureRecord),
            "test_runner failure_records grow");
    }
    XrTestFailureRecord *rec = &runner->failure_records[runner->failure_record_count++];
    const char *f = file ? file : "<unknown>";
    const char *t = test_name ? test_name : "<anonymous>";
    const char *m = message ? message : "";
    rec->file = xr_malloc(strlen(f) + 1); memcpy(rec->file, f, strlen(f) + 1);
    rec->test_name = xr_malloc(strlen(t) + 1); memcpy(rec->test_name, t, strlen(t) + 1);
    rec->message = xr_malloc(strlen(m) + 1); memcpy(rec->message, m, strlen(m) + 1);
    rec->status = status;
}

/* ========== Test Report ========== */

void xr_test_print_report(XrTestRunner *runner) {
    XR_DCHECK(runner != NULL, "test_print_report: NULL runner");
    int total_problems = runner->failed_tests + runner->error_tests + runner->timeout_tests;

    // Failures detail section
    if (runner->failure_record_count > 0) {
        printf("\n " CLR_RED CLR_BOLD "Failed Tests" CLR_RESET "\n\n");
        for (int i = 0; i < runner->failure_record_count; i++) {
            XrTestFailureRecord *rec = &runner->failure_records[i];
            const char *base = strrchr(rec->file, '/');
            base = base ? base + 1 : rec->file;
            // Strip .xr extension for display
            char name_buf[256];
            strncpy(name_buf, base, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            char *dot = strrchr(name_buf, '.');
            if (dot && strcmp(dot, ".xr") == 0) *dot = '\0';
            printf("  " CLR_RED "\u2717" CLR_RESET " %s " CLR_DIM ">" CLR_RESET " %s\n",
                   name_buf, rec->test_name);
            if (rec->message[0] != '\0') {
                printf("    " CLR_DIM "%s" CLR_RESET "\n", rec->message);
            }
        }
    }

    // Summary
    printf("\n " CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" CLR_RESET "\n");

    // Test counts line
    printf(" " CLR_BOLD " Tests" CLR_RESET "  ");
    printf("%d file%s", runner->file_count, runner->file_count == 1 ? "" : "s");
    printf(CLR_DIM " | " CLR_RESET);
    if (total_problems == 0) {
        printf(CLR_GREEN CLR_BOLD "%d passed" CLR_RESET, runner->passed_tests);
    } else {
        printf(CLR_GREEN "%d passed" CLR_RESET, runner->passed_tests);
        printf(CLR_DIM " | " CLR_RESET);
        printf(CLR_RED CLR_BOLD "%d failed" CLR_RESET, total_problems);
    }
    if (runner->skipped_tests > 0) {
        printf(CLR_DIM " | " CLR_RESET);
        printf(CLR_DIM "%d skipped" CLR_RESET, runner->skipped_tests);
    }
    if (runner->config.filter) {
        printf("  " CLR_DIM "(filter: \"%s\")" CLR_RESET, runner->config.filter);
    }
    printf("\n");

    // Time line
    double time_ms = runner->total_time_ms;
    printf(" " CLR_BOLD "  Time" CLR_RESET "  ");
    if (time_ms >= 1000.0) {
        printf("%.2fs\n", time_ms / 1000.0);
    } else {
        printf("%.0fms\n", time_ms);
    }

    printf(" " CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" CLR_RESET "\n");

    if (total_problems == 0) {
        printf("\n " CLR_GREEN CLR_BOLD "\u2713 All tests passed" CLR_RESET "\n\n");
    } else {
        printf("\n " CLR_RED CLR_BOLD "\u2717 %d test%s failed" CLR_RESET "\n\n",
               total_problems, total_problems == 1 ? "" : "s");
    }
}

/* ========== Cleanup ========== */

void xr_test_suite_free(XrTestSuite *suite) {
    if (!suite) return;

    if (suite->tests) xr_free(suite->tests);
    if (suite->before_each) xr_free(suite->before_each);
    if (suite->after_each) xr_free(suite->after_each);
    if (suite->before_all) xr_free(suite->before_all);
    if (suite->after_all) xr_free(suite->after_all);
    if (suite->results) xr_free(suite->results);

    xr_free(suite);
}
