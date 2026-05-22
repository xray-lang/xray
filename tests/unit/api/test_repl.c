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
 *   helpers, the const round-trip, and the XR_ISOLATE_PROFILE_REPL
 *   profile's JIT-off invariant.
 *
 *   Tests link xray_core plus a small slice of CLI (xisolate_profile)
 *   needed to exercise the profile factory.  No interactive readline
 *   path is exercised here — completion is verified by manual REPL
 *   sessions.
 */

#include "../test_framework.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xglobal_dict.h"
#include "../../../src/runtime/xexec_state.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "xrepl.h"
#include "xisolate_profile.h"
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

static XrayIsolate *make_repl_iso(void) {
    return xr_isolate_profile_new(XR_ISOLATE_PROFILE_REPL);
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

/* ========== Globals Dict API (Phase 1 of REPL × top-level globals migration) ==========
 *
 * The dict is the per-isolate name-keyed top-level binding store.
 * These tests exercise the C API directly, ahead of the lowering
 * pipeline switch (Phase 2) — they prove the runtime store is sound
 * before anything starts emitting OP_GETGLOBAL / OP_SETGLOBAL. */

TEST(globals_dict_initialized_with_isolate) {
    /* Every isolate constructed through the standard path must have
     * a non-NULL globals dict ready for use right after init.  This
     * is the structural invariant the new opcodes rely on. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    ASSERT_NOT_NULL(iso->vm.globals);
    ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 0);
    xray_isolate_delete(iso);
}

TEST(globals_dict_set_get_round_trip) {
    /* Set a binding under a name; get returns the same XrValue.
     * Uses xr_compile_time_intern so the key is a real interned
     * XrString, mirroring the runtime contract for OP_SETGLOBAL. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_compile_time_intern(iso, "answer", 6);
    ASSERT_NOT_NULL(name);
    XrValue v;
    v.tag = 0;
    v.i = 42;
    xr_global_dict_set(iso->vm.globals, name, v);

    ASSERT_TRUE(xr_global_dict_has(iso->vm.globals, name));
    ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 1);
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_EQ_INT((int) out.i, 42);

    xray_isolate_delete(iso);
}

TEST(globals_dict_overwrite_keeps_count) {
    /* Reassigning the same name must not grow the dict — the binding
     * is identified by name, the integer count is the # of distinct
     * names. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_compile_time_intern(iso, "x", 1);
    XrValue a = {.i = 1, .tag = 0};
    XrValue b = {.i = 2, .tag = 0};
    xr_global_dict_set(iso->vm.globals, name, a);
    xr_global_dict_set(iso->vm.globals, name, b);

    ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 1);
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_EQ_INT((int) out.i, 2);

    xray_isolate_delete(iso);
}

TEST(globals_dict_missing_key_returns_null) {
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_compile_time_intern(iso, "ghost", 5);
    ASSERT_FALSE(xr_global_dict_has(iso->vm.globals, name));
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_TRUE(XR_IS_NULL(out));

    xray_isolate_delete(iso);
}

/* ========== Profile Invariants ========== */

TEST(repl_profile_disables_jit) {
    /* The REPL profile must keep JIT off: one-shot top-level protos
     * never hit tier-up thresholds, and cross-input shape changes
     * would invalidate any speculated guards anyway. */
    XrayIsolateParams p;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_REPL, &p);
    ASSERT_FALSE(p.enable_jit);
}

TEST(repl_profile_clears_each_call) {
    /* Each xr_isolate_profile_params call must fully initialize the out
     * struct — leftover bits from a prior call on the same struct
     * must not bleed through.  Set a sentinel before the second call
     * to catch any field that depends on prior content. */
    XrayIsolateParams p;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &p);
    p.enable_jit = true; /* sentinel */
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_REPL, &p);
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
     * the primary motivation for the persistent analyzer path.  The
     * function value bound to `r` must be the call result (11), not
     * the closure itself — earlier versions of the REPL emit pipeline
     * left a stale shared_offset on nested protos, so cross-input
     * calls returned the closure value instead of invoking it. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "fn inc(n: int) -> int { return n + 1 }\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "let r = inc(10)\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_GE(find_symbol(t, "inc"), 0);
    ASSERT_GE(find_symbol(t, "r"), 0);

    int64_t r_val = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r_val));
    ASSERT_EQ_INT(r_val, 11);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_cross_input_function_reads_shared) {
    /* Regression: a function body that reads an outer-scope shared
     * variable must resolve to the correct slot when called from a
     * later REPL input.  The bug was that xi_emit baked shared_offset
     * into the nested proto at emit time, but REPL forces absolute
     * indices on the top-level proto only; nested protos kept the
     * stale offset and read the wrong slot.  Symptom: `let r =
     * getx()` bound `r` to the closure itself instead of x's value. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "let x = 10\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "fn getx() -> int { return x }\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrProto *p3 = xr_repl_compile(iso, "let r = getx()\n");
    ASSERT_NOT_NULL(p3);
    xr_execute(iso, p3);

    int64_t r_val = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r_val));
    ASSERT_EQ_INT(r_val, 10);

    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_cross_input_function_mutates_shared) {
    /* Same offset bug from the read side, exercised on the write
     * path: a function that increments a shared counter must actually
     * mutate the right slot across REPL inputs.  Before the fix this
     * test would observe counter==0 forever because SETSHARED in the
     * nested proto wrote to the wrong slot, leaving the real counter
     * untouched. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "let counter = 0\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 =
        xr_repl_compile(iso, "fn bump() -> int { counter = counter + 1; return counter }\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrProto *p3 = xr_repl_compile(iso, "let r1 = bump()\n");
    ASSERT_NOT_NULL(p3);
    xr_execute(iso, p3);

    XrProto *p4 = xr_repl_compile(iso, "let r2 = bump()\n");
    ASSERT_NOT_NULL(p4);
    xr_execute(iso, p4);

    int64_t r1 = 0, r2 = 0, counter = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r1", &r1));
    ASSERT_TRUE(xr_repl_peek_int(iso, "r2", &r2));
    ASSERT_TRUE(xr_repl_peek_int(iso, "counter", &counter));
    ASSERT_EQ_INT(r1, 1);
    ASSERT_EQ_INT(r2, 2);
    ASSERT_EQ_INT(counter, 2);

    xr_free_code(iso, p4);
    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_redefinition_reuses_slot) {
    /* `let x = 1` followed by `let x = 2` should keep one entry in
     * the symbol table, not duplicate it (repl_symbols_add_or_update
     * promises this contract).  Also asserts the second value
     * actually replaces the first — without value verification the
     * test passes even with a slot-collision bug. */
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

    int64_t v = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "x", &v));
    ASSERT_EQ_INT(v, 2);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_function_calls_function_cross_input) {
    /* fn b defined in input 1, fn a defined in input 2 calls b, then
     * a() executed in input 3.  Verifies that cross-input function
     * resolution chains transitively: a's body must resolve b through
     * the same persistent global table. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "fn b() -> int { return 100 }\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "fn a() -> int { return b() + 1 }\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrProto *p3 = xr_repl_compile(iso, "let r = a()\n");
    ASSERT_NOT_NULL(p3);
    xr_execute(iso, p3);

    int64_t r = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r));
    ASSERT_EQ_INT(r, 101);

    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_function_recursive_self_reference) {
    /* Single-input recursive function — the function name must
     * resolve to its own slot during its own body lowering.  Locks
     * down the forward-reference contract for self-recursive top-level
     * functions in REPL mode. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    const char *src = "fn fact(n: int) -> int {\n"
                      "  if (n <= 1) { return 1 }\n"
                      "  return n * fact(n - 1)\n"
                      "}\n";
    XrProto *p1 = xr_repl_compile(iso, src);
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "let r = fact(5)\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    int64_t r = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r));
    ASSERT_EQ_INT(r, 120);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

TEST(repl_function_mutates_array_cross_input) {
    /* Mutation through a captured array reference: the array's heap
     * object identity is shared, so push() in one function call must
     * be visible to any later lookup of the same name.  The bound
     * value in the globals table is the array reference, not a slot
     * snapshot. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "let arr: array<int> = []\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "fn push_one() { arr.push(1) }\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrProto *p3 = xr_repl_compile(iso, "push_one(); push_one()\n");
    ASSERT_NOT_NULL(p3);
    xr_execute(iso, p3);

    XrProto *p4 = xr_repl_compile(iso, "let n = arr.length\n");
    ASSERT_NOT_NULL(p4);
    xr_execute(iso, p4);

    int64_t n = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "n", &n));
    ASSERT_EQ_INT(n, 2);

    xr_free_code(iso, p4);
    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}

#if 0
/* Phase 3 acceptance — currently FAILS: cross-input class
 * instantiation does not resolve the class name in the second input.
 * Symptom in REPL: `Point(7, 8).x` reports unresolved variable
 * 'Point'.  Root cause is the same shared-slot model that bit
 * functions: class declarations are not visible across inputs
 * because the persistent symbol path drops them.
 *
 * After the globals-dict migration (Phase 3) classes will live in
 * the globals dict by name like any other top-level binding, and
 * this test must turn green.  Enable then. */
TEST(repl_class_instantiation_cross_input) {
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    const char *cls =
        "class Point {\n"
        "  let x: int; let y: int\n"
        "  constructor(x: int, y: int) { this.x = x; this.y = y }\n"
        "}\n";
    XrProto *p1 = xr_repl_compile(iso, cls);
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    XrProto *p2 = xr_repl_compile(iso, "let px = Point(7, 8).x\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    int64_t px = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "px", &px));
    ASSERT_EQ_INT(px, 7);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_isolate_delete(iso);
}
#endif

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

TEST(repl_auto_echo_creates_it_binding) {
    /* The trailing-expression rewrite must bind the result to `it`
     * (GHCi convention) so the user can chain off the previous
     * value.  A bare `null` echo therefore creates exactly one
     * symbol: `it`, with value null.  The print itself is
     * suppressed via skip_null. */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = xr_repl_compile(iso, "null\n");
    ASSERT_NOT_NULL(p);
    xr_execute(iso, p);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->count, 1);
    ASSERT_STR_EQ(xr_repl_symbol_cname(&t->symbols[0]), "it");

    xr_free_code(iso, p);
    xray_isolate_delete(iso);
}

TEST(repl_auto_echo_it_chaining) {
    /* `it` carries across REPL inputs, so `1 + 2` followed by
     * `it * 10` evaluates `it` against the prior result (3). */
    XrayIsolate *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = xr_repl_compile(iso, "1 + 2\n");
    ASSERT_NOT_NULL(p1);
    xr_execute(iso, p1);

    /* Subsequent compile must use AST_ASSIGNMENT for `it` (not a
     * re-declaration), so it must succeed without analyzer
     * "redeclared name" errors. */
    XrProto *p2 = xr_repl_compile(iso, "it * 10\n");
    ASSERT_NOT_NULL(p2);
    xr_execute(iso, p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    int idx = find_symbol(t, "it");
    ASSERT_GE(idx, 0);
    /* Still exactly one `it` entry after the second echo. */
    int count_it = 0;
    for (int i = 0; i < t->count; i++) {
        const char *n = xr_repl_symbol_cname(&t->symbols[i]);
        if (n && strcmp(n, "it") == 0)
            count_it++;
    }
    ASSERT_EQ_INT(count_it, 1);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
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
RUN_TEST_SUITE("Globals Dict");
RUN_TEST(globals_dict_initialized_with_isolate);
RUN_TEST(globals_dict_set_get_round_trip);
RUN_TEST(globals_dict_overwrite_keeps_count);
RUN_TEST(globals_dict_missing_key_returns_null);

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
RUN_TEST(repl_cross_input_function_reads_shared);
RUN_TEST(repl_cross_input_function_mutates_shared);
RUN_TEST(repl_redefinition_reuses_slot);
RUN_TEST(repl_function_calls_function_cross_input);
RUN_TEST(repl_function_recursive_self_reference);
RUN_TEST(repl_function_mutates_array_cross_input);

RUN_TEST_SUITE("REPL Auto-echo");
RUN_TEST(repl_auto_echo_compiles_bare_expression);
RUN_TEST(repl_auto_echo_creates_it_binding);
RUN_TEST(repl_auto_echo_it_chaining);

RUN_TEST_SUITE("REPL Introspection");
RUN_TEST(repl_print_vars_empty_is_safe);
RUN_TEST(repl_print_vars_after_compile_no_crash);
RUN_TEST(repl_print_type_null_and_empty_safe);
RUN_TEST(repl_print_type_simple_expression);
TEST_MAIN_END()
