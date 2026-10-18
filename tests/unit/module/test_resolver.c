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
#include "base/xcompile_resources.h"

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
    XrOsIoPolicy policy = xr_os_io_system_policy();
    XrLockfile *lockfile = NULL;
    ASSERT_EQ_INT(xr_lockfile_new_owned(&policy, &lockfile), XR_OS_IO_OK);
    ASSERT_EQ_INT(xr_lockfile_add_package_owned(lockfile, "xray/redis", "1.1.0", "", ""), XR_OS_IO_OK);

    XrResolveResult *result = xr_resolve_dependencies(graph, mock_get_info, lockfile, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_EQ_INT(result->count, 1);
    ASSERT_STR_EQ(result->versions[0], "1.1.0");  // Pinned by lockfile

    xr_resolve_result_free(result);
    xr_lockfile_free_owned(lockfile);
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

static XrPackageInfo *count_registry_queries(const char *name, void *context) {
    (*(int *) context)++;
    return mock_get_info(name, NULL);
}

TEST(resolve_lockfile_budget_does_not_query_registry) {
    XrCompileResourceLimits limits = {1048576, 1048576, 1048576};
    XrCompileResources *resources = NULL;
    ASSERT_EQ_INT(xr_compile_resources_new(&limits, &resources), XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}, used = {0};
    ASSERT_EQ_INT(xr_compile_resources_stats(resources, &initial), XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrLockfile *lockfile = NULL;
    ASSERT_EQ_INT(xr_lockfile_new_owned(&policy, &lockfile), XR_OS_IO_OK);
    ASSERT_EQ_INT(xr_lockfile_add_package_owned(lockfile, "xray/redis", "1.1.0", "", ""), XR_OS_IO_OK);
    XrDepGraph *graph = xr_depgraph_new();
    ASSERT_NOT_NULL(graph);
    ASSERT_TRUE(xr_depgraph_add_root(graph, "xray/redis", "^1.0.0"));
    ASSERT_EQ_INT(xr_compile_resources_stats(resources, &used), XR_COMPILE_RESOURCE_OK);
    ASSERT_TRUE(used.work <= limits.work);
    ASSERT_EQ_INT(xr_compile_resources_work(resources, limits.work - used.work), XR_COMPILE_RESOURCE_OK);
    int queries = 0;
    XrResolveResult *result = xr_resolve_dependencies(graph, count_registry_queries, lockfile, &queries);
    ASSERT_NOT_NULL(result);
    ASSERT_FALSE(result->success);
    ASSERT_EQ_INT(queries, 0);
    ASSERT_EQ_INT(lockfile->package_count, 1);
    ASSERT_STR_EQ(lockfile->packages[0].version, "1.1.0");
    xr_resolve_result_free(result);
    xr_depgraph_free(graph);
    xr_lockfile_free_owned(lockfile);
    ASSERT_EQ_INT(xr_compile_resources_stats(resources, &used), XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(used.live_bytes, initial.live_bytes);
    xr_compile_resources_release(resources);
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
RUN_TEST(resolve_lockfile_budget_does_not_query_registry);

TEST_MAIN_END()
