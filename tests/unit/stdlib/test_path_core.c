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

typedef struct PathCoreAllocStats {
    int allocs;
    int frees;
    int fail_after;
} PathCoreAllocStats;

static void *path_core_test_alloc(void *ctx, size_t size) {
    PathCoreAllocStats *stats = (PathCoreAllocStats *) ctx;
    if (stats) {
        if (stats->fail_after == 0)
            return NULL;
        if (stats->fail_after > 0)
            stats->fail_after--;
        stats->allocs++;
    }
    return malloc(size);
}

static void path_core_test_free(void *ctx, void *ptr) {
    PathCoreAllocStats *stats = (PathCoreAllocStats *) ctx;
    if (stats && ptr)
        stats->frees++;
    free(ptr);
}

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

static void assert_normalize_alloc(const char *path, const char *expected) {
    PathCoreAllocStats stats = {0, 0, -1};
    char *out = NULL;
    size_t out_len = 0;
    ASSERT(xr_path_core_normalize_alloc(path, path ? strlen(path) : 0, path_core_test_alloc,
                                        path_core_test_free, &stats, &out, &out_len));
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_UINT(out_len, strlen(expected));
    ASSERT_STR_EQ(out, expected);
    path_core_test_free(&stats, out);
    ASSERT_EQ_INT(stats.allocs, stats.frees);
}

TEST(path_core_normalize_alloc_writes_normalized_path) {
    assert_normalize_alloc("/usr/local/../bin/./xray", "/usr/bin/xray");
    assert_normalize_alloc("foo/bar/../../baz", "baz");
    assert_normalize_alloc("", ".");
    assert_normalize_alloc(NULL, ".");
}

TEST(path_core_normalize_alloc_cleans_segment_buffer_on_output_failure) {
    PathCoreAllocStats stats = {0, 0, 1};
    char *out = (char *) 0x1;
    size_t out_len = 99;
    ASSERT(!xr_path_core_normalize_alloc("/a/./b", strlen("/a/./b"), path_core_test_alloc,
                                         path_core_test_free, &stats, &out, &out_len));
    ASSERT_NULL(out);
    ASSERT_EQ_UINT(out_len, 4);
    ASSERT_EQ_INT(stats.allocs, stats.frees);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("path core");
RUN_TEST(path_core_constants_match_target);
RUN_TEST(path_core_format_joins_dir_and_base_once);
RUN_TEST(path_core_format_derives_base_from_name_ext);
RUN_TEST(path_core_format_base_wins_over_name_ext);
RUN_TEST(path_core_normalize_alloc_writes_normalized_path);
RUN_TEST(path_core_normalize_alloc_cleans_segment_buffer_on_output_failure);
TEST_MAIN_END()
