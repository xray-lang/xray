/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ast_side_table.c - Acceptance tests for the AST -> XrType side table
 *
 * KEY CONCEPT:
 *   Verifies the AST -> XrType side table that replaced the inline
 *   `AstNode::compile_type` field. Two API surfaces are exercised:
 *
 *     1. The raw `xa_node_table_*` map from xa_node_table.{c,h}.
 *        The map is pointer-keyed and never dereferences keys, so the
 *        tests use small heap-allocated stub structs as keys and
 *        stack-allocated XrType stubs as values. This isolates the
 *        map's contract from the rest of the analyzer.
 *
 *     2. The analyzer-level wrappers `xa_analyzer_set_node_type` /
 *        `xa_analyzer_get_node_type`. These are the canonical entry
 *        points for codegen / LSP / tests, and must be NULL-safe in
 *        every direction. We also verify two analyzers do not share
 *        their tables (per-analyzer ownership invariant).
 *
 *   This file does NOT verify that Pass 2 of the analyzer populates
 *   the table for real source -- that contract is exercised every
 *   time the regression suite runs (every `xr` snippet that compiles
 *   relies on the side table for codegen lookup), so a unit-level
 *   test would only duplicate signal.
 */

#include "../test_framework.h"

#include "frontend/analyzer/xa_node_table.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/parser/xast_nodes.h"
#include "runtime/value/xtype.h"
#include "xray_isolate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Direct map-API tests (no analyzer needed)                              */
/* ====================================================================== */

/* Helper: create a zeroed AstNode with a specific node_id. The table
 * keys on node_id, so each test node must have a unique ID. */
static AstNode make_node(uint32_t id) {
    AstNode n;
    memset(&n, 0, sizeof(n));
    n.node_id = id;
    return n;
}

// Same for XrType: the table only stores the pointer. We do still want
// the cast through `struct XrType *` to compile, so we declare
// stand-alone storage of the right type and pass its address.
//
// (xtype.h provides `struct XrType` definitions, so we can stack-
// allocate a value here even without a constructor.)

TEST(node_table_set_then_get) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(xa_node_table_size(t), 0);

    AstNode n = make_node(1);
    XrType ty;
    ty.kind = XR_KIND_INT;

    xa_node_table_set_type(t, &n, (struct XrType *) &ty);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);

    XrType *got = xa_node_table_get_type(t, &n);
    ASSERT_EQ_PTR(got, &ty);

    xa_node_table_free(t);
}

TEST(node_table_get_returns_null_for_unknown) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode known = make_node(10);
    AstNode unknown = make_node(11);
    XrType ty;
    ty.kind = XR_KIND_INT;

    xa_node_table_set_type(t, &known, (struct XrType *) &ty);

    // `unknown` was never inserted: get must return NULL, not the type
    // of `known` (i.e. no false positive from hash collision).
    ASSERT_NULL(xa_node_table_get_type(t, &unknown));

    xa_node_table_free(t);
}

TEST(node_table_set_null_clears_entry) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode n = make_node(20);
    XrType ty;
    ty.kind = XR_KIND_FLOAT;

    xa_node_table_set_type(t, &n, (struct XrType *) &ty);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);

    // NULL value clears the entry; size must drop, get must return NULL.
    xa_node_table_set_type(t, &n, NULL);
    ASSERT_EQ_INT(xa_node_table_size(t), 0);
    ASSERT_NULL(xa_node_table_get_type(t, &n));

    // Clearing a non-existent entry is a no-op (not an error).
    xa_node_table_set_type(t, &n, NULL);
    ASSERT_EQ_INT(xa_node_table_size(t), 0);

    xa_node_table_free(t);
}

TEST(node_table_set_overwrites_existing) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode n = make_node(30);
    XrType ty1, ty2;
    ty1.kind = XR_KIND_INT;
    ty2.kind = XR_KIND_STRING;

    xa_node_table_set_type(t, &n, (struct XrType *) &ty1);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);
    ASSERT_EQ_PTR(xa_node_table_get_type(t, &n), &ty1);

    // Overwrite same key with a different type: size unchanged, get
    // returns the new value.
    xa_node_table_set_type(t, &n, (struct XrType *) &ty2);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);
    ASSERT_EQ_PTR(xa_node_table_get_type(t, &n), &ty2);

    xa_node_table_free(t);
}

TEST(node_table_clear_drops_all_entries) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    XrType ty;
    ty.kind = XR_KIND_BOOL;

    // Insert a handful of distinct keys.
    enum {
        N = 16
    };
    AstNode nodes[N];
    for (int i = 0; i < N; i++) {
        nodes[i] = make_node((uint32_t) (100 + i));
        xa_node_table_set_type(t, &nodes[i], (struct XrType *) &ty);
    }
    ASSERT_EQ_INT(xa_node_table_size(t), N);

    xa_node_table_clear(t);
    ASSERT_EQ_INT(xa_node_table_size(t), 0);

    // After clear, every lookup must miss.
    for (int i = 0; i < N; i++) {
        ASSERT_NULL(xa_node_table_get_type(t, &nodes[i]));
    }

    // Table is reusable after clear.
    xa_node_table_set_type(t, &nodes[0], (struct XrType *) &ty);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);

    xa_node_table_free(t);
}

TEST(node_table_growth_preserves_entries) {
    // Default capacity is 64 buckets with a 0.75 load factor, so 49+
    // entries triggers at least one grow(). We insert 256 to force
    // multiple grows and assert every key still resolves to its
    // unique value.
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    enum {
        N = 256
    };
    AstNode *nodes = (AstNode *) malloc(sizeof(AstNode) * N); /* xr:allow-raw-alloc */
    XrType *types = (XrType *) malloc(sizeof(XrType) * N);    /* xr:allow-raw-alloc */
    ASSERT_NOT_NULL(nodes);
    ASSERT_NOT_NULL(types);

    for (int i = 0; i < N; i++) {
        nodes[i] = make_node((uint32_t) (1000 + i));
        types[i].kind = XR_KIND_INT;
        xa_node_table_set_type(t, &nodes[i], (struct XrType *) &types[i]);
    }
    ASSERT_EQ_INT(xa_node_table_size(t), N);

    // Every key must still resolve to its OWN value, not a neighbour.
    for (int i = 0; i < N; i++) {
        XrType *got = xa_node_table_get_type(t, &nodes[i]);
        ASSERT_EQ_PTR(got, &types[i]);
    }

    xa_node_table_free(t);
    free(nodes); /* xr:allow-raw-alloc */
    free(types); /* xr:allow-raw-alloc */
}

TEST(node_table_null_safe_api) {
    // All entry points are documented as NULL-safe for both arguments.
    // None of these calls may crash, and reads must return NULL.
    xa_node_table_set_type(NULL, NULL, NULL);
    xa_node_table_clear(NULL);
    xa_node_table_free(NULL);
    ASSERT_EQ_INT(xa_node_table_size(NULL), 0);
    ASSERT_NULL(xa_node_table_get_type(NULL, NULL));

    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);
    XrType ty;
    ty.kind = XR_KIND_INT;

    // NULL node on set / get -- no-op / NULL respectively.
    xa_node_table_set_type(t, NULL, (struct XrType *) &ty);
    ASSERT_EQ_INT(xa_node_table_size(t), 0);
    ASSERT_NULL(xa_node_table_get_type(t, NULL));

    xa_node_table_free(t);
}

TEST(node_table_scope_symbol_bindings) {
    // Verify the new scope/symbol binding API.
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode n = make_node(500);
    XrType ty;
    ty.kind = XR_KIND_INT;

    // Initially no bindings.
    ASSERT_NULL(xa_node_table_get_scope(t, &n));
    ASSERT_NULL(xa_node_table_get_symbol(t, &n));

    // Set full binding facts (scope/symbol as opaque pointers).
    int fake_scope = 42;
    int fake_symbol = 99;
    xa_node_table_set(t, &n, &ty, (struct XaScope *) &fake_scope, (struct XaSymbol *) &fake_symbol);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);
    ASSERT_EQ_PTR(xa_node_table_get_type(t, &n), &ty);
    ASSERT_EQ_PTR(xa_node_table_get_scope(t, &n), &fake_scope);
    ASSERT_EQ_PTR(xa_node_table_get_symbol(t, &n), &fake_symbol);

    xa_node_table_free(t);
}

/* ====================================================================== */
/* Analyzer-API wrapper tests                                              */
/* ====================================================================== */

static XrayIsolate *g_iso = NULL;

static void setup_isolate(void) {
    XrayIsolateParams p;
    xray_isolate_params_init(&p);
    g_iso = xray_isolate_new(&p);
}

static void teardown_isolate(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

TEST(analyzer_node_type_null_safe) {
    // No isolate / no analyzer: the wrappers must absorb NULL on both
    // sides without crashing or segfaulting.
    xa_analyzer_set_node_type(NULL, NULL, NULL);
    ASSERT_NULL(xa_analyzer_get_node_type(NULL, NULL));

    setup_isolate();
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT_NOT_NULL(a);

    // Live analyzer, NULL node: still a no-op / NULL.
    xa_analyzer_set_node_type(a, NULL, NULL);
    ASSERT_NULL(xa_analyzer_get_node_type(a, NULL));

    xa_analyzer_free(a);
    teardown_isolate();
}

TEST(analyzer_set_then_get_round_trip) {
    setup_isolate();
    XaAnalyzer *a = xa_analyzer_new(g_iso);
    ASSERT_NOT_NULL(a);

    AstNode n = make_node(200);
    XrType ty;
    ty.kind = XR_KIND_FLOAT;

    // Initially unknown.
    ASSERT_NULL(xa_analyzer_get_node_type(a, &n));

    xa_analyzer_set_node_type(a, &n, (struct XrType *) &ty);
    XrType *got = xa_analyzer_get_node_type(a, &n);
    ASSERT_EQ_PTR(got, &ty);

    // Clearing via NULL works through the wrapper too.
    xa_analyzer_set_node_type(a, &n, NULL);
    ASSERT_NULL(xa_analyzer_get_node_type(a, &n));

    xa_analyzer_free(a);
    teardown_isolate();
}

TEST(analyzer_tables_are_independent_per_analyzer) {
    // Two analyzers, one isolate, one shared AST stub. The side table
    // must be per-analyzer -- writing through `a1` must NOT be visible
    // through `a2`.
    setup_isolate();
    XaAnalyzer *a1 = xa_analyzer_new(g_iso);
    XaAnalyzer *a2 = xa_analyzer_new(g_iso);
    ASSERT_NOT_NULL(a1);
    ASSERT_NOT_NULL(a2);

    AstNode n = make_node(300);
    XrType ty;
    ty.kind = XR_KIND_STRING;

    xa_analyzer_set_node_type(a1, &n, (struct XrType *) &ty);

    XrType *via_a1 = xa_analyzer_get_node_type(a1, &n);
    XrType *via_a2 = xa_analyzer_get_node_type(a2, &n);
    ASSERT_EQ_PTR(via_a1, &ty);
    ASSERT_NULL(via_a2);

    xa_analyzer_free(a1);
    xa_analyzer_free(a2);
    teardown_isolate();
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xa_node_table direct API");
RUN_TEST(node_table_set_then_get);
RUN_TEST(node_table_get_returns_null_for_unknown);
RUN_TEST(node_table_set_null_clears_entry);
RUN_TEST(node_table_set_overwrites_existing);
RUN_TEST(node_table_clear_drops_all_entries);
RUN_TEST(node_table_growth_preserves_entries);
RUN_TEST(node_table_null_safe_api);
RUN_TEST(node_table_scope_symbol_bindings);

RUN_TEST_SUITE("xa_analyzer node_type wrappers");
RUN_TEST(analyzer_node_type_null_safe);
RUN_TEST(analyzer_set_then_get_round_trip);
RUN_TEST(analyzer_tables_are_independent_per_analyzer);
TEST_MAIN_END()
