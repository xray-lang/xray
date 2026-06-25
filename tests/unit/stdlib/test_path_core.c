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

static void assert_slice_eq(XrPathCoreSlice slice, const char *expected) {
    size_t expected_len = expected ? strlen(expected) : 0;
    ASSERT_EQ_UINT(slice.len, expected_len);
    if (expected_len > 0) {
        ASSERT_NOT_NULL(slice.data);
        ASSERT_EQ_INT(memcmp(slice.data, expected, expected_len), 0);
    }
}

TEST(path_core_parse_schema_matches_pathinfo_order) {
    ASSERT_EQ_INT(XR_PATH_CORE_PARSE_FIELD_COUNT, 5);
    ASSERT_STR_EQ(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_ROOT), "root");
    ASSERT_STR_EQ(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_DIR), "dir");
    ASSERT_STR_EQ(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_BASE), "base");
    ASSERT_STR_EQ(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_NAME), "name");
    ASSERT_STR_EQ(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_EXT), "ext");
    ASSERT_NULL(xr_path_core_parse_field_name(XR_PATH_CORE_PARSE_FIELD_COUNT));

    const char *const *names = xr_path_core_parse_field_names();
    ASSERT_STR_EQ(names[0], "root");
    ASSERT_STR_EQ(names[1], "dir");
    ASSERT_STR_EQ(names[2], "base");
    ASSERT_STR_EQ(names[3], "name");
    ASSERT_STR_EQ(names[4], "ext");
}

TEST(path_core_parse_plan_field_accessor_follows_schema) {
    const char *path = "/home/user/file.txt";
    XrPathCoreParsePlan plan;
    ASSERT(xr_path_core_parse_plan(path, strlen(path), &plan));

    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_ROOT), "/");
    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_DIR),
                    "/home/user");
    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_BASE),
                    "file.txt");
    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_NAME), "file");
    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_EXT), ".txt");
    assert_slice_eq(xr_path_core_parse_plan_field(&plan, XR_PATH_CORE_PARSE_FIELD_COUNT), "");
    assert_slice_eq(xr_path_core_parse_plan_field(NULL, XR_PATH_CORE_PARSE_FIELD_ROOT), "");
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

TEST(path_core_resolve_parts_keeps_absolute_inputs_without_cwd) {
    const char *input[] = {"rel", "/abs", "tail"};
    size_t lens[] = {3, 4, 4};
    const char *out[4] = {0};
    size_t out_lens[4] = {0};
    size_t out_count = 0;

    ASSERT(!xr_path_core_resolve_needs_cwd(input, lens, 3));
    ASSERT(xr_path_core_resolve_parts(input, lens, 3, "/cwd", 4, out, out_lens, 4, &out_count));
    ASSERT_EQ_UINT(out_count, 3);
    ASSERT_EQ_PTR(out[0], input[0]);
    ASSERT_EQ_PTR(out[1], input[1]);
    ASSERT_EQ_PTR(out[2], input[2]);
    ASSERT_EQ_UINT(out_lens[0], 3);
    ASSERT_EQ_UINT(out_lens[1], 4);
    ASSERT_EQ_UINT(out_lens[2], 4);
}

TEST(path_core_resolve_parts_prepends_cwd_for_relative_inputs) {
    const char *input[] = {"rel", "tail"};
    size_t lens[] = {3, 4};
    const char *out[4] = {0};
    size_t out_lens[4] = {0};
    size_t out_count = 0;

    ASSERT(xr_path_core_resolve_needs_cwd(input, lens, 2));
    ASSERT(xr_path_core_resolve_parts(input, lens, 2, "/cwd", 4, out, out_lens, 4, &out_count));
    ASSERT_EQ_UINT(out_count, 3);
    ASSERT_STR_EQ(out[0], "/cwd");
    ASSERT_EQ_PTR(out[1], input[0]);
    ASSERT_EQ_PTR(out[2], input[1]);
    ASSERT_EQ_UINT(out_lens[0], 4);
    ASSERT_EQ_UINT(out_lens[1], 3);
    ASSERT_EQ_UINT(out_lens[2], 4);
}

TEST(path_core_resolve_fallback_cwd_is_root) {
    char cwd[8] = {0};
    xr_path_core_resolve_fallback_cwd(cwd, sizeof(cwd));
    ASSERT_STR_EQ(cwd, "/");
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("path core");
RUN_TEST(path_core_constants_match_target);
RUN_TEST(path_core_parse_schema_matches_pathinfo_order);
RUN_TEST(path_core_parse_plan_field_accessor_follows_schema);
RUN_TEST(path_core_format_joins_dir_and_base_once);
RUN_TEST(path_core_format_derives_base_from_name_ext);
RUN_TEST(path_core_format_base_wins_over_name_ext);
RUN_TEST(path_core_normalize_alloc_writes_normalized_path);
RUN_TEST(path_core_normalize_alloc_cleans_segment_buffer_on_output_failure);
RUN_TEST(path_core_resolve_parts_keeps_absolute_inputs_without_cwd);
RUN_TEST(path_core_resolve_parts_prepends_cwd_for_relative_inputs);
RUN_TEST(path_core_resolve_fallback_cwd_is_root);
TEST_MAIN_END()
