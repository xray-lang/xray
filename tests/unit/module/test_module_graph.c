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
#include "toolchain/xcompiler_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ========== Isolate Stub ========== */

/* The graph builder needs a compiler session for parsing. */
#include "xray.h"
#include "xray_vm.h"

/* ========== Test Fixtures ========== */

static char g_tmpdir[512];
static XrVMRuntime *g_iso;
static XrCompilerSession *g_session;

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
    XrModuleGraph *g = xr_module_graph_new(g_session, r);
    ASSERT_NOT_NULL(g);
    ASSERT_EQ_INT(g->spec_count, 0);
    ASSERT_EQ_INT(g->entry_index, -1);
    xr_module_graph_free(g);
    xr_module_resolver_free(r);
}

TEST(graph_single_file_no_imports) {
    setup();
    create_file("main.xr", "var x = 42\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

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
    create_file("a.xr", "import \"./b\"\nvar a = 1\n");
    create_file("b.xr", "import \"./c\"\nvar b = 2\n");
    create_file("c.xr", "var c = 3\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

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
    create_file("left.xr", "import \"./base\"\nvar l = 1\n");
    create_file("right.xr", "import \"./base\"\nvar r = 2\n");
    create_file("base.xr", "var b = 0\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

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
    create_file("self.xr", "import \"./self\"\nvar x = 1\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("self.xr"), &err);
    ASSERT_EQ_INT(rc, 0);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);
    ASSERT_NOT_NULL(g->cycle_desc);
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "E0504"));
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "self.xr -> self.xr"));

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_cycle_two) {
    setup();
    create_file("a.xr", "import \"./b\"\nvar a = 1\n");
    create_file("b.xr", "import \"./a\"\nvar b = 2\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("a.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(g->spec_count, 2);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);
    ASSERT_NOT_NULL(g->cycle_desc);
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "E0504"));
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "a.xr -> b.xr -> a.xr"));

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
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("a.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(g->spec_count, 3);

    rc = xr_module_graph_topological_sort(g);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_TRUE(g->has_cycle);
    ASSERT_NOT_NULL(g->cycle_desc);
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "E0504"));
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "a.xr"));
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "b.xr"));
    ASSERT_NOT_NULL(strstr(g->cycle_desc, "c.xr"));

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_find_by_canonical) {
    setup();
    create_file("main.xr", "import \"./helper\"\n");
    create_file("helper.xr", "var h = 1\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

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
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, "/tmp/nonexistent_xray_file_9999.xr", &err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_NOT_NULL(err);
    xr_free(err);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
}

TEST(graph_parse_failure_is_build_failure) {
    setup();
    create_file("invalid.xr", "var value =\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = xr_module_graph_build(g, abs_path("invalid.xr"), &err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_NOT_NULL(err);
    ASSERT_NOT_NULL(strstr(err, "failed to parse module"));
    ASSERT_NOT_NULL(strstr(err, "invalid.xr"));
    xr_free(err);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

/* Global isolate for all tests */
XrVMConfig vm_config = {0};
g_iso = xray_vm_new_full(&vm_config);
g_session = xr_compiler_session_current_for_isolate(g_iso);
/* Not ASSERT_NOT_NULL: its bail-out is a bare `return`, which cannot carry an
 * exit status out of main(). Without a session no test below can run, so report
 * the failed precondition and leave with the suite's failure status. */
if (!g_session) {
    printf("\033[31mFAIL\033[0m no compiler session for the test isolate\n");
    xray_vm_delete(g_iso);
    XR_TEST_PROCESS_SHUTDOWN();
    return 1;
}

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
RUN_TEST(graph_parse_failure_is_build_failure);

xray_vm_delete(g_iso);
g_iso = NULL;
g_session = NULL;

TEST_MAIN_END()
