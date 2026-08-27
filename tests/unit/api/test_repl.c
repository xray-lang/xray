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
 *   path through xr_repl_eval, the .vars / .type introspection
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
#include "xray_vm.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xglobal_dict.h"
#include "../../../src/runtime/xexec_state.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../src/plan/target/xr_target_profile.h"
#include "../../../src/runtime/abi/xr_runtime_target_profile.h"
#include "../../../src/module/xmodule_identity.h"
#include "xrepl.h"
#include "xisolate_profile.h"
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

static const XrModuleIdentityAuthority k_repl_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "repl-fixture-v1",
};

static XrVMRuntime *make_repl_iso(void) {
    return xr_isolate_profile_new(XR_ISOLATE_PROFILE_REPL);
}

static XrProto *eval_repl(XrCompilerSession *session, XrVMRuntime *isolate, const char *source) {
    XrReplEvalResult result = xr_repl_eval(session, isolate, source, &k_repl_memory_authority);
    if (result.status == XR_REPL_EVAL_OK)
        return result.proto;
    if (result.proto)
        xr_free_code(isolate, result.proto);
    return NULL;
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

static void read_tmp_output(FILE *out, char *buf, size_t cap) {
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(cap > 0);
    fflush(out);
    rewind(out);
    size_t n = fread(buf, 1, cap - 1, out);
    buf[n] = '\0';
}

/* ========== Globals Dict API (Phase 1 of REPL × top-level globals migration) ==========
 *
 * The dict is the per-isolate name-keyed top-level binding store.
 * These tests exercise the C API directly, ahead of the lowering
 * pipeline switch (Phase 2) — they prove the runtime store is sound
 * before anything starts emitting OP_GETGLOBAL / OP_SETGLOBAL. */

TEST(product_profiles_install_exact_authority_and_reject_missing_profile) {
    static const XrIsolateProfile profiles[] = {
        XR_ISOLATE_PROFILE_REPL,
        XR_ISOLATE_PROFILE_RUN,
        XR_ISOLATE_PROFILE_EVAL,
        XR_ISOLATE_PROFILE_TEST,
    };
    XrTargetProfile *expected = NULL;
    char error[256] = {0};
    ASSERT_TRUE(xr_runtime_target_profile_build_native_hosted(&expected, error, sizeof(error)));

    XrVMRuntime *iso = make_repl_iso();
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (i != 0)
            iso = xr_isolate_profile_new(profiles[i]);
        ASSERT_NOT_NULL(iso);
        ASSERT_NOT_NULL(iso->vm.globals);
        ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 0);

        XrCompilerSession *installed_session = xr_compiler_session_current_for_isolate(iso);
        ASSERT_NOT_NULL(installed_session);
        ASSERT_TRUE(xr_target_profile_require_exact(
            expected, xr_compiler_session_target_profile(installed_session), error, sizeof(error)));

        if (i == 0) {
            XrCompilerSessionConfig config = {
                .vm_host = iso,
                .source_file = "<missing-target-profile>",
                .repl_mode = true,
            };
            XrCompilerSession *missing_session = xr_compiler_session_new(&config);
            ASSERT_NOT_NULL(missing_session);
            ASSERT_NULL(xr_compiler_session_target_profile(missing_session));
            XrReplEvalResult result =
                xr_repl_eval(missing_session, iso, "1 + 2\n", &k_repl_memory_authority);
            ASSERT_EQ(XR_REPL_EVAL_COMPILE_ERROR, result.status);
            ASSERT_NULL(result.proto);
            xr_compiler_session_delete(missing_session);
        }
        xray_vm_delete(iso);
    }
    xr_target_profile_free(expected);
}

TEST(globals_dict_set_get_round_trip) {
    /* Set a binding under a name; get returns the same XrValue.
     * Uses xr_string_intern_permanent so the key is a real interned
     * XrString, mirroring the runtime contract for OP_SETGLOBAL. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_string_intern_permanent(iso, "answer", 6);
    ASSERT_NOT_NULL(name);
    XrValue v;
    v.tag = 0;
    v.i = 42;
    xr_global_dict_set(iso->vm.globals, name, v);

    ASSERT_TRUE(xr_global_dict_has(iso->vm.globals, name));
    ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 1);
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_EQ_INT((int) out.i, 42);

    xray_vm_delete(iso);
}

TEST(globals_dict_overwrite_keeps_count) {
    /* Reassigning the same name must not grow the dict — the binding
     * is identified by name, the integer count is the # of distinct
     * names. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_string_intern_permanent(iso, "x", 1);
    XrValue a = {.i = 1, .tag = 0};
    XrValue b = {.i = 2, .tag = 0};
    xr_global_dict_set(iso->vm.globals, name, a);
    xr_global_dict_set(iso->vm.globals, name, b);

    ASSERT_EQ_INT((int) xr_global_dict_count(iso->vm.globals), 1);
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_EQ_INT((int) out.i, 2);

    xray_vm_delete(iso);
}

TEST(globals_dict_missing_key_returns_null) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrString *name = xr_string_intern_permanent(iso, "ghost", 5);
    ASSERT_FALSE(xr_global_dict_has(iso->vm.globals, name));
    XrValue out = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_TRUE(XR_IS_NULL(out));

    xray_vm_delete(iso);
}

TEST(sync_root_elides_coroutine_with_ordinary_allocation) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    ASSERT_NULL(iso->main_coro);

    XrProto *proto = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                               "var xs = [1, 2, 3]\nvar n = len(xs)\n");
    ASSERT_NOT_NULL(proto);
    ASSERT_NULL(iso->main_coro);

    XrString *name = xr_string_intern_permanent(iso, "n", 1);
    ASSERT_NOT_NULL(name);
    XrValue value = xr_global_dict_get(iso->vm.globals, name);
    ASSERT_TRUE(XR_IS_INT(value));
    ASSERT_EQ_INT((int) XR_TO_INT(value), 3);

    xr_free_code(iso, proto);
    xray_vm_delete(iso);
}

/* ========== Profile Invariants ========== */

TEST(repl_profile_clears_each_call) {
    /* Each xr_isolate_profile_params call must fully initialize the out
     * struct — leftover bits from a prior call on the same struct
     * must not bleed through.  Set a sentinel before the second call
     * to catch any field that depends on prior content. */
    XrVMConfig p;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_RUN, &p);
    p.trace_execution = true; /* sentinel */
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_REPL, &p);
    ASSERT_FALSE(p.trace_execution);
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
    ASSERT_TRUE(1);
}

TEST(repl_symbols_of_null_isolate) {
    ASSERT_NULL(xr_repl_symbols_of(NULL));
}

TEST(repl_symbol_cname_null_safety) {
    ASSERT_NULL(xr_repl_symbol_cname(NULL));
}

/* ========== Incremental Evaluation: Symbol Registration ========== */

TEST(repl_eval_let_registers_symbol) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 42\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "x");
    ASSERT_GE(i, 0);
    ASSERT_FALSE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_vm_delete(iso);
}

TEST(repl_eval_const_marks_is_const) {
    /* `const PI = ...` must round-trip the const bit through
     * XiFunc.slot_owned_consts so .vars can distinguish var from
     * const without re-parsing. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "const PI = 3.14\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "PI");
    ASSERT_GE(i, 0);
    ASSERT_TRUE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_vm_delete(iso);
}

TEST(repl_eval_function_registers_symbol) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                               "fn double(n: i64) -> i64 { return n * 2 }\n");
    ASSERT_NOT_NULL(proto);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    int i = find_symbol(t, "double");
    ASSERT_GE(i, 0);
    /* Functions are not const declarations. */
    ASSERT_FALSE(t->symbols[i].is_const);

    xr_free_code(iso, proto);
    xray_vm_delete(iso);
}

TEST(repl_eval_let_and_const_round_trip) {
    /* Mixed declarations within a single input must each carry the
     * correct is_const flag. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *proto =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 1\nconst Y = 2\n");
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
    xray_vm_delete(iso);
}

/* ========== Cross-Input Persistence ========== */

TEST(repl_cross_input_symbol_resolves) {
    /* Verifies the persistent analyzer + symbol table: the second
     * compile must resolve `x` to the first compile's shared slot. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 42\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var y = x + 1\n");
    ASSERT_NOT_NULL(p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_GE(find_symbol(t, "x"), 0);
    ASSERT_GE(find_symbol(t, "y"), 0);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_cross_input_function_call) {
    /* Defining a function in input N and calling it in input N+1 is
     * the primary motivation for the persistent analyzer path.  The
     * function value bound to `r` must be the call result (11), not
     * the closure itself — earlier versions of the REPL emit pipeline
     * left a stale shared_offset on nested protos, so cross-input
     * calls returned the closure value instead of invoking it. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn inc(n: i64) -> i64 { return n + 1 }\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r = inc(10)\n");
    ASSERT_NOT_NULL(p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_GE(find_symbol(t, "inc"), 0);
    ASSERT_GE(find_symbol(t, "r"), 0);

    int64_t r_val = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r_val));
    ASSERT_EQ_INT(r_val, 11);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_cross_input_function_reads_shared) {
    /* Regression: a function body that reads an outer-scope shared
     * variable must resolve to the correct slot when called from a
     * later REPL input.  The bug was that xi_emit baked shared_offset
     * into the nested proto at emit time, but REPL forces absolute
     * indices on the top-level proto only; nested protos kept the
     * stale offset and read the wrong slot.  Symptom: `var r =
     * getx()` bound `r` to the closure itself instead of x's value. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 10\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn getx() -> i64 { return x }\n");
    ASSERT_NOT_NULL(p2);

    XrProto *p3 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r = getx()\n");
    ASSERT_NOT_NULL(p3);

    int64_t r_val = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r_val));
    ASSERT_EQ_INT(r_val, 10);

    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_cross_input_function_mutates_shared) {
    /* Same offset bug from the read side, exercised on the write
     * path: a function that increments a shared counter must actually
     * mutate the right slot across REPL inputs.  Before the fix this
     * test would observe counter==0 forever because SETSHARED in the
     * nested proto wrote to the wrong slot, leaving the real counter
     * untouched. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var counter = 0\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn bump() -> i64 { counter = counter + 1; return counter }\n");
    ASSERT_NOT_NULL(p2);

    XrProto *p3 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r1 = bump()\n");
    ASSERT_NOT_NULL(p3);

    XrProto *p4 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r2 = bump()\n");
    ASSERT_NOT_NULL(p4);

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
    xray_vm_delete(iso);
}

TEST(repl_redefinition_reuses_slot) {
    /* `var x = 1` followed by `var x = 2` should keep one entry in
     * the symbol table, not duplicate it (repl_symbols_add_or_update
     * promises this contract).  Also asserts the second value
     * actually replaces the first — without value verification the
     * test passes even with a slot-collision bug. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 1\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 2\n");
    ASSERT_NOT_NULL(p2);

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
    xray_vm_delete(iso);
}

TEST(repl_function_calls_function_cross_input) {
    /* fn b defined in input 1, fn a defined in input 2 calls b, then
     * a() executed in input 3.  Verifies that cross-input function
     * resolution chains transitively: a's body must resolve b through
     * the same persistent global table. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn b() -> i64 { return 100 }\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn a() -> i64 { return b() + 1 }\n");
    ASSERT_NOT_NULL(p2);

    XrProto *p3 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r = a()\n");
    ASSERT_NOT_NULL(p3);

    int64_t r = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r));
    ASSERT_EQ_INT(r, 101);

    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_function_recursive_self_reference) {
    /* Single-input recursive function — the function name must
     * resolve to its own slot during its own body lowering.  Locks
     * down the forward-reference contract for self-recursive top-level
     * functions in REPL mode. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    const char *src = "fn fact(n: i64) -> i64 {\n"
                      "  if (n <= 1) { return 1 }\n"
                      "  return n * fact(n - 1)\n"
                      "}\n";
    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, src);
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var r = fact(5)\n");
    ASSERT_NOT_NULL(p2);

    int64_t r = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "r", &r));
    ASSERT_EQ_INT(r, 120);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_function_mutates_array_cross_input) {
    /* Mutation through a captured array reference: the array's heap
     * object identity is shared, so push() in one function call must
     * be visible to any later lookup of the same name.  The bound
     * value in the globals table is the array reference, not a slot
     * snapshot. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var arr: Array<i64> = []\n");
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso,
                            "fn push_one() { arr.push(1) }\n");
    ASSERT_NOT_NULL(p2);

    XrProto *p3 =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "push_one(); push_one()\n");
    ASSERT_NOT_NULL(p3);

    XrProto *p4 =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var n = len(arr)\n");
    ASSERT_NOT_NULL(p4);

    int64_t n = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "n", &n));
    ASSERT_EQ_INT(n, 2);

    xr_free_code(iso, p4);
    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
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
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    const char *cls =
        "class Point {\n"
        "  var x: i64; var y: i64\n"
        "  constructor(x: i64, y: i64) { this.x = x; this.y = y }\n"
        "}\n";
    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, cls);
    ASSERT_NOT_NULL(p1);

    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var px = Point(7, 8).x\n");
    ASSERT_NOT_NULL(p2);

    int64_t px = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "px", &px));
    ASSERT_EQ_INT(px, 7);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}
#endif

/* ========== Auto-echo (Trailing Expression) ========== */

TEST(repl_auto_echo_compiles_bare_expression) {
    /* A bare trailing expression must compile (rewritten internally
     * into a print).  We do not capture stdout here; just verify
     * compilation succeeds and produces a runnable proto. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "1 + 1\n");
    ASSERT_NOT_NULL(p);

    xr_free_code(iso, p);
    xray_vm_delete(iso);
}

TEST(repl_auto_echo_creates_it_binding) {
    /* The public `it` name is an alias to immutable, versioned result
     * storage. The implementation name is not a user declaration and
     * null printing remains suppressed via skip_null. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "null\n");
    ASSERT_NOT_NULL(p);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->count, 0);
    ASSERT_EQ_INT(t->result_count, 1);
    ASSERT_TRUE(xr_repl_has_last_result(iso));

    xr_free_code(iso, p);
    xray_vm_delete(iso);
}

TEST(repl_auto_echo_it_chaining) {
    /* `it` carries across REPL inputs, so `1 + 2` followed by
     * `it * 10` evaluates `it` against the prior result (3). */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p1 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "1 + 2\n");
    ASSERT_NOT_NULL(p1);

    /* `it` resolves to the prior immutable result while the new result gets
     * a fresh hidden binding. */
    XrProto *p2 = eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "it * 10\n");
    ASSERT_NOT_NULL(p2);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->count, 0);
    ASSERT_EQ_INT(t->result_count, 2);
    int64_t value = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "it", &value));
    ASSERT_EQ_INT(value, 30);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_it_can_change_static_type) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso, "1\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(session, iso, "\"hello\"\n");
    ASSERT_NOT_NULL(p2);
    XrProto *p3 = eval_repl(session, iso, "var n = len(it)\n");
    ASSERT_NOT_NULL(p3);

    int64_t n = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "n", &n));
    ASSERT_EQ_INT(n, 5);

    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_unit_result_does_not_replace_it) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso,
                            "var calls = 0\n"
                            "fn test() { calls = calls + 1 }\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(session, iso, "test\n");
    ASSERT_NOT_NULL(p2);
    XrProto *p3 = eval_repl(session, iso, "test()\n");
    ASSERT_NOT_NULL(p3);
    XrProto *p4 = eval_repl(session, iso, "it()\n");
    ASSERT_NOT_NULL(p4);

    XrReplSymbolTable *t = xr_repl_symbols_of(iso);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->result_count, 1);
    int64_t calls = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "calls", &calls));
    ASSERT_EQ_INT(calls, 2);

    xr_free_code(iso, p4);
    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_it_reference_is_a_typed_snapshot) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso, "1\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(session, iso, "fn previous() -> i64 { return it }\n");
    ASSERT_NOT_NULL(p2);
    XrProto *p3 = eval_repl(session, iso, "\"later\"\n");
    ASSERT_NOT_NULL(p3);
    XrProto *p4 = eval_repl(session, iso, "var saved = previous()\n");
    ASSERT_NOT_NULL(p4);

    int64_t saved = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "saved", &saved));
    ASSERT_EQ_INT(saved, 1);

    xr_free_code(iso, p4);
    xr_free_code(iso, p3);
    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_unit_before_value_does_not_create_it) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso, "fn noop() {}\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(session, iso, "noop()\n");
    ASSERT_NOT_NULL(p2);
    ASSERT_FALSE(xr_repl_has_last_result(iso));

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_it_is_reserved_for_implicit_results) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrReplEvalResult result = xr_repl_eval(session, iso, "var it = 1\n", &k_repl_memory_authority);
    ASSERT_EQ_INT(result.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_NULL(result.proto);
    ASSERT_FALSE(xr_repl_has_last_result(iso));

    xray_vm_delete(iso);
}

TEST(repl_cross_input_call_rejects_missing_type_authority) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso, "fn inc(n: i64) -> i64 { return n + 1 }\n");
    ASSERT_NOT_NULL(p1);
    XrReplSymbolTable *table = xr_repl_symbols_of(iso);
    int index = find_symbol(table, "inc");
    ASSERT_GE(index, 0);
    table->symbols[index].type = NULL;

    XrReplEvalResult rejected =
        xr_repl_eval(session, iso, "var r = inc(10)\n", &k_repl_memory_authority);
    ASSERT_EQ_INT(rejected.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_NULL(rejected.proto);

    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_cross_input_call_rejects_invalid_symbol_authority) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso, "fn inc(n: i64) -> i64 { return n + 1 }\n");
    ASSERT_NOT_NULL(p1);
    XrReplSymbolTable *table = xr_repl_symbols_of(iso);
    int index = find_symbol(table, "inc");
    ASSERT_GE(index, 0);
    table->symbols[index].symbol_id = UINT32_MAX;

    XrReplEvalResult rejected =
        xr_repl_eval(session, iso, "var r = inc(10)\n", &k_repl_memory_authority);
    ASSERT_EQ_INT(rejected.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_NULL(rejected.proto);

    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

TEST(repl_eval_requires_explicit_valid_memory_identity) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);
    XrModuleIdentityAuthority invalid = {
        .kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "<eval>",
    };

    XrReplEvalResult missing = xr_repl_eval(session, iso, "1 + 2\n", NULL);
    XrReplEvalResult malformed = xr_repl_eval(session, iso, "1 + 2\n", &invalid);
    ASSERT_EQ_INT(missing.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_EQ_INT(malformed.status, XR_REPL_EVAL_COMPILE_ERROR);
    ASSERT_NULL(missing.proto);
    ASSERT_NULL(malformed.proto);

    xray_vm_delete(iso);
}

TEST(repl_auto_echo_evaluates_expression_once) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(iso);

    XrProto *p1 = eval_repl(session, iso,
                            "var counter = 0\n"
                            "fn next() -> i64 { counter = counter + 1; return counter }\n");
    ASSERT_NOT_NULL(p1);
    XrProto *p2 = eval_repl(session, iso, "next()\n");
    ASSERT_NOT_NULL(p2);

    int64_t counter = 0;
    int64_t result = 0;
    ASSERT_TRUE(xr_repl_peek_int(iso, "counter", &counter));
    ASSERT_TRUE(xr_repl_peek_int(iso, "it", &result));
    ASSERT_EQ_INT(counter, 1);
    ASSERT_EQ_INT(result, 1);

    xr_free_code(iso, p2);
    xr_free_code(iso, p1);
    xray_vm_delete(iso);
}

/* ========== Introspection: .vars / .type ========== */

TEST(repl_print_vars_empty_is_safe) {
    /* Calling before any compile must not crash; prints "(no
     * bindings)". */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);
    xr_repl_print_vars(iso);
    xray_vm_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_vars_after_compile_no_crash) {
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    XrProto *p =
        eval_repl(xr_compiler_session_current_for_isolate(iso), iso, "var x = 1\nconst Y = 2\n");
    ASSERT_NOT_NULL(p);

    xr_repl_print_vars(iso);

    xr_free_code(iso, p);
    xray_vm_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_type_null_and_empty_safe) {
    /* xr_repl_print_type tolerates NULL / "" / whitespace-only input
     * without invoking the compiler. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    xr_repl_print_type(iso, NULL, &k_repl_memory_authority);
    xr_repl_print_type(iso, "", &k_repl_memory_authority);
    xr_repl_print_type(iso, "   \t  ", &k_repl_memory_authority);

    xray_vm_delete(iso);
    ASSERT_TRUE(1);
}

TEST(repl_print_type_simple_expression) {
    /* Driving .type through the API end-to-end exercises:
     * synthesize source → xr_repl_eval. Capturing
     * stdout here also prevents the helper from regressing to the
     * removed `typename(...)` spelling, which compiles to no output. */
    XrVMRuntime *iso = make_repl_iso();
    ASSERT_NOT_NULL(iso);

    FILE *out = tmpfile();
    ASSERT_NOT_NULL(out);
    xr_isolate_set_stdout(iso, out);
    xr_repl_print_type(iso, "1 + 2", &k_repl_memory_authority);

    char buf[64];
    read_tmp_output(out, buf, sizeof(buf));
    ASSERT_NOT_NULL(strstr(buf, "i64"));
    fclose(out);
    xray_vm_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Globals Dict");
RUN_TEST(product_profiles_install_exact_authority_and_reject_missing_profile);
RUN_TEST(globals_dict_set_get_round_trip);
RUN_TEST(globals_dict_overwrite_keeps_count);
RUN_TEST(globals_dict_missing_key_returns_null);
RUN_TEST(sync_root_elides_coroutine_with_ordinary_allocation);

RUN_TEST_SUITE("REPL Profile");
RUN_TEST(repl_profile_clears_each_call);

RUN_TEST_SUITE("REPL Symbol Table");
RUN_TEST(repl_symbols_new_and_free);
RUN_TEST(repl_symbols_free_null_is_noop);
RUN_TEST(repl_symbols_of_null_isolate);
RUN_TEST(repl_symbol_cname_null_safety);

RUN_TEST_SUITE("REPL Incremental Evaluation");
RUN_TEST(repl_eval_let_registers_symbol);
RUN_TEST(repl_eval_const_marks_is_const);
RUN_TEST(repl_eval_function_registers_symbol);
RUN_TEST(repl_eval_let_and_const_round_trip);

RUN_TEST_SUITE("REPL Cross-Input Persistence");
RUN_TEST(repl_cross_input_symbol_resolves);
RUN_TEST(repl_cross_input_function_call);
RUN_TEST(repl_cross_input_call_rejects_missing_type_authority);
RUN_TEST(repl_cross_input_call_rejects_invalid_symbol_authority);
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
RUN_TEST(repl_it_can_change_static_type);
RUN_TEST(repl_unit_result_does_not_replace_it);
RUN_TEST(repl_it_reference_is_a_typed_snapshot);
RUN_TEST(repl_unit_before_value_does_not_create_it);
RUN_TEST(repl_it_is_reserved_for_implicit_results);
RUN_TEST(repl_eval_requires_explicit_valid_memory_identity);
RUN_TEST(repl_auto_echo_evaluates_expression_once);

RUN_TEST_SUITE("REPL Introspection");
RUN_TEST(repl_print_vars_empty_is_safe);
RUN_TEST(repl_print_vars_after_compile_no_crash);
RUN_TEST(repl_print_type_null_and_empty_safe);
RUN_TEST(repl_print_type_simple_expression);
TEST_MAIN_END()
