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
#include "base/xfileio.h"
#include "base/xhashmap.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_temp.h"
#include "../test_win_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Test Fixtures ========== */

static char g_tmpdir[512];
static char g_created_files[32][1024];
static size_t g_created_file_count;
static XrCompilerSession *g_session;

static void setup(void) {
    g_created_file_count = 0u;
    if (xr_temp_dir_create("xray-test-graph", g_tmpdir, sizeof(g_tmpdir)) != 0)
        abort();
    /* Same canonical-root requirement as the resolver suite: the graph
     * canonicalizes the entry it is handed, and the identity authority
     * compares that against this root byte for byte. */
    char *canonical = xr_realpath(g_tmpdir, NULL);
    if (!canonical)
        abort();
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", canonical);
    xr_free(canonical);
}

static void teardown(void) {
    while (g_created_file_count > 0u)
        if (remove(g_created_files[--g_created_file_count]) != 0)
            abort();
    if (xr_test_rmdir(g_tmpdir) != 0)
        abort();
    g_tmpdir[0] = '\0';
}

static void create_file(const char *rel_path, const char *content) {
    char full[1024];
    /* This fixture owns flat files below its atomically reserved directory. */
    int length = snprintf(full, sizeof(full), "%s/%s", g_tmpdir, rel_path);
    if (!g_tmpdir[0] || strchr(rel_path, '/') || strchr(rel_path, '\\') || length < 0 ||
        (size_t) length >= sizeof(full))
        abort();
    FILE *f = fopen(full, "w");
    if (!f)
        abort();
    if (content)
        fputs(content, f);
    if (fclose(f) != 0)
        abort();
    for (size_t i = 0u; i < g_created_file_count; ++i)
        if (strcmp(g_created_files[i], full) == 0)
            return;
    if (g_created_file_count >= sizeof(g_created_files) / sizeof(g_created_files[0]))
        abort();
    memcpy(g_created_files[g_created_file_count++], full, (size_t) length + 1u);
}

static char *abs_path(const char *rel) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/%s", g_tmpdir, rel);
    return buf;
}

static int build_script_graph(XrModuleGraph *graph, const char *entry, char **error) {
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT,
        .physical_root = g_tmpdir,
    };
    return xr_module_graph_build(graph, entry, &authority, error);
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

TEST(graph_logical_source_checks_supplied_bytes_under_exact_file_authority) {
    setup();
    create_file("main.xr", "this disk content must not be parsed\n");
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    XrModuleGraph *graph = xr_module_graph_new(g_session, resolver);
    ASSERT_NOT_NULL(graph);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_SCRIPT, .physical_root = g_tmpdir,
    };
    const char *supplied = "var supplied = 42\n";
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_graph_build_logical_source(graph, &authority, "main.xr",
                                                       abs_path("main.xr"), supplied, &error), 0);
    ASSERT_NULL(error);
    ASSERT_EQ_INT(graph->spec_count, 1);
    ASSERT_NOT_NULL(graph->specs[graph->entry_index].ast);
    XrFingerprint expected;
    xr_module_source_fingerprint(supplied, &expected);
    ASSERT_EQ_INT(memcmp(&graph->specs[graph->entry_index].source_content_fingerprint,
                          &expected, sizeof(expected)), 0);
    xr_module_graph_free(graph);
    graph = xr_module_graph_new(g_session, resolver);
    ASSERT_EQ_INT(xr_module_graph_build_logical_source(graph, &authority, "other.xr",
                                                       abs_path("main.xr"), supplied, &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_EQ_INT(graph->spec_count, 0);
    xr_free(error);
    xr_module_graph_free(graph);
    xr_module_resolver_free(resolver);
    teardown();
}

TEST(graph_memory_source_rejects_file_or_logical_authority) {
    XrModuleResolverConfig config = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config);
    XrModuleGraph *graph = xr_module_graph_new(g_session, resolver);
    ASSERT_NOT_NULL(graph);
    XrModuleIdentityAuthority authority = {
        .kind = XR_MODULE_IDENTITY_MEMORY, .namespace_id = "logical-source-memory",
    };
    char *error = NULL;
    ASSERT_EQ_INT(xr_module_graph_build_logical_source(graph, &authority, "wrong.xr", NULL,
                                                       "var value = 42", &error), -1);
    ASSERT_NOT_NULL(error);
    ASSERT_EQ_INT(graph->spec_count, 0);
    xr_free(error);
    xr_module_graph_free(graph);
    xr_module_resolver_free(resolver);
}

TEST(graph_single_file_no_imports) {
    setup();
    create_file("main.xr", "var x = 42\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = build_script_graph(g, abs_path("main.xr"), &err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(g->spec_count, 1);
    ASSERT_EQ_INT(g->entry_index, 0);
    ASSERT_NOT_NULL(g->specs[0].ast);
    ASSERT_EQ_INT(g->specs[0].status, XR_MODSPEC_RESOLVED);
    char *raw_source = xr_file_read_all(abs_path("main.xr"), "rb", NULL);
    ASSERT_NOT_NULL(raw_source);
    XrFingerprint expected_fingerprint;
    xr_module_source_fingerprint(raw_source, &expected_fingerprint);
    xr_free(raw_source);
    ASSERT_EQ_INT(memcmp(&g->specs[0].source_content_fingerprint, &expected_fingerprint,
        sizeof(expected_fingerprint)), 0);

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
    int rc = build_script_graph(g, abs_path("a.xr"), &err);
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
    create_file("left.xr", "import \"./base\"\nvar l = 1\n");
    create_file("right.xr", "import \"./base\"\nvar r = 2\n");
    create_file("base.xr", "var b = 0\n");

    const char *imports[] = {"import \"./left\"\nimport \"./right\"\n",
                             "import \"./right\"\nimport \"./left\"\n"};
    const char *expected[][4] = {{"base.xr", "left.xr", "right.xr", "main.xr"},
                                 {"base.xr", "right.xr", "left.xr", "main.xr"}};
    for (int order = 0; order < 2; order++) {
        create_file("main.xr", imports[order]);
        XrModuleResolverConfig cfg = {0};
        XrModuleResolver *r = xr_module_resolver_new(&cfg);
        XrModuleGraph *g = xr_module_graph_new(g_session, r);
        char *err = NULL;
        ASSERT_EQ_INT(build_script_graph(g, abs_path("main.xr"), &err), 0);
        ASSERT_NULL(err);
        ASSERT_EQ_INT(g->spec_count, 4);
        ASSERT_EQ_INT(xr_module_graph_topological_sort(g), 0);
        ASSERT_FALSE(g->has_cycle);
        ASSERT_EQ_INT(g->topo_count, 4);
        /* The independent order also proves the diamond's shared dependency
         * is visited exactly once, before either importer. */
        for (int i = 0; i < 4; i++) {
            const XrModuleSpec *spec = &g->specs[g->topo_order[i]];
            ASSERT_STR_EQ(spec->logical_path, expected[order][i]);
            ASSERT_EQ_INT(spec->topo_index, i);
        }
        xr_module_graph_free(g);
        xr_module_resolver_free(r);
    }
    teardown();
}

TEST(graph_cycle_self) {
    setup();
    create_file("self.xr", "import \"./self\"\nvar x = 1\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = build_script_graph(g, abs_path("self.xr"), &err);
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
    int rc = build_script_graph(g, abs_path("a.xr"), &err);
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
    int rc = build_script_graph(g, abs_path("a.xr"), &err);
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
    build_script_graph(g, abs_path("main.xr"), &err);

    /* The entry is indexed by its exact typed relocatable identity. */
    int idx = xr_module_graph_find(g, g->specs[0].canonical);
    ASSERT_EQ_INT(idx, 0);

    int missing = xr_module_graph_find(g, "/nonexistent/path.xr");
    ASSERT_EQ_INT(missing, -1);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_entry_not_found) {
    setup();
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = build_script_graph(g, abs_path("nonexistent.xr"), &err);
    ASSERT_EQ_INT(rc, XR_MODULE_NOT_FOUND);
    ASSERT_NOT_NULL(err);
    xr_free(err);

    xr_module_graph_free(g);
    xr_module_resolver_free(r);
    teardown();
}

TEST(graph_parse_failure_is_build_failure) {
    setup();
    create_file("invalid.xr", "var value =\n");

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);

    char *err = NULL;
    int rc = build_script_graph(g, abs_path("invalid.xr"), &err);
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

TEST(graph_additional_root_preserves_entry) {
    setup();
    create_file("main.xr", "var value = 1\n");
    create_file("extra.xr", "import \"./leaf\"\nvar extra = 2\n");
    create_file("leaf.xr", "var leaf = 3\n");
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, g_tmpdir};
    char *err = NULL;
    ASSERT_EQ_INT(build_script_graph(g, abs_path("main.xr"), &err), 0);
    struct AstNode *entry_ast = g->specs[g->entry_index].ast;
    ASSERT_EQ_INT(xr_module_graph_topological_sort(g), 0);
    ASSERT_EQ_INT(xr_module_graph_include(g, abs_path("extra.xr"), &authority, &err), 0);
    ASSERT_NULL(err);
    ASSERT_EQ_INT(g->entry_index, 0);
    ASSERT_EQ_INT(g->spec_count, 3);
    ASSERT_TRUE(g->specs[0].ast == entry_ast);
    ASSERT_EQ_INT(g->specs[0].dep_count, 0);
    ASSERT_EQ_INT(g->specs[1].dep_count, 1);
    ASSERT_EQ_INT(g->specs[1].dep_indices[0], 2);
    ASSERT_NULL(g->topo_order);
    ASSERT_EQ_INT(g->topo_count, 0);
    ASSERT_EQ_INT(g->specs[0].topo_index, -1);
    ASSERT_EQ_INT(xr_module_graph_topological_sort(g), 0);
    ASSERT_EQ_INT(g->topo_count, 3);
    ASSERT_TRUE(g->specs[2].topo_index < g->specs[1].topo_index);
    int *order = g->topo_order;
    struct AstNode *extra_ast = g->specs[1].ast;
    ASSERT_EQ_INT(xr_module_graph_include(g, abs_path("extra.xr"), &authority, &err), 0);
    ASSERT_EQ_INT(xr_module_graph_include(g, abs_path("leaf.xr"), &authority, &err), 0);
    ASSERT_EQ_INT(xr_module_graph_include(g, abs_path("main.xr"), &authority, &err), 0);
    ASSERT_EQ_INT(g->spec_count, 3);
    ASSERT_TRUE(g->topo_order == order && g->specs[1].ast == extra_ast);
    ASSERT_EQ_INT(build_script_graph(g, abs_path("extra.xr"), &err), -1);
    ASSERT_NOT_NULL(err);
    ASSERT_EQ_INT(g->entry_index, 0);
    xr_free(err);
    xr_module_graph_free(g); xr_module_resolver_free(r); teardown();
}

TEST(graph_additional_root_failures) {
    setup();
    create_file("main.xr", "var value = 1\n");
    create_file("invalid.xr", "var value =\n");
    create_file("cycle.xr", "import \"./cycle\"\n");
    create_file("unresolved.xr", "import \"./absent\"\n");
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, g_tmpdir};
    const char *names[] = {"invalid.xr", "cycle.xr", "unresolved.xr", "absent.xr"};
    for (int i = 0; i < 4; ++i) {
        XrModuleGraph *g = xr_module_graph_new(g_session, r);
        char *err = NULL;
        ASSERT_EQ_INT(xr_module_graph_include(g, abs_path("main.xr"), &authority, &err), -1);
        ASSERT_NOT_NULL(err); xr_free(err); err = NULL;
        ASSERT_EQ_INT(g->spec_count, 0);
        ASSERT_EQ_INT(build_script_graph(g, abs_path("main.xr"), &err), 0);
        ASSERT_EQ_INT(xr_module_graph_topological_sort(g), 0);
        int result = xr_module_graph_include(g, abs_path(names[i]), &authority, &err);
        ASSERT_EQ_INT(result, i == 1 ? XR_MODULE_OK : i >= 2 ? XR_MODULE_NOT_FOUND : XR_MODULE_INVALID);
        ASSERT_EQ_INT(g->entry_index, 0);
        ASSERT_EQ_INT(g->specs[0].dep_count, 0);
        if (i == 1) {
            ASSERT_NULL(err);
            ASSERT_NULL(g->topo_order);
            ASSERT_EQ_INT(xr_module_graph_topological_sort(g), -1);
            ASSERT_TRUE(g->has_cycle);
        } else {
            ASSERT_NOT_NULL(err);
        }
        xr_free(err); xr_module_graph_free(g);
    }
    xr_module_resolver_free(r); teardown();
}

TEST(graph_additional_root_conflicting_authority) {
    setup();
    create_file("main.xr", "var value = 1\n");
    char other_directory[512], other_path[1024];
    ASSERT_EQ_INT(xr_temp_dir_create("xray-test-graph-other", other_directory, sizeof(other_directory)), 0);
    char *other_root = xr_realpath(other_directory, NULL);
    ASSERT_NOT_NULL(other_root);
    int length = snprintf(other_path, sizeof(other_path), "%s/main.xr", other_root);
    ASSERT_TRUE(length > 0 && (size_t)length < sizeof(other_path));
    FILE *file = fopen(other_path, "w");
    ASSERT_NOT_NULL(file);
    ASSERT_TRUE(fputs("var value = 2\n", file) >= 0);
    ASSERT_EQ_INT(fclose(file), 0);
    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *r = xr_module_resolver_new(&cfg);
    XrModuleGraph *g = xr_module_graph_new(g_session, r);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PROJECT, "graph-project", g_tmpdir};
    char *err = NULL;
    ASSERT_EQ_INT(xr_module_graph_build(g, abs_path("main.xr"), &authority, &err), 0);
    ASSERT_EQ_INT(xr_module_graph_topological_sort(g), 0);
    struct AstNode *ast = g->specs[0].ast;
    int *order = g->topo_order;
    authority.physical_root = other_root;
    ASSERT_EQ_INT(xr_module_graph_include(g, other_path, &authority, &err), -1);
    ASSERT_NOT_NULL(err);
    ASSERT_NOT_NULL(strstr(err, "conflicting source authority"));
    ASSERT_EQ_INT(g->entry_index, 0);
    ASSERT_EQ_INT(g->spec_count, 1);
    ASSERT_TRUE(g->specs[0].ast == ast && g->topo_order == order);
    xr_free(err); xr_module_graph_free(g); xr_module_resolver_free(r);
    ASSERT_EQ_INT(remove(other_path), 0);
    ASSERT_EQ_INT(xr_test_rmdir(other_root), 0);
    xr_free(other_root); teardown();
}

TEST_MAIN_BEGIN()

/* Parsing owns a compiler session independently of runtime initialization. */
g_session = xr_compiler_session_new(NULL);
/* Not ASSERT_NOT_NULL: its bail-out is a bare `return`, which cannot carry an
 * exit status out of main(). Without a session no test below can run, so report
 * the failed precondition and leave with the suite's failure status. */
if (!g_session) {
    printf("\033[31mFAIL\033[0m no compiler session for the graph tests\n");
    XR_TEST_PROCESS_SHUTDOWN();
    return 1;
}

RUN_TEST_SUITE("Lifecycle");
RUN_TEST(graph_new_free);

RUN_TEST_SUITE("Build - Basic");
RUN_TEST(graph_logical_source_checks_supplied_bytes_under_exact_file_authority);
RUN_TEST(graph_memory_source_rejects_file_or_logical_authority);
RUN_TEST(graph_single_file_no_imports);
RUN_TEST(graph_linear_deps);
RUN_TEST(graph_diamond_deps);
RUN_TEST(graph_additional_root_preserves_entry);
RUN_TEST(graph_additional_root_failures);
RUN_TEST(graph_additional_root_conflicting_authority);

RUN_TEST_SUITE("Cycle Detection");
RUN_TEST(graph_cycle_self);
RUN_TEST(graph_cycle_two);
RUN_TEST(graph_cycle_three);

RUN_TEST_SUITE("Lookup");
RUN_TEST(graph_find_by_canonical);
RUN_TEST(graph_entry_not_found);
RUN_TEST(graph_parse_failure_is_build_failure);

xr_compiler_session_delete(g_session);
g_session = NULL;

TEST_MAIN_END()
