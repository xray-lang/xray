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


typedef struct CacheTestOwner {
    XrCompileResources *resources;
    XrCompileState *state;
    XrSourceCache *cache;
    XrCompileResourceStats baseline;
} CacheTestOwner;

/* The test caller owns this finite ledger; the cache retains its shared state. */
static bool cache_test_open(CacheTestOwner *owner) {
    const XrCompileResourceLimits limits={1048576,1048576,1048576};
    XrCompileResourceStatus status=xr_compile_resources_new(&limits,&owner->resources);
    if(status==XR_COMPILE_RESOURCE_OK)
        status=xr_compile_resources_stats(owner->resources,&owner->baseline);
    if(status==XR_COMPILE_RESOURCE_OK)
        status=xr_compile_state_new(owner->resources,&owner->state);
    if(status==XR_COMPILE_RESOURCE_OK)
        status=xr_compile_source_cache_open(owner->state,&owner->cache);
    if(status==XR_COMPILE_RESOURCE_OK)return true;
    xr_owned_source_cache_close(owner->cache);
    xr_compile_state_release(owner->state);
    xr_compile_resources_release(owner->resources);
    *owner=(CacheTestOwner){0};return false;
}

static bool cache_test_close(CacheTestOwner *owner) {
    XrCompileResourceStats before={0},after={0};
    bool ok=xr_compile_resources_stats(owner->resources,&before)==XR_COMPILE_RESOURCE_OK;
    xr_owned_source_cache_close(owner->cache);owner->cache=NULL;
    xr_compile_state_release(owner->state);owner->state=NULL;
    ok=ok && xr_compile_resources_stats(owner->resources,&after)==XR_COMPILE_RESOURCE_OK;
    ok=ok && after.live_bytes==owner->baseline.live_bytes &&
        after.allocated_bytes==before.allocated_bytes && after.work==before.work &&
        after.peak_bytes==before.peak_bytes && after.allocation_count==before.allocation_count;
    xr_compile_resources_release(owner->resources);owner->resources=NULL;
    return ok;
}

/* ========== Basic Operations ========== */

TEST(source_cache_create_free) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_NOT_NULL(cache);
    ASSERT_EQ_INT(cache->file_count, 0);
    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_add_single) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_NOT_NULL(cache);

    XrCompileResourceStatus status=xr_compile_source_cache_add(cache, "test.xr", "line1\nline2\nline3");
    ASSERT_EQ_INT(status,XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(cache->file_count, 1);

    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_add_multiple) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;

    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "a.xr", "aaa"),XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "b.xr", "bbb"),XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "c.xr", "ccc"),XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(cache->file_count, 3);

    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_add_duplicate) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;

    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "content1"),XR_COMPILE_RESOURCE_OK);
    // Adding the same path succeeds without replacing its published content
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "content2"),XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(cache->file_count, 1);

    ASSERT_TRUE(cache_test_close(&owner));
}

/* ========== Line Retrieval ========== */

TEST(source_cache_get_line) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "first\nsecond\nthird"),XR_COMPILE_RESOURCE_OK);

    // Lines are 1-indexed
    const char *line1 = xr_runtime_source_cache_get_line(cache, "test.xr", 1);
    ASSERT_NOT_NULL(line1);
    // line1 points into the content, starts at "first\n..."
    ASSERT_TRUE(strncmp(line1, "first", 5) == 0);

    const char *line2 = xr_runtime_source_cache_get_line(cache, "test.xr", 2);
    ASSERT_NOT_NULL(line2);
    ASSERT_TRUE(strncmp(line2, "second", 6) == 0);

    const char *line3 = xr_runtime_source_cache_get_line(cache, "test.xr", 3);
    ASSERT_NOT_NULL(line3);
    ASSERT_TRUE(strncmp(line3, "third", 5) == 0);

    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_get_line_boundary) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "only"),XR_COMPILE_RESOURCE_OK);

    // Line 0 is out of range (1-indexed)
    ASSERT_NULL(xr_runtime_source_cache_get_line(cache, "test.xr", 0));
    // Line 2 is out of range for single-line content
    ASSERT_NULL(xr_runtime_source_cache_get_line(cache, "test.xr", 2));
    // Negative line
    ASSERT_NULL(xr_runtime_source_cache_get_line(cache, "test.xr", -1));

    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_get_line_nonexistent_file) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "exists.xr", "content"),XR_COMPILE_RESOURCE_OK);

    ASSERT_NULL(xr_runtime_source_cache_get_line(cache, "missing.xr", 1));

    ASSERT_TRUE(cache_test_close(&owner));
}

/* ========== Line Length ========== */

TEST(source_cache_line_length) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "hello\nworld!\nok"),XR_COMPILE_RESOURCE_OK);

    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 1), 5);  // "hello"
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 2), 6);  // "world!"
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 3), 2);  // "ok"

    ASSERT_TRUE(cache_test_close(&owner));
}

TEST(source_cache_line_length_boundary) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "x"),XR_COMPILE_RESOURCE_OK);

    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 0), 0);
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 99), 0);
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "missing.xr", 1), 0);

    ASSERT_TRUE(cache_test_close(&owner));
}

/* ========== Empty Content ========== */

TEST(source_cache_empty_lines) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_EQ_INT(xr_compile_source_cache_add(cache, "test.xr", "\n\n"),XR_COMPILE_RESOURCE_OK);

    // "\n\n" = 3 lines: "", "", ""
    const char *line1 = xr_runtime_source_cache_get_line(cache, "test.xr", 1);
    ASSERT_NOT_NULL(line1);
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, "test.xr", 1), 0);

    ASSERT_TRUE(cache_test_close(&owner));
}

/* ========== Capacity Growth ========== */

TEST(source_cache_grow) {
    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;

    // Add more files than initial capacity (4)
    char path[32];
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "file_%d.xr", i);
        ASSERT_EQ_INT(xr_compile_source_cache_add(cache, path, "content"),XR_COMPILE_RESOURCE_OK);
    }
    ASSERT_EQ_INT(cache->file_count, 10);

    // All files accessible
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "file_%d.xr", i);
        const char *line = xr_runtime_source_cache_get_line(cache, path, 1);
        ASSERT_NOT_NULL(line);
    }

    ASSERT_TRUE(cache_test_close(&owner));
}

/* ========== NULL Safety ========== */

TEST(source_cache_null_safety) {
    xr_owned_source_cache_close(NULL);  // should not crash

    CacheTestOwner owner={0};
    ASSERT_TRUE(cache_test_open(&owner));
    XrSourceCache *cache=owner.cache;
    ASSERT_NULL(xr_runtime_source_cache_get_line(NULL, "x", 1));
    ASSERT_NULL(xr_runtime_source_cache_get_line(cache, NULL, 1));
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(NULL, "x", 1), 0);
    ASSERT_EQ_INT(xr_runtime_source_cache_get_line_length(cache, NULL, 1), 0);

    ASSERT_TRUE(cache_test_close(&owner));
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
