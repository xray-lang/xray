/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_io_core.c - Tests for the remaining runtime-neutral IO ABI helpers.
 */

#include "../test_framework.h"
#include "shared/xr_io_core.h"

#include <string.h>

TEST(io_core_path_result_view_normalizes_windows_extended_prefix) {
    XrIoCorePathView view = {0};
    ASSERT_TRUE(xr_io_core_path_result_view("plain", 5, &view));
    ASSERT_EQ_UINT(view.len, 5);
    ASSERT_MEM_EQ(view.data, "plain", 5);

    ASSERT_TRUE(xr_io_core_path_result_cstr_view("\\\\?\\C:\\tmp", &view));
    ASSERT_EQ_UINT(view.len, 6);
    ASSERT_MEM_EQ(view.data, "C:\\tmp", 6);
}

TEST(io_core_path_result_view_rejects_invalid_args) {
    XrIoCorePathView view = {.data = "stale", .len = 5};
    ASSERT_FALSE(xr_io_core_path_result_view(NULL, 0, &view));
    ASSERT_NULL(view.data);
    ASSERT_EQ_UINT(view.len, 0);
    ASSERT_FALSE(xr_io_core_path_result_view("x", 1, NULL));
}

TEST(io_core_stat_schema_and_projection_are_stable) {
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_SIZE], "size");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_FILE], "isFile");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_DIR], "isDir");
    ASSERT_STR_EQ(XR_IO_CORE_STAT_FIELD_NAMES[XR_IO_CORE_STAT_IS_SYMLINK], "isSymlink");

    XrIoCoreStatFields fields =
        xr_io_core_stat_fields(12, 0100644, 1, 2, 3, 501, 20, true, false, true);
    ASSERT_EQ_INT(fields.size, 12);
    ASSERT_EQ_INT(fields.mode, 0644);
    ASSERT_EQ_INT(fields.mtime, 1);
    ASSERT_EQ_INT(fields.atime, 2);
    ASSERT_EQ_INT(fields.ctime, 3);
    ASSERT_EQ_INT(fields.uid, 501);
    ASSERT_EQ_INT(fields.gid, 20);
    ASSERT_TRUE(fields.is_file);
    ASSERT_FALSE(fields.is_dir);
    ASSERT_TRUE(fields.is_symlink);
}

TEST(io_core_chmod_mode_checks_the_host_int_range) {
    int mode = -1;
    ASSERT_TRUE(xr_io_core_chmod_mode(0644, &mode));
    ASSERT_EQ_INT(mode, 0644);
    ASSERT_TRUE(xr_io_core_chmod_mode(0, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_TRUE(xr_io_core_chmod_mode(INT_MAX, &mode));
    ASSERT_EQ_INT(mode, INT_MAX);

    ASSERT_FALSE(xr_io_core_chmod_mode(-1, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_FALSE(xr_io_core_chmod_mode((int64_t) INT_MAX + 1, &mode));
    ASSERT_EQ_INT(mode, 0);
    ASSERT_FALSE(xr_io_core_chmod_mode(0644, NULL));
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("IO Core - path result ABI");
RUN_TEST(io_core_path_result_view_normalizes_windows_extended_prefix);
RUN_TEST(io_core_path_result_view_rejects_invalid_args);

RUN_TEST_SUITE("IO Core - stat and chmod ABI");
RUN_TEST(io_core_stat_schema_and_projection_are_stable);
RUN_TEST(io_core_chmod_mode_checks_the_host_int_range);

TEST_MAIN_END()
