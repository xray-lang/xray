/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_path_core.c - Unit tests for runtime-neutral path core helpers
 */

#include "../test_framework.h"
#include "shared/xr_path_core.h"

TEST(path_core_constants_match_target) {
    ASSERT_STR_EQ(xr_path_core_sep_str(), "/");
#ifdef XR_OS_WINDOWS
    ASSERT_STR_EQ(xr_path_core_delimiter_str(), ";");
#else
    ASSERT_STR_EQ(xr_path_core_delimiter_str(), ":");
#endif
}

static void assert_format(const char *dir, const char *base, const char *name, const char *ext,
                          const char *expected) {
    XrPathCoreFormatPlan plan;
    ASSERT(xr_path_core_format_plan(xr_path_core_slice(dir, dir ? strlen(dir) : 0),
                                    xr_path_core_slice(base, base ? strlen(base) : 0),
                                    xr_path_core_slice(name, name ? strlen(name) : 0),
                                    xr_path_core_slice(ext, ext ? strlen(ext) : 0), &plan));
    ASSERT_EQ_UINT(plan.out_len, strlen(expected));
    char buf[128];
    xr_path_core_format_write(&plan, buf);
    ASSERT_STR_EQ(buf, expected);
}

TEST(path_core_format_joins_dir_and_base_once) {
    assert_format("/home/user", "file.txt", "", "", "/home/user/file.txt");
    assert_format("/home/user/", "file.txt", "", "", "/home/user/file.txt");
}

TEST(path_core_format_derives_base_from_name_ext) {
    assert_format("", "", "archive", ".tar", "archive.tar");
    assert_format("/tmp", "", "archive", ".tar", "/tmp/archive.tar");
}

TEST(path_core_format_base_wins_over_name_ext) {
    assert_format("/tmp", "file.txt", "ignored", ".bak", "/tmp/file.txt");
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("path core");
RUN_TEST(path_core_constants_match_target);
RUN_TEST(path_core_format_joins_dir_and_base_once);
RUN_TEST(path_core_format_derives_base_from_name_ext);
RUN_TEST(path_core_format_base_wins_over_name_ext);
TEST_MAIN_END()
