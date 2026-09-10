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
#include "frontend/parser/xtype_ref.h"
#include "runtime/value/xtype.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

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

TEST(node_table_generic_specialization_copies_tuple_and_clears_atomically) {
    XaNodeTable *table = xa_node_table_new();
    ASSERT_NOT_NULL(table);
    AstNode call = make_node(180);
    AstNode owner_decl = make_node(181);
    owner_decl.type = AST_STRUCT_DECL;
    owner_decl.as.struct_decl.type_param_count = 1;
    AstNode generic_decl = make_node(182);
    generic_decl.type = AST_METHOD_DECL;
    generic_decl.as.method_decl.type_param_count = 2;
    XrTypeRef callback_type = {.kind = XR_TREF_FUNCTION};
    XrParamNode callback_param = {.type = &callback_type};
    XrParamNode *params[1] = {&callback_param};
    generic_decl.as.method_decl.params = params;
    generic_decl.as.method_decl.param_count = 1;
    AstNode *methods[1] = {&generic_decl};
    owner_decl.as.struct_decl.methods = methods;
    owner_decl.as.struct_decl.method_count = 1;
    XrTypeRef first = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef second = {.kind = XR_TREF_STRING};
    XrTypeRef *receiver_input[1] = {&first};
    XrTypeRef *declaration_input[2] = {&first, &second};
    XaGenericSpecializationFact fact = {
        .generic_decl = &generic_decl,
        .owner_decl = &owner_decl,
        .receiver_type_args = receiver_input,
        .receiver_type_arg_count = 1u,
        .declaration_type_args = declaration_input,
        .declaration_type_arg_count = 2u,
        .effect = XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW,
    };
    ASSERT_TRUE(xa_node_table_set_generic_specialization(table, &call, &fact));
    receiver_input[0] = &second;
    declaration_input[0] = &second;

    XaGenericSpecializationFact observed = {0};
    ASSERT_TRUE(xa_node_table_get_generic_specialization(table, &call, &observed));
    ASSERT_EQ_PTR(observed.generic_decl, &generic_decl);
    ASSERT_EQ_PTR(observed.owner_decl, &owner_decl);
    ASSERT_EQ_UINT(observed.receiver_type_arg_count, 1u);
    ASSERT_EQ_PTR(observed.receiver_type_args[0], &first);
    ASSERT_EQ_UINT(observed.declaration_type_arg_count, 2u);
    ASSERT_EQ_PTR(observed.declaration_type_args[0], &first);
    ASSERT_EQ_PTR(observed.declaration_type_args[1], &second);
    ASSERT_EQ_UINT(observed.effect, XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW);

    XaGenericSpecializationFact invalid = fact;
    invalid.receiver_type_args = NULL;
    ASSERT_FALSE(xa_node_table_set_generic_specialization(table, &call, &invalid));
    invalid = fact;
    invalid.owner_decl = NULL;
    ASSERT_FALSE(xa_node_table_set_generic_specialization(table, &call, &invalid));
    invalid = fact;
    invalid.effect = (XaGenericSpecializationEffect) 255;
    ASSERT_FALSE(xa_node_table_set_generic_specialization(table, &call, &invalid));
    ASSERT_TRUE(xa_node_table_get_generic_specialization(table, &call, &observed));
    ASSERT_EQ_PTR(observed.receiver_type_args[0], &first);
    ASSERT_EQ_PTR(observed.declaration_type_args[0], &first);

    xa_node_table_clear_generic_specializations(table);
    ASSERT_FALSE(xa_node_table_get_generic_specialization(table, &call, &observed));
    ASSERT_EQ_INT(xa_node_table_size(table), 0);
    xa_node_table_free(table);
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

TEST(type_ref_bindings_grow_and_clear_independently) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    enum {
        N = 256
    };
    XrTypeRef *refs = (XrTypeRef *) malloc(sizeof(*refs) * N); /* xr:allow-raw-alloc */
    XrType *types = (XrType *) malloc(sizeof(*types) * N);     /* xr:allow-raw-alloc */
    ASSERT_NOT_NULL(refs);
    ASSERT_NOT_NULL(types);

    AstNode node = make_node(4000);
    xa_node_table_set_type(t, &node, &types[0]);
    for (int i = 0; i < N; ++i) {
        memset(&refs[i], 0, sizeof(refs[i]));
        memset(&types[i], 0, sizeof(types[i]));
        types[i].kind = XR_KIND_INT;
        ASSERT_TRUE(xa_node_table_set_type_ref_type(t, &refs[i], &types[i]));
    }
    for (int i = 0; i < N; ++i)
        ASSERT_EQ_PTR(xa_node_table_get_type_ref_type(t, &refs[i]), &types[i]);

    types[0].kind = XR_KIND_STRING;
    ASSERT_TRUE(xa_node_table_set_type_ref_type(t, &refs[0], &types[0]));
    ASSERT_EQ_PTR(xa_node_table_get_type_ref_type(t, &refs[0]), &types[0]);

    refs[0].scalar_rep = XR_NATIVE_U64;
    ASSERT_NULL(xa_node_table_get_type_ref_type(t, &refs[0]));
    ASSERT_TRUE(xa_node_table_set_type_ref_type(t, &refs[0], &types[0]));
    ASSERT_EQ_PTR(xa_node_table_get_type_ref_type(t, &refs[0]), &types[0]);

    xa_node_table_clear_type_ref_types(t);
    for (int i = 0; i < N; ++i)
        ASSERT_NULL(xa_node_table_get_type_ref_type(t, &refs[i]));
    ASSERT_EQ_PTR(xa_node_table_get_type(t, &node), &types[0]);

    ASSERT_FALSE(xa_node_table_set_type_ref_type(NULL, NULL, NULL));
    ASSERT_NULL(xa_node_table_get_type_ref_type(NULL, NULL));
    xa_node_table_clear_type_ref_types(NULL);

    xa_node_table_free(t);
    free(refs);  /* xr:allow-raw-alloc */
    free(types); /* xr:allow-raw-alloc */
}

TEST(analyzer_type_ref_bindings_follow_graphless_ast_batch) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    ASSERT_NOT_NULL(analyzer);

    AstNode first = {.type = AST_PROGRAM, .node_id = 4101};
    AstNode second = {.type = AST_PROGRAM, .node_id = 4102};
    AstNode call = {.type = AST_CALL_EXPR, .node_id = 4103};
    AstNode generic_decl = {.type = AST_FUNCTION_DECL, .node_id = 4104};
    generic_decl.as.function_decl.type_param_count = 1;
    XrTypeRef type_ref = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *type_args[1] = {&type_ref};
    XrType type = {.kind = XR_KIND_INT, .scalar_rep = XR_NATIVE_I64};
    XaGenericSpecializationFact specialization = {
        .generic_decl = &generic_decl,
        .declaration_type_args = type_args,
        .declaration_type_arg_count = 1u,
    };
    XaGenericSpecializationFact observed = {0};

    xa_analyzer_analyze(analyzer, "first.xr", &first);
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &type_ref, &type));
    ASSERT_TRUE(xa_analyzer_set_generic_specialization(analyzer, &call, &specialization));
    ASSERT_TRUE(xa_analyzer_get_generic_specialization(analyzer, &call, &observed));
    xa_analyzer_analyze(analyzer, "first.xr", &first);
    ASSERT_EQ_PTR(xa_analyzer_get_type_ref_type(analyzer, &type_ref), &type);

    ASSERT_TRUE(xr_compiler_session_reset_incremental(session));
    ASSERT_FALSE(xa_analyzer_get_generic_specialization(analyzer, &call, &observed));
    ASSERT_NULL(xa_analyzer_get_type_ref_type(analyzer, &type_ref));
    xa_analyzer_analyze(analyzer, "first-reset-id.xr", &first);
    ASSERT_NULL(xa_analyzer_get_type_ref_type(analyzer, &type_ref));

    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &type_ref, &type));
    ASSERT_TRUE(xa_analyzer_set_generic_specialization(analyzer, &call, &specialization));
    xa_analyzer_analyze(analyzer, "second.xr", &second);
    ASSERT_FALSE(xa_analyzer_get_generic_specialization(analyzer, &call, &observed));
    ASSERT_NULL(xa_analyzer_get_type_ref_type(analyzer, &type_ref));

    xa_analyzer_free(analyzer);
    xr_compiler_session_delete(session);
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

TEST(node_table_ct_value_round_trip) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode n = make_node(600);
    XrCtValue value = {.kind = XR_CT_BOOL, .as.bool_val = true};
    XrCtValue got = {0};

    ASSERT(!xa_node_table_get_ct_value(t, &n, &got));
    xa_node_table_set_ct_value(t, &n, &value);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);
    ASSERT(xa_node_table_get_ct_value(t, &n, &got));
    ASSERT_EQ_INT(got.kind, XR_CT_BOOL);
    ASSERT_EQ_INT(got.as.bool_val, true);

    xa_node_table_set_ct_value(t, &n, NULL);
    ASSERT(!xa_node_table_get_ct_value(t, &n, &got));
    ASSERT_EQ_INT(xa_node_table_size(t), 0);

    xa_node_table_free(t);
}

TEST(node_table_ct_value_preserves_type_entry) {
    XaNodeTable *t = xa_node_table_new();
    ASSERT_NOT_NULL(t);

    AstNode n = make_node(601);
    XrType ty;
    ty.kind = XR_KIND_RUNE;
    XrCtValue value = {.kind = XR_CT_CHAR, .as.rune_val = 'x'};

    xa_node_table_set_type(t, &n, &ty);
    xa_node_table_set_ct_value(t, &n, &value);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);

    xa_node_table_set_ct_value(t, &n, NULL);
    ASSERT_EQ_PTR(xa_node_table_get_type(t, &n), &ty);
    ASSERT_EQ_INT(xa_node_table_size(t), 1);

    xa_node_table_free(t);
}

/* ====================================================================== */
/* Analyzer-API wrapper tests                                              */
/* ====================================================================== */

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;

static void setup_isolate(void) {
    XrVMConfig p = {0};
    g_iso = xray_vm_new_full(&p);
    g_session = xr_compiler_session_current_for_isolate(g_iso);
    ASSERT_NOT_NULL(g_session);
}

static void teardown_isolate(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
        g_session = NULL;
    }
}

TEST(analyzer_node_type_null_safe) {
    // No isolate / no analyzer: the wrappers must absorb NULL on both
    // sides without crashing or segfaulting.
    xa_analyzer_set_node_type(NULL, NULL, NULL);
    ASSERT_NULL(xa_analyzer_get_node_type(NULL, NULL));

    setup_isolate();
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT_NOT_NULL(a);

    // Live analyzer, NULL node: still a no-op / NULL.
    xa_analyzer_set_node_type(a, NULL, NULL);
    ASSERT_NULL(xa_analyzer_get_node_type(a, NULL));

    xa_analyzer_free(a);
    teardown_isolate();
}

TEST(analyzer_set_then_get_round_trip) {
    setup_isolate();
    XaAnalyzer *a = xa_analyzer_new(g_session);
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
    XaAnalyzer *a1 = xa_analyzer_new(g_session);
    XaAnalyzer *a2 = xa_analyzer_new(g_session);
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
RUN_TEST(node_table_generic_specialization_copies_tuple_and_clears_atomically);
RUN_TEST(node_table_growth_preserves_entries);
RUN_TEST(type_ref_bindings_grow_and_clear_independently);
RUN_TEST(analyzer_type_ref_bindings_follow_graphless_ast_batch);
RUN_TEST(node_table_null_safe_api);
RUN_TEST(node_table_scope_symbol_bindings);
RUN_TEST(node_table_ct_value_round_trip);
RUN_TEST(node_table_ct_value_preserves_type_entry);

RUN_TEST_SUITE("xa_analyzer node_type wrappers");
RUN_TEST(analyzer_node_type_null_safe);
RUN_TEST(analyzer_set_then_get_round_trip);
RUN_TEST(analyzer_tables_are_independent_per_analyzer);
TEST_MAIN_END()
