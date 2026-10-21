/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_reporting.c - Observable reports from successful and failing test cases
 *
 * KEY CONCEPT:
 *   Failed assertions must not produce a passing case or hide later cases.
 *   Multiple helper failures still belong to one registered test case.
 */
#include "../test_framework.h"

TEST(success) {
    ASSERT_EQ_INT(42, 42);
}

TEST(direct_failure) {
    ASSERT_MSG(0, "intentional direct failure");
}

static void fail_helper(void) {
    ASSERT_MSG(0, "intentional helper failure");
}

TEST(helper_failure) {
    fail_helper();
    fail_helper();
    puts("helper body continued");
}

TEST(after_failure) {
    puts("later case executed");
    ASSERT(1);
}

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "success")) {
        RUN_TEST(success);
    } else if (!strcmp(argv[1], "direct_failure")) {
        RUN_TEST(direct_failure);
    } else if (!strcmp(argv[1], "helper_failure")) {
        RUN_TEST(helper_failure);
    } else {
        return 2;
    }
    RUN_TEST(after_failure);
    printf("COUNTERS run=%d passed=%d assertions_failed=%d\n",
        xr_tests_run, xr_tests_passed, xr_tests_failed);
    TEST_REPORT();
    return TEST_EXIT();
}
