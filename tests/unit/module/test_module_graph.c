/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_graph.c - Unit tests for module dependency graph
 */

#include "../test_framework.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "base/xhashmap.h"
#include "base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ========== Isolate Stub ========== */

/* The graph builder needs an isolate for parsing. */
#include "xray.h"
#include "xray_isolate.h"

/* ========== Test Fixtures ========== */

static char g_tmpdir[512];
static XrayIsolate *g_iso;

static void setup(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/xray_test_graph_XXXXXX");
    char *d = mkdtemp(g_tmpdir);
    if (!d)
        abort();
}

static void teardown(void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_tmpdir);
    (void) system(cmd);
}

static void create_file(const char *rel_path, const char *content) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", g_tmpdir, rel_path);
    char dir[1024];
    strncpy(dir, full, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char mkd[1100];
        snprintf(mkd, sizeof(mkd), "mkdir -p %s", dir);
        (void) system(mkd);
    }
    FILE *f = fopen(full, "w");
    if (!f)
        abort();
    if (content)
        fputs(content, f);
    fclose(f);
}

static char *abs_path(const char *rel) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", g_tmpdir, rel);
    return buf;
}

/* ========== Tests ========== */

TEST(graph_new_free) {
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);
    ASSERT_NOT_NULL(g);
    ASSERT_EQ_INT(g->spec_count, 0);
    ASSERT_EQ_INT(g->entry_index, -1);
    xr_module_graph_free(g);
    xr_module_resolver_free(r);
}

TEST(graph_single_file_no_imports) {
    setup();
    create_file("main.xr", "let x = 42\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("main.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(g->spec_count, 1);
    ASSERT_EQ_INT(g->entry_index, 0);
    ASSERT_NOT_NULL(g->specs[0].ast);
    ASSERT_EQ_INT(g->specs[0].status, XR_MODSPEC_RESOLVED);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_FALSE(g->has_cycle);
    ASSERT_EQ_INT(g->topo_count, 1);
    ASSERT_EQ_INT(g->topo_order[0], 0);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_linear_deps) {
    setup();
    create_file("a.xr", "import \"./b\"\nlet a = 1\n");
    create_file("b.xr", "import \"./c\"\nlet b = 2\n");
    create_file("c.xr", "let c = 3\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("a.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(g->spec_count, 3);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_FALSE(g->has_cycle);
    ASSERT_EQ_INT(g->topo_count, 3);

    /* c should come before b, b before a in topo order */
    int idx_a = xr_module_graph_find(g, g->specs[g->entry_index].canonical);
    ASSERT_TRUE(idx_a >= 0);
    ASSERT_TRUE(g->specs[idx_a].topo_index > 0);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_diamond_deps) {
    setup();
    create_file("main.xr", "import \"./left\"\nimport \"./right\"\n");
    create_file("left.xr", "import \"./base\"\nlet l = 1\n");
    create_file("right.xr", "import \"./base\"\nlet r = 2\n");
    create_file("base.xr", "let b = 0\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("main.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(g->spec_count, 4);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_FALSE(g->has_cycle);
    ASSERT_EQ_INT(g->topo_count, 4);

    /* base must come before left and right in topo order */
    int base_idx = -1;
    for (int i = 0; i < g->spec_count; i++) {
        if (strstr(g->specs[i].canonical, "base"))
            base_idx = i;
    }
    ASSERT_TRUE(base_idx >= 0);
    ASSERT_TRUE(g->specs[base_idx].topo_index < g->specs[g->entry_index].topo_index);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_cycle_self) {
    setup();
    create_file("self.xr", "import \"./self\"\nlet x = 1\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("self.xr"), &err);
    ASSERT_EQ_INT(rc, 0);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_cycle_two) {
    setup();
    create_file("a.xr", "import \"./b\"\nlet a = 1\n");
    create_file("b.xr", "import \"./a\"\nlet b = 2\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("a.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(g->spec_count, 2);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);
    ASSERT_NOT_NULL(g->cycle_desc);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_cycle_three) {
    setup();
    create_file("a.xr", "import \"./b\"\n");
    create_file("b.xr", "import \"./c\"\n");
    create_file("c.xr", "import \"./a\"\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("a.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(g->spec_count, 3);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_find_by_canonical) {
    setup();
    create_file("main.xr", "import \"./helper\"\n");
    create_file("helper.xr", "let h = 1\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    xr_module_graph_build(g, abs_path("main.xr"), &err);

    /* The entry's canonical is its absolute path */
    int idx = xr_module_graph_find(g, g->specs[0].canonical);
    ASSERT_EQ_INT(idx, 0);

    int missing = xr_module_graph_find(g, "/nonexistent/path.xr");
    ASSERT_EQ_INT(missing, -1);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_entry_not_found) {
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_iso, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, "/tmp/nonexistent_xray_file_9999.xr", &err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_NOT_NULL(err);
    xr_free(err);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

/* Global isolate for all tests */
g_iso = xray_isolate_new(NULL);

RUN_TEST_SUITE("Lifecycle");
RUN_TEST(graph_new_free);

RUN_TEST_SUITE("Build - Basic");
RUN_TEST(graph_single_file_no_imports);
RUN_TEST(graph_linear_deps);
RUN_TEST(graph_diamond_deps);

RUN_TEST_SUITE("Cycle Detection");
RUN_TEST(graph_cycle_self);
RUN_TEST(graph_cycle_two);
RUN_TEST(graph_cycle_three);

RUN_TEST_SUITE("Lookup");
RUN_TEST(graph_find_by_canonical);
RUN_TEST(graph_entry_not_found);

xray_isolate_delete(g_iso);
g_iso = NULL;

TEST_MAIN_END()
