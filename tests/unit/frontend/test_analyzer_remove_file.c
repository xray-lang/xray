/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_remove_file.c - regression tests for
 *                                  xa_analyzer_remove_file()
 *
 * KEY CONCEPT:
 *   Verifies that xa_analyzer_remove_file() leaves the analyzer in a
 *   self-consistent state:
 *     1. dependency-graph edges referencing removed symbols are gone
 *     2. file_count drops by exactly one
 *     3. dep_graph_free() reclaims BOTH forward AND reverse chains so
 *        ASan reports no leaks at xa_analyzer_free()
 *
 *   An earlier dep_graph_free() leaked one allocation per edge for the
 *   analyzer's lifetime. An earlier xa_analyzer_remove_file() left
 *   dangling dep edges pointing at freed symbols.
 */

#include "../test_framework.h"
#include <string.h>

#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_symbol.h"
#include "frontend/analyzer/xanalyzer_incremental.h"
#include "runtime/value/xtype_pool.h"
#include "xray_isolate.h"

/* ---------------------------------------------------------------------- */
/* Test fixtures                                                          */
/* ---------------------------------------------------------------------- */

static XrayIsolate *g_iso = NULL;

static void setup(void) {
    XrayIsolateParams p;
    xray_isolate_params_init(&p);
    g_iso = xray_isolate_new(&p);
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

// Build a fresh symbol with a fixed file path and add it to the analyzer's
// global scope. Returns the symbol (still owned by the scope).
static XaSymbol *add_symbol_in_file(XaAnalyzer *a, const char *name, const char *file) {
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_FUNCTION);
    XaSymbolLinks *links = xa_analyzer_get_links(a, sym);
    if (links) {
        links->file_path = file;  // we only need pointer equality / strcmp
    }
    xa_scope_add_symbol(a->global_scope, sym);
    return sym;
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

// dep_graph_free reclaims BOTH chains. The "no leak" assertion is
// implicit -- the test runs under ASan in CI, and ASan reports any leak
// of the reverse-chain nodes.
TEST(dep_graph_free_no_leak) {
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    // Create two symbols and a dependency edge between them. xa_dep_add()
    // mallocs ONE forward node + ONE reverse node per call.
    XaSymbol *s1 = add_symbol_in_file(a, "f1", "a.xr");
    XaSymbol *s2 = add_symbol_in_file(a, "f2", "a.xr");

    XaIncrementalCtx *incr = (XaIncrementalCtx *) a->incremental;
    ASSERT(incr != NULL);
    xa_dep_add(incr, s1->id, s2->id, XA_DEP_REFERENCE);
    xa_dep_add(incr, s2->id, s1->id, XA_DEP_REFERENCE);
    ASSERT_EQ_INT(incr->deps->edge_count, 2);

    // Free the analyzer. An earlier implementation leaked the two reverse-chain
    // allocations; under ASan, the test process exits non-zero on leak.
    xa_analyzer_free(a);
}

// Removing a file drops every edge that touches a symbol owned by that
// file, and the file_count invariant holds.
TEST(remove_file_drops_dep_edges) {
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    // Two files, two symbols each. Edges A1 -> B1 and B2 -> A2 cross the
    // file boundary so both removal directions are exercised.
    XaSymbol *a1 = add_symbol_in_file(a, "a1", "a.xr");
    XaSymbol *a2 = add_symbol_in_file(a, "a2", "a.xr");
    XaSymbol *b1 = add_symbol_in_file(a, "b1", "b.xr");
    XaSymbol *b2 = add_symbol_in_file(a, "b2", "b.xr");

    // Track file_count. xa_analyzer_invalidate_range() is the public
    // file-registration entry (creates a XaFileEntry on first call).
    xa_analyzer_invalidate_range(a, "a.xr", 1, 100);
    xa_analyzer_invalidate_range(a, "b.xr", 1, 100);
    int file_count_before = a->file_count;
    ASSERT(file_count_before >= 2);

    XaIncrementalCtx *incr = (XaIncrementalCtx *) a->incremental;
    ASSERT(incr != NULL);
    xa_dep_add(incr, a1->id, b1->id, XA_DEP_REFERENCE);
    xa_dep_add(incr, b2->id, a2->id, XA_DEP_CALL);
    xa_dep_add(incr, b1->id, b2->id, XA_DEP_REFERENCE);  // intra-b edge
    ASSERT_EQ_INT(incr->deps->edge_count, 3);

    // Remove file a.xr: it should drop the two cross-file edges (a1->b1
    // and b2->a2), leaving only the intra-b edge.
    xa_analyzer_remove_file(a, "a.xr");

    ASSERT_EQ_INT(incr->deps->edge_count, 1);
    ASSERT_EQ_INT(a->file_count, file_count_before - 1);

    // Removing b.xr next should drop the remaining edge.
    xa_analyzer_remove_file(a, "b.xr");
    ASSERT_EQ_INT(incr->deps->edge_count, 0);
    ASSERT_EQ_INT(a->file_count, file_count_before - 2);

    xa_analyzer_free(a);
}

// Removing a non-tracked file is a no-op (file_count unchanged,
// edge_count unchanged). The DCHECK invariants must still hold.
TEST(remove_file_unknown_path_is_noop) {
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT(a != NULL);

    int file_count_before = a->file_count;
    XaIncrementalCtx *incr = (XaIncrementalCtx *) a->incremental;
    int edge_count_before = (incr && incr->deps) ? incr->deps->edge_count : 0;

    xa_analyzer_remove_file(a, "/path/does/not/exist.xr");

    ASSERT_EQ_INT(a->file_count, file_count_before);
    ASSERT_EQ_INT(incr ? incr->deps->edge_count : 0, edge_count_before);

    xa_analyzer_free(a);
}

/* ---------------------------------------------------------------------- */
/* Main                                                                   */
/* ---------------------------------------------------------------------- */

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("xa_analyzer_remove_file");
RUN_TEST(dep_graph_free_no_leak);
RUN_TEST(remove_file_drops_dep_edges);
RUN_TEST(remove_file_unknown_path_is_noop);
teardown();
TEST_MAIN_END()
