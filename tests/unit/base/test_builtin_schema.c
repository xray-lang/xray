/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_builtin_schema.c - Unit tests for shared builtin object schemas
 */

#include "../test_framework.h"
#include "shared/xr_builtin_schema.h"

TEST(exception_field_order_is_stable) {
    ASSERT_EQ_INT(EXCEPTION_FIELD_MESSAGE, 0);
    ASSERT_EQ_INT(EXCEPTION_FIELD_STACK, 1);
    ASSERT_EQ_INT(EXCEPTION_FIELD_CAUSE, 2);
    ASSERT_EQ_INT(EXCEPTION_FIELD_CODE, 3);
    ASSERT_EQ_INT(EXCEPTION_FIELD_DATA, 4);
    ASSERT_EQ_INT(EXCEPTION_FIELD_COUNT, 5);
}

TEST(exception_field_names_match_indices) {
    ASSERT(strcmp(xr_exception_field_name(EXCEPTION_FIELD_MESSAGE), "message") == 0);
    ASSERT(strcmp(xr_exception_field_name(EXCEPTION_FIELD_STACK), "stack") == 0);
    ASSERT(strcmp(xr_exception_field_name(EXCEPTION_FIELD_CAUSE), "cause") == 0);
    ASSERT(strcmp(xr_exception_field_name(EXCEPTION_FIELD_CODE), "code") == 0);
    ASSERT(strcmp(xr_exception_field_name(EXCEPTION_FIELD_DATA), "data") == 0);
    ASSERT_NULL(xr_exception_field_name(-1));
    ASSERT_NULL(xr_exception_field_name(EXCEPTION_FIELD_COUNT));

    const char *const *names = xr_exception_field_names();
    ASSERT(strcmp(names[EXCEPTION_FIELD_MESSAGE], "message") == 0);
    ASSERT_EQ_INT(xr_exception_field_index("message"), EXCEPTION_FIELD_MESSAGE);
    ASSERT_EQ_INT(xr_exception_field_index("data"), EXCEPTION_FIELD_DATA);
    ASSERT_EQ_INT(xr_exception_field_index("missing"), -1);
    ASSERT_EQ_INT(xr_exception_field_index(NULL), -1);
}

TEST(process_field_order_is_stable) {
    ASSERT_EQ_INT(PROCESS_FIELD_FILE, 0);
    ASSERT_EQ_INT(PROCESS_FIELD_ARGS, 1);
    ASSERT_EQ_INT(PROCESS_FIELD_DIR, 2);
    ASSERT_EQ_INT(PROCESS_FIELD_COUNT, 3);
}

TEST(process_field_names_match_indices) {
    ASSERT(strcmp(xr_process_field_name(PROCESS_FIELD_FILE), "file") == 0);
    ASSERT(strcmp(xr_process_field_name(PROCESS_FIELD_ARGS), "args") == 0);
    ASSERT(strcmp(xr_process_field_name(PROCESS_FIELD_DIR), "dir") == 0);
    ASSERT_NULL(xr_process_field_name(-1));
    ASSERT_NULL(xr_process_field_name(PROCESS_FIELD_COUNT));

    ASSERT_EQ_INT(xr_process_field_index("file"), PROCESS_FIELD_FILE);
    ASSERT_EQ_INT(xr_process_field_index("args"), PROCESS_FIELD_ARGS);
    ASSERT_EQ_INT(xr_process_field_index("dir"), PROCESS_FIELD_DIR);
    ASSERT_EQ_INT(xr_process_field_index("missing"), -1);
    ASSERT_EQ_INT(xr_process_field_index(NULL), -1);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Builtin Schema");
RUN_TEST(exception_field_order_is_stable);
RUN_TEST(exception_field_names_match_indices);
RUN_TEST(process_field_order_is_stable);
RUN_TEST(process_field_names_match_indices);

TEST_MAIN_END()
