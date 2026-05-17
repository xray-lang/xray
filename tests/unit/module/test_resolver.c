/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_resolver.c - Unit tests for dependency resolver module
 */

#include "../test_framework.h"
#include "module/xresolver.h"
#include "base/xmalloc.h"

/* ========== Dependency Graph Tests ========== */

TEST(depgraph_new_and_free) {
    XrDepGraph *graph = xr_depgraph_new();
    ASSERT_NOT_NULL(graph);
    ASSERT_EQ_INT(graph->node_count, 0);
    ASSERT_EQ_INT(graph->root_count, 0);
    xr_depgraph_free(graph);
}

TEST(depgraph_add_root) {
    XrDepGraph *graph = xr_depgraph_new();

    ASSERT_TRUE(xr_depgraph_add_root(graph, "xray/redis", "^1.0.0"));
    ASSERT_EQ_INT(graph->node_count, 1);
    ASSERT_EQ_INT(graph->root_count, 1);

    ASSERT_TRUE(xr_depgraph_add_root(graph, "xray/http", "~2.0.0"));
    ASSERT_EQ_INT(graph->node_count, 2);
    ASSERT_EQ_INT(graph->root_count, 2);

    xr_depgraph_free(graph);
}

TEST(depgraph_find) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");

    XrDepNode *node = xr_depgraph_find(graph, "xray/redis");
    ASSERT_NOT_NULL(node);
    ASSERT_STR_EQ(node->name, "xray/redis");

    XrDepNode *missing = xr_depgraph_find(graph, "xray/nonexist");
    ASSERT_NULL(missing);

    xr_depgraph_free(graph);
}

TEST(depgraph_find_null_args) {
    ASSERT_NULL(xr_depgraph_find(NULL, "test"));

    XrDepGraph *graph = xr_depgraph_new();
    ASSERT_NULL(xr_depgraph_find(graph, NULL));
    xr_depgraph_free(graph);
}

/* ========== Cycle Detection Tests ========== */

TEST(depgraph_no_cycle) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "A", "^1.0.0");
    xr_depgraph_add_root(graph, "B", "^1.0.0");

    char *cycle = NULL;
    ASSERT_FALSE(xr_depgraph_has_cycle(graph, &cycle));
    ASSERT_NULL(cycle);

    xr_depgraph_free(graph);
}

TEST(depgraph_many_roots) {
    XrDepGraph *graph = xr_depgraph_new();
    char name[32];

    for (int i = 0; i < 50; i++) {
        snprintf(name, sizeof(name), "pkg/%d", i);
        ASSERT_TRUE(xr_depgraph_add_root(graph, name, "^1.0.0"));
    }

    ASSERT_EQ_INT(graph->root_count, 50);
    ASSERT_EQ_INT(graph->node_count, 50);

    // No cycles in a flat graph
    ASSERT_FALSE(xr_depgraph_has_cycle(graph, NULL));

    xr_depgraph_free(graph);
}

/* ========== Topological Sort Tests ========== */

TEST(depgraph_topo_sort_empty) {
    XrDepGraph *graph = xr_depgraph_new();

    XrDepNode **order = NULL;
    int count = 0;
    ASSERT_TRUE(xr_depgraph_topo_sort(graph, &order, &count));
    ASSERT_EQ_INT(count, 0);
    xr_free(order);

    xr_depgraph_free(graph);
}

TEST(depgraph_topo_sort_single) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");

    XrDepNode **order = NULL;
    int count = 0;
    ASSERT_TRUE(xr_depgraph_topo_sort(graph, &order, &count));
    ASSERT_EQ_INT(count, 1);
    ASSERT_STR_EQ(order[0]->name, "xray/redis");
    xr_free(order);

    xr_depgraph_free(graph);
}

TEST(depgraph_topo_sort_null_args) {
    ASSERT_FALSE(xr_depgraph_topo_sort(NULL, NULL, NULL));

    XrDepGraph *graph = xr_depgraph_new();
    ASSERT_FALSE(xr_depgraph_topo_sort(graph, NULL, NULL));
    xr_depgraph_free(graph);
}

/* ========== Resolution Tests ========== */

// Mock package info callback for testing
static XrPackageInfo *mock_get_info(const char *name, void *user_data) {
    (void) user_data;

    XrPackageInfo *info = (XrPackageInfo *) xr_calloc(1, sizeof(XrPackageInfo));
    if (!info)
        return NULL;

    info->name = xr_strdup(name);

    // Provide some versions
    info->version_count = 3;
    info->versions = (char **) xr_malloc(3 * sizeof(char *));
    info->versions[0] = xr_strdup("1.0.0");
    info->versions[1] = xr_strdup("1.1.0");
    info->versions[2] = xr_strdup("1.2.0");

    info->dep_count = 0;
    info->deps = NULL;

    return info;
}

TEST(resolve_simple) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");

    XrResolveResult *result = xr_resolve_dependencies(graph, mock_get_info, NULL, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_EQ_INT(result->count, 1);
    ASSERT_STR_EQ(result->packages[0], "xray/redis");
    ASSERT_STR_EQ(result->versions[0], "1.2.0");  // Best match for ^1.0.0

    xr_resolve_result_free(result);
    xr_depgraph_free(graph);
}

TEST(resolve_multiple_roots) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");
    xr_depgraph_add_root(graph, "xray/http", "~1.1.0");

    XrResolveResult *result = xr_resolve_dependencies(graph, mock_get_info, NULL, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_EQ_INT(result->count, 2);

    xr_resolve_result_free(result);
    xr_depgraph_free(graph);
}

// Mock that returns no versions
static XrPackageInfo *mock_get_info_empty(const char *name, void *user_data) {
    (void) user_data;

    XrPackageInfo *info = (XrPackageInfo *) xr_calloc(1, sizeof(XrPackageInfo));
    if (!info)
        return NULL;

    info->name = xr_strdup(name);
    info->version_count = 0;
    info->versions = NULL;
    info->dep_count = 0;
    info->deps = NULL;

    return info;
}

TEST(resolve_no_matching_version) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");

    XrResolveResult *result = xr_resolve_dependencies(graph, mock_get_info_empty, NULL, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_FALSE(result->success);
    ASSERT_NOT_NULL(result->error);

    xr_resolve_result_free(result);
    xr_depgraph_free(graph);
}

TEST(resolve_with_lockfile) {
    XrDepGraph *graph = xr_depgraph_new();
    xr_depgraph_add_root(graph, "xray/redis", "^1.0.0");

    // Create lockfile with pinned version
    XrLockfile *lockfile = xr_lockfile_new();
    xr_lockfile_add_package(lockfile, "xray/redis", "1.1.0", "", "");

    XrResolveResult *result = xr_resolve_dependencies(graph, mock_get_info, lockfile, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_EQ_INT(result->count, 1);
    ASSERT_STR_EQ(result->versions[0], "1.1.0");  // Pinned by lockfile

    xr_resolve_result_free(result);
    xr_lockfile_free(lockfile);
    xr_depgraph_free(graph);
}

TEST(resolve_result_free_null) {
    // Should not crash
    xr_resolve_result_free(NULL);
}

/* ========== Package Info Free Tests ========== */

TEST(package_info_free_null) {
    // Should not crash
    xr_package_info_free(NULL);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Dependency Graph");
RUN_TEST(depgraph_new_and_free);
RUN_TEST(depgraph_add_root);
RUN_TEST(depgraph_find);
RUN_TEST(depgraph_find_null_args);

RUN_TEST_SUITE("Cycle Detection");
RUN_TEST(depgraph_no_cycle);
RUN_TEST(depgraph_many_roots);

RUN_TEST_SUITE("Topological Sort");
RUN_TEST(depgraph_topo_sort_empty);
RUN_TEST(depgraph_topo_sort_single);
RUN_TEST(depgraph_topo_sort_null_args);

RUN_TEST_SUITE("Dependency Resolution");
RUN_TEST(resolve_simple);
RUN_TEST(resolve_multiple_roots);
RUN_TEST(resolve_no_matching_version);
RUN_TEST(resolve_with_lockfile);
RUN_TEST(resolve_result_free_null);
RUN_TEST(package_info_free_null);

TEST_MAIN_END()
