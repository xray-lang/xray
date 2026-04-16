/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_cache.c - Unit tests for source code cache
 *
 * KEY CONCEPT:
 *   Tests XrSourceCache operations: create, add files, retrieve lines,
 *   line counting, duplicate handling, and boundary conditions.
 */

#include "../test_framework.h"
#include "base/xsource_cache.h"

/* ========== Basic Operations ========== */

TEST(source_cache_create_free) {
    XrSourceCache *cache = xr_source_cache_new();
    ASSERT_NOT_NULL(cache);
    ASSERT_EQ_INT(cache->file_count, 0);
    xr_source_cache_free(cache);
}

TEST(source_cache_add_single) {
    XrSourceCache *cache = xr_source_cache_new();
    ASSERT_NOT_NULL(cache);

    bool ok = xr_source_cache_add(cache, "test.xr", "line1\nline2\nline3");
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(cache->file_count, 1);

    xr_source_cache_free(cache);
}

TEST(source_cache_add_multiple) {
    XrSourceCache *cache = xr_source_cache_new();

    ASSERT_TRUE(xr_source_cache_add(cache, "a.xr", "aaa"));
    ASSERT_TRUE(xr_source_cache_add(cache, "b.xr", "bbb"));
    ASSERT_TRUE(xr_source_cache_add(cache, "c.xr", "ccc"));
    ASSERT_EQ_INT(cache->file_count, 3);

    xr_source_cache_free(cache);
}

TEST(source_cache_add_duplicate) {
    XrSourceCache *cache = xr_source_cache_new();

    ASSERT_TRUE(xr_source_cache_add(cache, "test.xr", "content1"));
    // Adding same path again should return true but not add duplicate
    ASSERT_TRUE(xr_source_cache_add(cache, "test.xr", "content2"));
    ASSERT_EQ_INT(cache->file_count, 1);

    xr_source_cache_free(cache);
}

/* ========== Line Retrieval ========== */

TEST(source_cache_get_line) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "test.xr", "first\nsecond\nthird");

    // Lines are 1-indexed
    const char *line1 = xr_source_cache_get_line(cache, "test.xr", 1);
    ASSERT_NOT_NULL(line1);
    // line1 points into the content, starts at "first\n..."
    ASSERT_TRUE(strncmp(line1, "first", 5) == 0);

    const char *line2 = xr_source_cache_get_line(cache, "test.xr", 2);
    ASSERT_NOT_NULL(line2);
    ASSERT_TRUE(strncmp(line2, "second", 6) == 0);

    const char *line3 = xr_source_cache_get_line(cache, "test.xr", 3);
    ASSERT_NOT_NULL(line3);
    ASSERT_TRUE(strncmp(line3, "third", 5) == 0);

    xr_source_cache_free(cache);
}

TEST(source_cache_get_line_boundary) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "test.xr", "only");

    // Line 0 is out of range (1-indexed)
    ASSERT_NULL(xr_source_cache_get_line(cache, "test.xr", 0));
    // Line 2 is out of range for single-line content
    ASSERT_NULL(xr_source_cache_get_line(cache, "test.xr", 2));
    // Negative line
    ASSERT_NULL(xr_source_cache_get_line(cache, "test.xr", -1));

    xr_source_cache_free(cache);
}

TEST(source_cache_get_line_nonexistent_file) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "exists.xr", "content");

    ASSERT_NULL(xr_source_cache_get_line(cache, "missing.xr", 1));

    xr_source_cache_free(cache);
}

/* ========== Line Length ========== */

TEST(source_cache_line_length) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "test.xr", "hello\nworld!\nok");

    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 1), 5);   // "hello"
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 2), 6);   // "world!"
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 3), 2);   // "ok"

    xr_source_cache_free(cache);
}

TEST(source_cache_line_length_boundary) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "test.xr", "x");

    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 0), 0);
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 99), 0);
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "missing.xr", 1), 0);

    xr_source_cache_free(cache);
}

/* ========== Empty Content ========== */

TEST(source_cache_empty_lines) {
    XrSourceCache *cache = xr_source_cache_new();
    xr_source_cache_add(cache, "test.xr", "\n\n");

    // "\n\n" = 3 lines: "", "", ""
    const char *line1 = xr_source_cache_get_line(cache, "test.xr", 1);
    ASSERT_NOT_NULL(line1);
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, "test.xr", 1), 0);

    xr_source_cache_free(cache);
}

/* ========== Capacity Growth ========== */

TEST(source_cache_grow) {
    XrSourceCache *cache = xr_source_cache_new();

    // Add more files than initial capacity (4)
    char path[32];
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "file_%d.xr", i);
        ASSERT_TRUE(xr_source_cache_add(cache, path, "content"));
    }
    ASSERT_EQ_INT(cache->file_count, 10);

    // All files accessible
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "file_%d.xr", i);
        const char *line = xr_source_cache_get_line(cache, path, 1);
        ASSERT_NOT_NULL(line);
    }

    xr_source_cache_free(cache);
}

/* ========== NULL Safety ========== */

TEST(source_cache_null_safety) {
    xr_source_cache_free(NULL);  // should not crash

    XrSourceCache *cache = xr_source_cache_new();
    ASSERT_NULL(xr_source_cache_get_line(NULL, "x", 1));
    ASSERT_NULL(xr_source_cache_get_line(cache, NULL, 1));
    ASSERT_EQ_INT(xr_source_cache_get_line_length(NULL, "x", 1), 0);
    ASSERT_EQ_INT(xr_source_cache_get_line_length(cache, NULL, 1), 0);

    xr_source_cache_free(cache);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("SourceCache - Basic Operations");
    RUN_TEST(source_cache_create_free);
    RUN_TEST(source_cache_add_single);
    RUN_TEST(source_cache_add_multiple);
    RUN_TEST(source_cache_add_duplicate);

    RUN_TEST_SUITE("SourceCache - Line Retrieval");
    RUN_TEST(source_cache_get_line);
    RUN_TEST(source_cache_get_line_boundary);
    RUN_TEST(source_cache_get_line_nonexistent_file);

    RUN_TEST_SUITE("SourceCache - Line Length");
    RUN_TEST(source_cache_line_length);
    RUN_TEST(source_cache_line_length_boundary);

    RUN_TEST_SUITE("SourceCache - Edge Cases");
    RUN_TEST(source_cache_empty_lines);
    RUN_TEST(source_cache_grow);

    RUN_TEST_SUITE("SourceCache - NULL Safety");
    RUN_TEST(source_cache_null_safety);

TEST_MAIN_END()
