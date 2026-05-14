/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_repl.c - Unit tests for REPL incremental compilation API
 *
 * KEY CONCEPT:
 *   Covers the REPL symbol table lifecycle, the persistent analyzer
 *   path through xr_repl_compile, the .vars / .type introspection
 *   helpers, the const round-trip, and the XR_CLI_ISOLATE_REPL
 *   profile's JIT-off invariant.
 *
 *   Tests link xray_core plus a small slice of CLI (xcli_isolate)
 *   needed to exercise the profile factory.  No interactive readline
 *   path is exercised here — completion is verified by manual REPL
 *   sessions.
 */

#include "../test_framework.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../../src/runtime/xisolate_api.h"
#include "xrepl.h"
#include "xcli_isolate.h"
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

static XrayIsolate *make_repl_iso(void) {
    return xr_cli_isolate_new(XR_CLI_ISOLATE_REPL);
}

/* Find a symbol by name; returns -1 if not present. */
static int find_symbol(const XrReplSymbolTable *t, const char *name) {
    if (!t)
        return -1;
    for (int i = 0; i < t->count; i++) {
        const char *n = xr_repl_symbol_cname(&t->symbols[i]);
        if (n && strcmp(n, name) == 0)
            return i;
    }
    return -1;
}

/* ========== Profile Invariants ========== */

TEST(repl_profile_disables_jit) {
    /* The REPL profile must keep JIT off: one-shot top-level protos
     * never hit tier-up thresholds, and cross-input shape changes
     * would invalidate any speculated guards anyway. */
    XrayIsolateParams p;
    xr_cli_isolate_params(XR_CLI_ISOLATE_REPL, &p);
    ASSERT_FALSE(p.enable_jit);
}

TEST(repl_profile_clears_each_call) {
    /* Each xr_cli_isolate_params call must fully initialize the out
     * struct — leftover bits from a prior call on the same struct
     * must not bleed through.  Set a sentinel before the second call
     * to catch any field that depends on prior content. */
    XrayIsolateParams p;
    xr_cli_isolate_params(XR_CLI_ISOLATE_RUN, &p);
    p.enable_jit = true; /* sentinel */
    xr_cli_isolate_params(XR_CLI_ISOLATE_REPL, &p);
    ASSERT_FALSE(p.enable_jit);
}

/* ========== Symbol Table Lifecycle ========== */

TEST(repl_symbols_new_and_free) {
    XrReplSymbolTable *t = xr_repl_symbols_new();
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->count, 0);
    ASSERT_GT(t->capacity, 0);
    xr_repl_symbols_free(t);
}

TEST(repl_symbols_free_null_is_noop) {
    /* Lifecycle helpers must tolerate NULL without crashing. */
    xr_repl_symbols_free(NULL);
    xr_repl_symbols_clear(NULL);
    ASSERT_TRUE(1);
}

TEST(repl_symbols_of_null_isolate) {
    ASSERT_NULL(xr_repl_symbols_of(NULL));
}

TEST(repl_symbol_cname_null_safety) {
    ASSERT_NULL(xr_repl_symbol_cname(NULL));
}

/* ========== Incremental Compile: Symbol Registration ========== */

TEST(repl_compile_let_registers_symbol) {
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_repl_compile(iso, "let x = 42\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "x");
    ASSERT_GE(i, 0);
    ASSERT_FALSE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_isolate_delete(iso);
}

TEST(repl_compile_const_marks_is_const) {
    /* `const PI = ...` must round-trip the const bit through
     * XiFunc.slot_owned_consts so .vars can distinguish let from
     * const without re-parsing. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_repl_compile(iso, "const PI = 3.14\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "PI");
    ASSERT_GE(i, 0);
    ASSERT_TRUE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_isolate_delete(iso);
}

TEST(repl_compile_function_registers_symbol) {
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_repl_compile(iso, "fn double(n) { return n * 2 }\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "double");
    ASSERT_GE(i, 0);
    /* Functions are not const declarations. */
    ASSERT_FALSE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_isolate_delete(iso);
}

TEST(repl_compile_let_and_const_round_trip) {
    /* Mixed declarations within a single input must each carry the
     * correct is_const flag. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_repl_compile(iso, "let x = 1\nconst Y = 2\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_GE(t->count, 2);

    int ix = find_symbol(t, "x");
    int iy = find_symbol(t, "Y");
    ASSERT_GE(ix, 0);
    ASSERT_GE(iy, 0);
    ASSERT_FALSE(t->symbols[ix].is_const);
    ASSERT_TRUE(t->symbols[iy].is_const);

    xr_free_code(iso, proto);
    xray_isolate_delete(iso);
}

/* ========== Cross-Input Persistence ========== */

TEST(repl_cross_input_symbol_resolves) {
    /* Verifies the persistent analyzer + symbol table: the second
     * compile must resolve `x` to the first compile's shared slot. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "let x = 42\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "let y = x + 1\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_GE(find_symbol(t, "x"), 0);
    ASSERT_GE(find_symbol(t, "y"), 0);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_cross_input_function_call) {
    /* Defining a function in input N and calling it in input N+1 is
     * the primary motivation for the persistent analyzer path. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "fn inc(n) { return n + 1 }\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "let r = inc(10)\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_GE(find_symbol(t, "inc"), 0);
    ASSERT_GE(find_symbol(t, "r"), 0);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_redefinition_reuses_slot) {
    /* `let x = 1` followed by `let x = 2` should keep one entry in
     * the symbol table, not duplicate it (repl_symbols_add_or_update
     * promises this contract). */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "let x = 1\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);
    XrProto *p2 = xr_repl_compile(iso, "let x = 2\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    int count_x = 0;
    for (int i = 0; i < t->count; i++) {
        const char *n = xr_repl_symbol_cname(&t->symbols[i]);
        if (n && strcmp(n, "x") == 0)
            count_x++;
    }
    ASSERT_EQ_INT(count_x, 1);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

/* ========== Auto-echo (Trailing Expression) ========== */

TEST(repl_auto_echo_compiles_bare_expression) {
    /* A bare trailing expression must compile (rewritten internally
     * into a print).  We do not capture stdout here; just verify
     * compilation succeeds and produces a runnable proto. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = xr_repl_compile(iso, "1 + 1\n");
    ASSERT_NOT_NULL(p);
    int rc = xr_execute(iso, p);
    (void) rc; /* execution itself just needs to not abort */

    xr_free_code(iso, p);
    xray_isolate_delete(iso);
}

TEST(repl_auto_echo_null_does_not_register_symbol) {
    /* The trailing-expression rewrite must not create any binding;
     * only the wrapping print statement.  The symbol table stays
     * empty when the only "input" is a bare expression. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = xr_repl_compile(iso, "null\n");
    ASSERT_NOT_NULL(p);
    xr_execute(iso, p);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    /* Table may have been allocated but should hold zero symbols. */
    if (t)
        ASSERT_EQ_INT(t->count, 0);

    xr_free_code(iso, p);
    xray_isolate_delete(iso);
}

/* ========== Introspection: .vars / .type ========== */

TEST(repl_print_vars_empty_is_safe) {
    /* Calling before any compile must not crash; prints "(no
     * bindings)". */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    xr_repl_print_vars(iso);
    xray_isolate_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_vars_after_compile_no_crash) {
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = xr_repl_compile(iso, "let x = 1\nconst Y = 2\n");
    ASSERT_NOT_NULL(p);
    xr_execute(iso, p);

    xr_repl_print_vars(iso);

    xr_free_code(iso, p);
    xray_isolate_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_type_null_and_empty_safe) {
    /* xr_repl_print_type tolerates NULL / "" / whitespace-only input
     * without invoking the compiler. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    xr_repl_print_type(iso, NULL);
    xr_repl_print_type(iso, "");
    xr_repl_print_type(iso, "   \t  ");

    xray_isolate_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_type_simple_expression) {
    /* Driving .type through the API end-to-end exercises:
     * synthesize source → xr_repl_compile → xr_execute.  We just
     * verify it does not crash; stdout content is checked in manual
     * REPL session tests. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    xr_repl_print_type(iso, "1 + 2");

    xray_isolate_delete(iso);
    ASSERT_TRUE(1);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("REPL Profile");
RUN_TEST(repl_profile_disables_jit);
RUN_TEST(repl_profile_clears_each_call);

RUN_TEST_SUITE("REPL Symbol Table");
RUN_TEST(repl_symbols_new_and_free);
RUN_TEST(repl_symbols_free_null_is_noop);
RUN_TEST(repl_symbols_of_null_isolate);
RUN_TEST(repl_symbol_cname_null_safety);

RUN_TEST_SUITE("REPL Incremental Compile");
RUN_TEST(repl_compile_let_registers_symbol);
RUN_TEST(repl_compile_const_marks_is_const);
RUN_TEST(repl_compile_function_registers_symbol);
RUN_TEST(repl_compile_let_and_const_round_trip);

RUN_TEST_SUITE("REPL Cross-Input Persistence");
RUN_TEST(repl_cross_input_symbol_resolves);
RUN_TEST(repl_cross_input_function_call);
RUN_TEST(repl_redefinition_reuses_slot);

RUN_TEST_SUITE("REPL Auto-echo");
RUN_TEST(repl_auto_echo_compiles_bare_expression);
RUN_TEST(repl_auto_echo_null_does_not_register_symbol);

RUN_TEST_SUITE("REPL Introspection");
RUN_TEST(repl_print_vars_empty_is_safe);
RUN_TEST(repl_print_vars_after_compile_no_crash);
RUN_TEST(repl_print_type_null_and_empty_safe);
RUN_TEST(repl_print_type_simple_expression);
TEST_MAIN_END()
