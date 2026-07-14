/*
 * test_analyzer.c - Unit tests for static type analyzer
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Only include analyzer headers (avoid GC type conflicts)
#include "xtype.h"
#include "xanalyzer_symbol.h"
#include "xanalyzer.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_flow.h"
#include "xanalyzer_infer.h"
#include "xanalyzer_visitor.h"
#include "xast_nodes.h"
#include "xparse.h"
#include "xtype_ref.h"
#include "xtype_ref_resolve.h"
#include "xtype_pool.h"
#include "xhashmap.h"
#include "xarena.h"
#include "xray_vm.h"
#include "module/xmodule_graph.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

// Global isolate and analyzer for pool initialization
static XrVMRuntime *g_isolate = NULL;
static XrCompilerSession *g_session = NULL;
static XaAnalyzer *g_analyzer = NULL;

static void setup_pool(void) {
    if (!g_isolate) {
        XrVMConfig p;
        xray_vm_config_init(&p);
        g_isolate = xray_vm_new(&p);
        XrCompilerSessionConfig cfg = {.vm_host = g_isolate};
        g_session = xr_compiler_session_new(&cfg);
        xr_compiler_session_attach_isolate(g_isolate, g_session);
    }
    if (!g_analyzer) {
        g_analyzer = xa_analyzer_new(g_session);
    }
    // Ensure thread-local pool and symbol ID counter are set (even if g_analyzer
    // already exists, a test may have overwritten them with its own pool)
    xr_type_set_current_pool(g_analyzer->type_pool, &g_analyzer->type_pool->next_type_id);
    xa_symbol_set_id_counter(&g_analyzer->next_symbol_id);
}

static void teardown_pool(void) {
    if (g_analyzer) {
        xa_analyzer_free(g_analyzer);
        g_analyzer = NULL;
    }
    if (g_isolate) {
        if (g_session) {
            xr_compiler_session_delete(g_session);
            g_session = NULL;
        }
        xray_vm_delete(g_isolate);
        g_isolate = NULL;
    }
}

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Running %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASSED\n");                                                                        \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

// ============================================================================
// Type tests
// ============================================================================

TEST(type_primitives) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_string = xr_type_new_string(NULL);
    XrType *t_bool = xr_type_new_bool(NULL);
    XrType *t_null = xr_type_new_null(NULL);

    ASSERT(XR_TYPE_IS_INT(t_int));
    ASSERT(XR_TYPE_IS_FLOAT(t_float));
    ASSERT(XR_TYPE_IS_STRING(t_string));
    ASSERT(XR_TYPE_IS_BOOL(t_bool));
    ASSERT(XR_TYPE_IS_NULL(t_null));

    ASSERT(XR_TYPE_IS_NUMERIC(t_int));
    ASSERT(XR_TYPE_IS_NUMERIC(t_float));
    ASSERT(!XR_TYPE_IS_NUMERIC(t_string));

    ASSERT(XR_TYPE_IS_PRIMITIVE(t_int));
    ASSERT(XR_TYPE_IS_PRIMITIVE(t_string));
    ASSERT(!XR_TYPE_IS_PRIMITIVE(t_null));
}

TEST(type_containers) {
    XrType *elem = xr_type_new_int(NULL);
    XrType *arr = xr_type_new_array(g_isolate, elem);

    ASSERT(XR_TYPE_IS_ARRAY(arr));
    ASSERT(arr->container.element_type == elem);

    XrType *key = xr_type_new_string(NULL);
    XrType *val = xr_type_new_int(NULL);
    XrType *map = xr_type_new_map(g_isolate, key, val);

    ASSERT(XR_TYPE_IS_MAP(map));
    ASSERT(map->map.key_type == key);
    ASSERT(map->map.value_type == val);
}

TEST(type_union) {
    // Test 1: T | null = T? (nullable type)
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_null = xr_type_new_null(NULL);
    XrType *nullable_int = xr_type_union(g_isolate, t_int, t_null);
    ASSERT(nullable_int != NULL);
    ASSERT(nullable_int->is_nullable);
    ASSERT(XR_TYPE_IS_INT(nullable_int));

    // Test 2: int | string = union type
    XrType *t_string = xr_type_new_string(NULL);
    XrType *union_type = xr_type_union(g_isolate, t_int, t_string);
    ASSERT(union_type != NULL);
    ASSERT(XR_TYPE_IS_UNION(union_type));

    // Test 3: Same types = same type
    XrType *t_int2 = xr_type_new_int(NULL);
    XrType *same = xr_type_union(g_isolate, t_int, t_int2);
    ASSERT(XR_TYPE_IS_INT(same));
}

TEST(type_error_recovery) {
    XrType *t_error = xr_type_new_error(NULL);
    ASSERT(t_error != NULL);
    ASSERT(XR_TYPE_IS_ERROR(t_error));
    ASSERT(XR_TYPE_IS_UNKNOWN_OR_ERROR(t_error));
    ASSERT(!XR_TYPE_IS_UNKNOWN(t_error));
    ASSERT(strcmp(xr_type_to_string(t_error), "<error>") == 0);
    ASSERT(xr_type_equals(t_error, xr_type_new_error(NULL)));
    ASSERT(!xr_type_equals(t_error, xr_type_new_unknown(NULL)));

    XrTypeRef error_ref = {.kind = XR_TREF_ERROR};
    XrType *resolved = xr_tref_resolve(g_isolate, &error_ref);
    ASSERT(resolved != NULL);
    ASSERT(XR_TYPE_IS_ERROR(resolved));
    ASSERT(resolved == t_error);

    XrType *poisoned = xr_type_union(g_isolate, xr_type_new_int(NULL), t_error);
    ASSERT(poisoned != NULL);
    ASSERT(XR_TYPE_IS_ERROR(poisoned));
}

TEST(type_assignable) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_unknown = xr_type_new_unknown(NULL);
    XrType *t_never = xr_type_new_never(NULL);

    // int assignable to int
    ASSERT(xr_type_assignable(t_int, t_int));

    // int assignable to float (numeric coercion)
    ASSERT(xr_type_assignable(t_float, t_int));

    // Internal lattice keeps unknown as a permissive top type.
    ASSERT(xr_type_assignable(t_unknown, t_int));

    // never assignable to anything
    ASSERT(xr_type_assignable(t_int, t_never));
}

TEST(typecheck_assignable_rejects_unknown_source) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_unknown = xr_type_new_unknown(NULL);

    ASSERT(!xa_typecheck_assignable(t_int, t_unknown));
    ASSERT(xa_typecheck_assignable(t_unknown, t_int));
}

TEST(typecheck_assignable_rejects_unknown_container_member) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *target = xr_type_new_array(g_isolate, t_int);
    XrType *source = xr_type_new_array(g_isolate, xr_type_new_unknown(NULL));

    ASSERT(xr_type_assignable(target, source));
    ASSERT(xa_typecheck_assignable(target, source));
}

TEST(analyzer_check_assignment_rejects_unknown_source) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_unknown = xr_type_new_unknown(NULL);
    XrLocation loc = {.file = "test.xr", .line = 1, .column = 1};

    xa_analyzer_clear_diagnostics(g_analyzer);
    ASSERT(!xa_analyzer_check_assignment(g_analyzer, t_int, t_unknown, &loc));

    int count = 0;
    XaDiagnostic *diag = xa_analyzer_get_diagnostics(g_analyzer, &count);
    ASSERT(count == 1);
    ASSERT(diag != NULL);
    ASSERT(diag->code == XR_ERR_ANALYZE_TYPE_MISMATCH);

    xa_analyzer_clear_diagnostics(g_analyzer);
}

TEST(type_to_string) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_u8 = xr_type_new_int_width(NULL, XR_NATIVE_U8);
    XrType *t_u64 = xr_type_new_int_width(NULL, XR_NATIVE_U64);
    XrType *t_arr = xr_type_new_array(g_isolate, xr_type_new_string(NULL));
    XrType *t_byte_arr = xr_type_new_array(g_isolate, t_u8);
    XrType *t_byte_slice = xr_type_new_span(g_isolate, t_u8);
    XrType *fn_params[] = {xr_type_new_int_width(NULL, XR_NATIVE_I32)};
    XrType *t_cfn = xr_type_new_function(g_isolate, fn_params, 1,
                                         xr_type_new_int_width(NULL, XR_NATIVE_I32), false);
    t_cfn->function.is_c_abi = true;
    XrType *byte_fn_params[] = {t_u8};
    XrType *t_byte_fn = xr_type_new_function(g_isolate, byte_fn_params, 1, t_u8, false);

    ASSERT(strcmp(xr_type_to_string(t_int), "int") == 0);
    ASSERT(strcmp(xr_type_to_string(t_u8), "byte") == 0);
    ASSERT(strcmp(xr_type_to_string(t_u64), "uint64") == 0);
    ASSERT(strcmp(xr_type_to_string(t_arr), "Array<string>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_arr), "Array<byte>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_slice), "Slice<byte>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_cfn), "CFn<fn(int32): int32>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_fn), "fn(byte): byte") == 0);
}

TEST(type_narrowing) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_null = xr_type_new_null(NULL);
    XrType *u = xr_type_union(g_isolate, t_int, t_null);

    // Filter to int only
    XrType *filtered = xr_type_filter(g_isolate, u, XR_KIND_INT);
    ASSERT(XR_TYPE_IS_INT(filtered));
    ASSERT(!XR_TYPE_IS_NULL(filtered));

    // Exclude null
    XrType *non_null = xr_type_non_nullable(g_isolate, u);
    ASSERT(XR_TYPE_IS_INT(non_null));
}

// ============================================================================
// Symbol tests
// ============================================================================

TEST(symbol_create) {
    XaSymbol *sym = xa_symbol_new("myVar", XA_SYM_VARIABLE);

    ASSERT(sym != NULL);
    ASSERT(strcmp(sym->name, "myVar") == 0);
    ASSERT(sym->kind == XA_SYM_VARIABLE);
    ASSERT(sym->id > 0);

    xa_symbol_free(sym);
}

TEST(scope_basic) {
    XaScope *global = xa_scope_new(XA_SCOPE_GLOBAL, NULL);
    ASSERT(global != NULL);
    ASSERT(global->kind == XA_SCOPE_GLOBAL);
    ASSERT(global->parent == NULL);

    XaScope *func = xa_scope_new(XA_SCOPE_FUNCTION, global);
    ASSERT(func->parent == global);

    xa_scope_free(global);  // Also frees func
}

TEST(scope_lookup) {
    XaScope *global = xa_scope_new(XA_SCOPE_GLOBAL, NULL);
    XaScope *func = xa_scope_new(XA_SCOPE_FUNCTION, global);

    XaSymbol *x = xa_symbol_new("x", XA_SYM_VARIABLE);
    XaSymbol *y = xa_symbol_new("y", XA_SYM_VARIABLE);

    xa_scope_add_symbol(global, x);
    xa_scope_add_symbol(func, y);

    // y visible in func scope
    ASSERT(xa_scope_lookup(func, "y") == y);

    // x visible in func scope (from parent)
    ASSERT(xa_scope_lookup(func, "x") == x);

    // y not visible in global scope
    ASSERT(xa_scope_lookup(global, "y") == NULL);

    xa_scope_free(global);
}

// ============================================================================
// Analyzer tests
// ============================================================================

TEST(analyzer_create) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    ASSERT(a->global_scope != NULL);
    ASSERT(a->current_scope == a->global_scope);
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count == 0);

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(analyzer_diagnostics) {
    XaAnalyzer *a = xa_analyzer_new(g_session);

    XrLocation loc = {.file = "test.xr", .line = 10, .column = 5};
    xa_analyzer_add_diagnostic(a, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR, "Test error",
                               &loc);

    int count;
    XaDiagnostic *diags = xa_analyzer_get_diagnostics(a, &count);

    ASSERT(count == 1);
    ASSERT(diags != NULL);
    ASSERT(diags->severity == XR_DIAG_SEV_ERROR);
    ASSERT(diags->code == XR_ERR_ANALYZE_UNDEFINED_VAR);

    xa_analyzer_clear_diagnostics(a);
    diags = xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(analyzer_type_telemetry_splits_unknown_and_error) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *missing_program = xr_parse(g_session, "missing;");
    ASSERT(missing_program != NULL);
    xa_analyzer_analyze(a, "telemetry_unresolved_inference.xr", missing_program);
    ASSERT(a->unresolved_inference_count >= 1);
    ASSERT(a->recovery_poison_type_count == 0);
    xa_analyzer_free(a);

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(
        xr_compiler_session_push_arena(g_session, &arena, "telemetry_error_recovery.xr", &scope));
    AstNode *error_expr = xr_ast_as_expr(g_session, xr_ast_literal_int(g_session, 1, 1),
                                         xr_tref_error(g_session), false, 1);
    ASSERT(error_expr != NULL);
    XrType *error_type = xa_analyzer_infer_expr_type(a, error_expr);
    ASSERT(error_type != NULL);
    ASSERT(XR_TYPE_IS_ERROR(error_type));
    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(analyzer_scope_management) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaScope *global = a->current_scope;

    xa_analyzer_enter_scope(a, XA_SCOPE_FUNCTION, NULL);
    ASSERT(a->current_scope != global);
    ASSERT(a->current_scope->parent == global);

    xa_analyzer_exit_scope(a);
    ASSERT(a->current_scope == global);

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

// ============================================================================
// Flow analysis tests
// ============================================================================

TEST(flow_builder_create) {
    XaFlowBuilder *fb = xa_flow_builder_new();
    ASSERT(fb != NULL);
    ASSERT(fb->unreachable_flow != NULL);

    xa_flow_builder_free(fb);
}

TEST(flow_basic_graph) {
    XaFlowBuilder *fb = xa_flow_builder_new();

    XaFlowNode *start = xa_flow_create_start(fb);
    ASSERT(start != NULL);
    ASSERT(start->flags & XA_FLOW_START);

    XaFlowNode *assign = xa_flow_create_assignment(fb, NULL, "x", xr_type_new_int(NULL));
    ASSERT(assign != NULL);
    ASSERT(assign->flags & XA_FLOW_ASSIGNMENT);
    ASSERT(assign->antecedent_count == 1);
    ASSERT(assign->antecedents[0] == start);

    xa_flow_builder_free(fb);
}

TEST(flow_condition_branches) {
    XaFlowBuilder *fb = xa_flow_builder_new();

    xa_flow_create_start(fb);

    // Create true and false branches
    XaFlowNode *true_branch = xa_flow_create_condition(fb, NULL, true);
    XaFlowNode *false_branch = xa_flow_create_condition(fb, NULL, false);

    ASSERT(true_branch->flags & XA_FLOW_TRUE_CONDITION);
    ASSERT(false_branch->flags & XA_FLOW_FALSE_CONDITION);

    // Create merge point
    XaFlowNode *merge = xa_flow_create_branch_label(fb);
    xa_flow_add_antecedent(merge, true_branch);
    xa_flow_add_antecedent(merge, false_branch);

    ASSERT(merge->antecedent_count == 2);

    xa_flow_builder_free(fb);
}

TEST(flow_cache) {
    XaFlowCache *cache = xa_flow_cache_new();
    ASSERT(cache != NULL);

    XaFlowBuilder *fb = xa_flow_builder_new();
    XaFlowNode *node = xa_flow_create_start(fb);
    XrType *type = xr_type_new_int(NULL);

    xa_flow_cache_set(cache, node, type);

    XrType *got = xa_flow_cache_get(cache, node);
    ASSERT(got == type);

    xa_flow_cache_clear(cache);
    ASSERT(xa_flow_cache_get(cache, node) == NULL);

    xa_flow_builder_free(fb);
    xa_flow_cache_free(cache);
}

TEST(narrow_by_typeof) {
    // NOTE: xray now only supports nullable types (T | null = T?), not general unions.
    // Nullable types use is_nullable flag, not XR_KIND_NULL in flags.

    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_null = xr_type_new_null(NULL);

    // Create nullable int (int | null = int?)
    XrType *nullable_int = xr_type_union(g_isolate, t_int, t_null);
    ASSERT(nullable_int != NULL);
    ASSERT(nullable_int->is_nullable);

    // typeof x === "int" on nullable int -> int
    XrType *narrowed = xa_narrow_by_typeof(nullable_int, "int", true);
    ASSERT(XR_TYPE_IS_INT(narrowed));

    // typeof narrowing on pure null type
    XrType *narrowed_null = xa_narrow_by_typeof(t_null, "null", true);
    ASSERT(XR_TYPE_IS_NULL(narrowed_null));

    // typeof narrowing on pure int type
    XrType *narrowed_int = xa_narrow_by_typeof(t_int, "int", true);
    ASSERT(XR_TYPE_IS_INT(narrowed_int));

    // typeof x !== "int" on int -> never (no other type remaining)
    XrType *excluded_int = xa_narrow_by_typeof(t_int, "int", false);
    ASSERT(XR_TYPE_IS_NEVER(excluded_int));

    // NOTE: 'any' type is a special marker type (XR_KIND_ANY flag only),
    // not a union of all types. Typeof narrowing on 'any' returns 'never'
    // because any doesn't have specific type flags.
}

TEST(narrow_by_null) {
    // NOTE: xray now uses nullable types (T?) instead of union (T | null).
    // xr_type_union(int, null) returns a nullable int (is_nullable = true).

    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_null = xr_type_new_null(NULL);
    XrType *nullable_int = xr_type_union(g_isolate, t_int, t_null);

    // Verify it's a nullable int
    ASSERT(nullable_int != NULL);
    ASSERT(nullable_int->is_nullable || (nullable_int->kind == XR_KIND_NULL));

    // x == null && true -> null (filter to only null)
    XrType *is_null = xa_narrow_by_null_check(nullable_int, true, true);
    // When narrowing nullable to null, we get the null type
    ASSERT(is_null != NULL);
    ASSERT(XR_TYPE_IS_NULL(is_null) || XR_TYPE_IS_NEVER(is_null));

    // x != null && true -> int (non-null part)
    XrType *not_null = xa_narrow_by_null_check(nullable_int, false, true);
    ASSERT(not_null != NULL);
    // For nullable int, non-null part should be int
    ASSERT(XR_TYPE_IS_INT(not_null));
    ASSERT(!not_null->is_nullable);
}

// ============================================================================
// Additional type tests
// ============================================================================

TEST(type_class_instance) {
    XrType *cls = xr_type_new_class(g_isolate, "MyClass");
    ASSERT(XR_TYPE_IS_CLASS(cls));
    ASSERT(cls->instance.class_name != NULL);
    ASSERT(strcmp(cls->instance.class_name, "MyClass") == 0);

    // Instance type requires class info
    XrClassInfo *info = xa_class_info_new("TestClass");
    XrType *inst = xr_type_new_instance(g_isolate, info);
    ASSERT(XR_TYPE_IS_INSTANCE(inst));
    xa_class_info_free(info);
}

TEST(type_function_complex) {
    // (int, string) -> Array<int>
    XrType *param1 = xr_type_new_int(NULL);
    XrType *param2 = xr_type_new_string(NULL);
    XrType *ret = xr_type_new_array(g_isolate, xr_type_new_int(NULL));

    XrType *params[] = {param1, param2};
    XrType *fn = xr_type_new_function(g_isolate, params, 2, ret, false);

    ASSERT(XR_TYPE_IS_FUNCTION(fn));
    ASSERT(fn->function.param_count == 2);
    ASSERT(XR_TYPE_IS_INT(xr_type_function_param_type(fn, 0)));
    ASSERT(XR_TYPE_IS_STRING(xr_type_function_param_type(fn, 1)));
    ASSERT(XR_TYPE_IS_ARRAY(fn->function.return_type));
}

TEST(type_void_never) {
    XrType *t_void = xr_type_new_unit(NULL);
    XrType *t_never = xr_type_new_never(NULL);

    ASSERT(XR_TYPE_IS_UNIT(t_void));
    ASSERT(XR_TYPE_IS_NEVER(t_never));

    // never is assignable to anything
    ASSERT(xr_type_assignable(xr_type_new_int(NULL), t_never));
}

TEST(type_rejects_invalid_counts) {
    XrType *param_types[] = {xr_type_new_int(NULL)};
    const char *field_names[] = {"value"};
    XrType *field_types[] = {xr_type_new_string(NULL)};

    ASSERT(xr_type_new_function(g_isolate, param_types, -1, xr_type_new_unit(NULL), false) == NULL);
    ASSERT(xr_type_new_function(g_isolate, NULL, 1, xr_type_new_unit(NULL), false) == NULL);
    ASSERT(xr_type_new_generic_instance(g_isolate, "Box", NULL, NULL, 1) == NULL);
    ASSERT(xr_type_new_tuple(g_isolate, NULL, 1) == NULL);
    ASSERT(xr_type_new_tuple(g_isolate, param_types, -1) == NULL);
    ASSERT(xr_type_new_json_with_fields(g_isolate, NULL, field_types, 1, false) == NULL);
    ASSERT(xr_type_new_json_with_fields(g_isolate, field_names, NULL, 1, false) == NULL);
}

TEST(type_function_copy_preserves_metadata) {
    XrType *param_types[] = {xr_type_new_int(NULL), xr_type_new_string(NULL)};
    XrType *fn = xr_type_new_function(g_isolate, param_types, 2, xr_type_new_bool(NULL), false);
    ASSERT(fn != NULL);

    fn->function.min_params = 1;
    ASSERT(xr_type_function_set_param_mode(fn, 0, XR_PARAM_IN));
    ASSERT(xr_type_function_set_param_mode(fn, 1, XR_PARAM_REF));
    fn->function.is_c_abi = true;

    XrType *copy = xr_type_copy(g_isolate, fn);
    ASSERT(copy != NULL);
    ASSERT(copy != fn);
    ASSERT(copy->function.param_count == 2);
    ASSERT(copy->function.min_params == 1);
    ASSERT(copy->function.params != fn->function.params);
    ASSERT(xr_type_function_param_type(copy, 0) == xr_type_function_param_type(fn, 0));
    ASSERT(xr_type_function_param_type(copy, 1) == xr_type_function_param_type(fn, 1));
    ASSERT(xr_type_function_param_mode(copy, 0) == XR_PARAM_IN);
    ASSERT(xr_type_function_param_mode(copy, 1) == XR_PARAM_REF);
    ASSERT(copy->function.is_c_abi);

    XrType *normal = xr_type_new_function(g_isolate, param_types, 2, xr_type_new_bool(NULL), false);
    ASSERT(!xr_type_equals(fn, normal));
    ASSERT(!xr_type_assignable(fn, normal));
    ASSERT(!xr_type_assignable(normal, fn));
}

// ============================================================================
// Inference context tests
// ============================================================================

TEST(infer_context_create) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaInferContext *ctx = xa_infer_context_new(a);

    ASSERT(ctx != NULL);
    ASSERT(ctx->analyzer == a);
    ASSERT(ctx->flow != NULL);
    ASSERT(ctx->cache != NULL);
    ASSERT(ctx->return_type_count == 0);

    xa_infer_context_free(ctx);
    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(infer_return_type_collection) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaInferContext *ctx = xa_infer_context_new(a);

    // Add multiple return types
    xa_infer_add_return_type(ctx, xr_type_new_int(NULL));
    xa_infer_add_return_type(ctx, xr_type_new_string(NULL));

    ASSERT(ctx->return_type_count == 2);

    // Compute union of return types
    // NOTE: xray doesn't support general union types (int | string).
    // Non-nullable unions degrade to 'any', so the result should be 'any'.
    XrType *ret = xa_infer_compute_return_type(ctx);
    ASSERT(ret != NULL);
    // int | string -> union type
    ASSERT(XR_TYPE_IS_UNION(ret));

    xa_infer_context_free(ctx);
    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(infer_single_return_type) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaInferContext *ctx = xa_infer_context_new(a);

    xa_infer_add_return_type(ctx, xr_type_new_int(NULL));

    XrType *ret = xa_infer_compute_return_type(ctx);
    ASSERT(XR_TYPE_IS_INT(ret));

    xa_infer_context_free(ctx);
    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(infer_no_return_type) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaInferContext *ctx = xa_infer_context_new(a);

    // No return types added -> unit (0-arity tuple)
    XrType *ret = xa_infer_compute_return_type(ctx);
    ASSERT(XR_TYPE_IS_UNIT(ret));

    xa_infer_context_free(ctx);
    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

// ============================================================================
// Compile type conversion tests
// ============================================================================

TEST(compile_type_primitives) {
    // Test primitive types using new XrType API
    ASSERT(XR_TYPE_IS_INT(xr_type_new_int(NULL)));
    ASSERT(XR_TYPE_IS_FLOAT(xr_type_new_float(NULL)));
    ASSERT(XR_TYPE_IS_STRING(xr_type_new_string(NULL)));
    ASSERT(XR_TYPE_IS_BOOL(xr_type_new_bool(NULL)));
    ASSERT(XR_TYPE_IS_NULL(xr_type_new_null(NULL)));
    ASSERT(XR_TYPE_IS_UNIT(xr_type_new_unit(NULL)));
}

TEST(compile_type_containers) {
    // Array<int> using new API
    XrType *arr = xr_type_new_array(g_analyzer->isolate, xr_type_new_int(NULL));
    ASSERT(XR_TYPE_IS_ARRAY(arr));
    ASSERT(arr->container.element_type != NULL);
    ASSERT(XR_TYPE_IS_INT(arr->container.element_type));

    // Map<string, int> using new API
    XrType *map =
        xr_type_new_map(g_analyzer->isolate, xr_type_new_string(NULL), xr_type_new_int(NULL));
    ASSERT(XR_TYPE_IS_MAP(map));
    ASSERT(XR_TYPE_IS_STRING(map->map.key_type));
    ASSERT(XR_TYPE_IS_INT(map->map.value_type));
}

TEST(compile_type_function) {
    // (int, string) -> bool using new API
    XrType *param_types[] = {xr_type_new_int(NULL), xr_type_new_string(NULL)};
    XrType *fn =
        xr_type_new_function(g_analyzer->isolate, param_types, 2, xr_type_new_bool(NULL), false);
    ASSERT(XR_TYPE_IS_FUNCTION(fn));
    ASSERT(fn->function.param_count == 2);
    ASSERT(XR_TYPE_IS_INT(xr_type_function_param_type(fn, 0)));
    ASSERT(XR_TYPE_IS_STRING(xr_type_function_param_type(fn, 1)));
    ASSERT(XR_TYPE_IS_BOOL(fn->function.return_type));
}

TEST(compile_type_ref_function_modes) {
    AstNode *program = xr_parse(g_session, "type Handler = (in int, ref string, out bool) -> int");
    ASSERT(program != NULL);
    ASSERT(program->type == AST_PROGRAM);
    ASSERT(program->as.program.count == 1);
    AstNode *alias = program->as.program.statements[0];
    ASSERT(alias != NULL);
    ASSERT(alias->type == AST_TYPE_ALIAS);
    XrTypeRef *tref = alias->as.type_alias.resolved_type;
    ASSERT(tref != NULL);

    XrType *fn = xr_tref_resolve_in_analyzer(g_analyzer, tref);
    ASSERT(XR_TYPE_IS_FUNCTION(fn));
    ASSERT(fn->function.param_count == 3);
    ASSERT(XR_TYPE_IS_INT(xr_type_function_param_type(fn, 0)));
    ASSERT(XR_TYPE_IS_STRING(xr_type_function_param_type(fn, 1)));
    ASSERT(XR_TYPE_IS_BOOL(xr_type_function_param_type(fn, 2)));
    ASSERT(xr_type_function_param_mode(fn, 0) == XR_PARAM_IN);
    ASSERT(xr_type_function_param_mode(fn, 1) == XR_PARAM_REF);
    ASSERT(xr_type_function_param_mode(fn, 2) == XR_PARAM_OUT);
    ASSERT(XR_TYPE_IS_INT(fn->function.return_type));
}

static bool analyzer_diag_contains(XaAnalyzer *analyzer, const char *needle) {
    int count = 0;
    XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &count);
    for (; diag; diag = diag->next) {
        if (diag->message && strstr(diag->message, needle))
            return true;
    }
    return false;
}

static const XaEffectSummary *analyzer_function_effect_summary(XaAnalyzer *analyzer,
                                                               const char *name) {
    XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, name);
    if (!sym || sym->links.effect_id == XA_EFFECT_NONE)
        return NULL;
    return xa_effect_db_get(analyzer->effect_db, sym->links.effect_id);
}

static bool effect_summary_has_enum_named(XaAnalyzer *analyzer, const XaEffectSummary *summary,
                                          const char *name) {
    if (!analyzer || !summary || !name)
        return false;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        XrType *type =
            xa_effect_db_error_type_handle(analyzer->effect_db, summary->escaping.types[i].type_id);
        if (type && XR_TYPE_IS_ENUM(type) && type->enum_type.enum_name &&
            strcmp(type->enum_type.enum_name, name) == 0)
            return true;
    }
    return false;
}

static const XaErrorTypeSet *effect_summary_enum_set_named(XaAnalyzer *analyzer,
                                                           const XaEffectSummary *summary,
                                                           const char *name) {
    if (!analyzer || !summary || !name)
        return NULL;
    for (uint32_t i = 0; i < summary->escaping.count; i++) {
        XrType *type =
            xa_effect_db_error_type_handle(analyzer->effect_db, summary->escaping.types[i].type_id);
        if (type && XR_TYPE_IS_ENUM(type) && type->enum_type.enum_name &&
            strcmp(type->enum_type.enum_name, name) == 0)
            return &summary->escaping.types[i];
    }
    return NULL;
}

TEST(analyzer_error_effect_records_direct_throw_variant) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum DirectErr { First, Second, Payload(code: int), Third }\n"
                         "enum PayloadArgErr { Boom }\n"
                         "fn throwsSecond() { throw DirectErr.Second }\n"
                         "fn throwsPayload() { throw DirectErr.Payload(1) }\n"
                         "fn payloadCode() -> int { throw PayloadArgErr.Boom\n"
                         "  return 1 }\n"
                         "fn throwsPayloadArg() { throw DirectErr.Payload(payloadCode()) }\n"
                         "fn throwsVariable(e: DirectErr) { throw e }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_direct_throw_variant.xr", program);

    const XaEffectSummary *specific = analyzer_function_effect_summary(a, "throwsSecond");
    const XaEffectSummary *payload = analyzer_function_effect_summary(a, "throwsPayload");
    const XaEffectSummary *payload_arg = analyzer_function_effect_summary(a, "throwsPayloadArg");
    const XaEffectSummary *all = analyzer_function_effect_summary(a, "throwsVariable");
    ASSERT(specific != NULL);
    ASSERT(payload != NULL);
    ASSERT(payload_arg != NULL);
    ASSERT(all != NULL);

    const XaErrorTypeSet *specific_set = effect_summary_enum_set_named(a, specific, "DirectErr");
    ASSERT(specific_set != NULL);
    ASSERT(!specific_set->all_variants);
    ASSERT(!xa_bitset_test(&specific_set->variants, 0));
    ASSERT(xa_bitset_test(&specific_set->variants, 1));
    ASSERT(!xa_bitset_test(&specific_set->variants, 2));
    ASSERT(!xa_bitset_test(&specific_set->variants, 3));

    const XaErrorTypeSet *payload_set = effect_summary_enum_set_named(a, payload, "DirectErr");
    ASSERT(payload_set != NULL);
    ASSERT(!payload_set->all_variants);
    ASSERT(!xa_bitset_test(&payload_set->variants, 0));
    ASSERT(!xa_bitset_test(&payload_set->variants, 1));
    ASSERT(xa_bitset_test(&payload_set->variants, 2));
    ASSERT(!xa_bitset_test(&payload_set->variants, 3));

    const XaErrorTypeSet *payload_arg_set =
        effect_summary_enum_set_named(a, payload_arg, "DirectErr");
    ASSERT(payload_arg_set != NULL);
    ASSERT(!payload_arg_set->all_variants);
    ASSERT(!xa_bitset_test(&payload_arg_set->variants, 0));
    ASSERT(!xa_bitset_test(&payload_arg_set->variants, 1));
    ASSERT(xa_bitset_test(&payload_arg_set->variants, 2));
    ASSERT(!xa_bitset_test(&payload_arg_set->variants, 3));
    ASSERT(effect_summary_has_enum_named(a, payload_arg, "PayloadArgErr"));

    const XaErrorTypeSet *all_set = effect_summary_enum_set_named(a, all, "DirectErr");
    ASSERT(all_set != NULL);
    ASSERT(all_set->all_variants);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_const_function_value_aliases) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum AliasErr { Boom }\n"
                         "fn failAlias() { throw AliasErr.Boom }\n"
                         "fn noThrowAlias() { }\n"
                         "fn viaConstAlias() {\n"
                         "  const f = failAlias\n"
                         "  f()\n"
                         "}\n"
                         "fn viaConstAliasChain() {\n"
                         "  const f = failAlias\n"
                         "  const g = (f)\n"
                         "  g()\n"
                         "}\n"
                         "fn sameLocalNameDoesNotLeak() {\n"
                         "  const f = noThrowAlias\n"
                         "  f()\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_const_function_alias.xr", program);

    const XaEffectSummary *direct = analyzer_function_effect_summary(a, "viaConstAlias");
    const XaEffectSummary *chain = analyzer_function_effect_summary(a, "viaConstAliasChain");
    const XaEffectSummary *same_name =
        analyzer_function_effect_summary(a, "sameLocalNameDoesNotLeak");
    ASSERT(direct != NULL);
    ASSERT(chain != NULL);
    ASSERT(same_name != NULL);

    ASSERT(effect_summary_has_enum_named(a, direct, "AliasErr"));
    ASSERT(effect_summary_has_enum_named(a, chain, "AliasErr"));
    ASSERT(!effect_summary_has_enum_named(a, same_name, "AliasErr"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_stable_var_function_values) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source =
        "enum DynamicErr { Boom }\n"
        "enum OtherDynamicErr { Boom }\n"
        "fn failDynamic() { throw DynamicErr.Boom }\n"
        "fn failOtherDynamic() { throw OtherDynamicErr.Boom }\n"
        "fn noThrowDynamic() { }\n"
        "fn maybeDynamic(flag: bool) { if (flag) { throw DynamicErr.Boom } }\n"
        "fn chooseDynamic(flag: bool) -> () -> () {\n"
        "  if (flag) { return failDynamic }\n"
        "  return failOtherDynamic\n"
        "}\n"
        "fn chooseNoThrowOrThrow(flag: bool) -> () -> () {\n"
        "  if (flag) { return failDynamic }\n"
        "  return noThrowDynamic\n"
        "}\n"
        "fn chooseUnknownDynamic(cb: () -> ()) -> () -> () {\n"
        "  return cb\n"
        "}\n"
        "fn makeCapturedFunctionValue(flag: bool) -> () -> () {\n"
        "  var f = noThrowDynamic\n"
        "  if (flag) { f = failDynamic } else { f = failOtherDynamic }\n"
        "  return fn() { f() }\n"
        "}\n"
        "fn makeCapturedUnknownFunctionValue(cb: () -> ()) -> () -> () {\n"
        "  var f = failDynamic\n"
        "  f = cb\n"
        "  return fn() { f() }\n"
        "}\n"
        "fn invokeCallback(cb: () -> ()) {\n"
        "  cb()\n"
        "}\n"
        "fn chooseCallback(flag: bool, a: () -> (), b: () -> ()) {\n"
        "  var cb = a\n"
        "  if (flag) { cb = b }\n"
        "  cb()\n"
        "}\n"
        "fn viaStableVarAlias() {\n"
        "  var f = failDynamic\n"
        "  f()\n"
        "}\n"
        "fn viaStableVarAliasChain() {\n"
        "  var f = failDynamic\n"
        "  var g = f\n"
        "  g()\n"
        "}\n"
        "fn viaReboundVarAlias() {\n"
        "  var f = failDynamic\n"
        "  f = noThrowDynamic\n"
        "  f()\n"
        "}\n"
        "fn viaReboundVarAliasToThrowing() {\n"
        "  var f = noThrowDynamic\n"
        "  f = failDynamic\n"
        "  f()\n"
        "}\n"
        "fn viaUnknownReboundVarAlias(cb: () -> ()) {\n"
        "  var f = failDynamic\n"
        "  f = cb\n"
        "  f()\n"
        "}\n"
        "fn viaConditionalReboundVarAlias(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  if (flag) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaIfElseTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  if (flag) { f = failDynamic } else { f = failOtherDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaConditionalUnknownVarAlias(flag: bool, cb: () -> ()) {\n"
        "  var f = failDynamic\n"
        "  if (flag) { f = cb }\n"
        "  f()\n"
        "}\n"
        "fn viaWhileTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  while (flag) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaForTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  for (; flag; ) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaForIncrementTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  for (; flag; f = failOtherDynamic) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaForInTargetUnion() {\n"
        "  var f = noThrowDynamic\n"
        "  for (i in 0..3) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaLoopUnknownVarAlias(flag: bool, cb: () -> ()) {\n"
        "  var f = failDynamic\n"
        "  while (flag) { f = cb }\n"
        "  f()\n"
        "}\n"
        "fn viaTryCatchTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  try { f = failDynamic; maybeDynamic(flag) } "
        "catch (e: DynamicErr) { f = failOtherDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaTryCatchBaseTargetUnion(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  try { maybeDynamic(flag) } catch (e: DynamicErr) { f = failDynamic }\n"
        "  f()\n"
        "}\n"
        "fn viaTryCatchUnknownVarAlias(flag: bool, cb: () -> ()) {\n"
        "  var f = failDynamic\n"
        "  try { maybeDynamic(flag) } catch (e: DynamicErr) { f = cb }\n"
        "  f()\n"
        "}\n"
        "fn viaTryMutatedAliasReadInCatch(flag: bool) {\n"
        "  var f = noThrowDynamic\n"
        "  try { f = failDynamic; maybeDynamic(flag) } catch (e: DynamicErr) { f() }\n"
        "}\n"
        "fn viaReturnedFunctionValue(flag: bool) {\n"
        "  var f = chooseDynamic(flag)\n"
        "  f()\n"
        "}\n"
        "fn viaReturnedFunctionValueDirect(flag: bool) {\n"
        "  chooseDynamic(flag)()\n"
        "}\n"
        "fn viaReturnedFunctionValueBase(flag: bool) {\n"
        "  var f = chooseNoThrowOrThrow(flag)\n"
        "  f()\n"
        "}\n"
        "fn viaReturnedFunctionValueUnknown(cb: () -> ()) {\n"
        "  var f = chooseUnknownDynamic(cb)\n"
        "  f()\n"
        "}\n"
        "fn viaCapturedFunctionValue(flag: bool) {\n"
        "  var run = makeCapturedFunctionValue(flag)\n"
        "  run()\n"
        "}\n"
        "fn viaCapturedFunctionValueDirect(flag: bool) {\n"
        "  makeCapturedFunctionValue(flag)()\n"
        "}\n"
        "fn viaCapturedFunctionValueUnknown(cb: () -> ()) {\n"
        "  var run = makeCapturedUnknownFunctionValue(cb)\n"
        "  run()\n"
        "}\n"
        "fn viaCapturedFunctionValueCurrentRebound() {\n"
        "  var f = failDynamic\n"
        "  var run = fn() { f() }\n"
        "  f = noThrowDynamic\n"
        "  run()\n"
        "}\n"
        "fn viaHigherOrderCallback() {\n"
        "  invokeCallback(failDynamic)\n"
        "}\n"
        "fn viaHigherOrderFunctionExpr() {\n"
        "  invokeCallback(fn() { throw OtherDynamicErr.Boom })\n"
        "}\n"
        "fn viaHigherOrderUnion(flag: bool) {\n"
        "  chooseCallback(flag, failDynamic, failOtherDynamic)\n"
        "}\n"
        "fn viaHigherOrderUnknown(cb: () -> ()) {\n"
        "  invokeCallback(cb)\n"
        "}\n"
        "fn viaConstAliasStillExact() {\n"
        "  const f = failDynamic\n"
        "  f()\n"
        "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_dynamic_function_value.xr", program);

    const XaEffectSummary *stable_var = analyzer_function_effect_summary(a, "viaStableVarAlias");
    const XaEffectSummary *stable_chain =
        analyzer_function_effect_summary(a, "viaStableVarAliasChain");
    const XaEffectSummary *rebound_var = analyzer_function_effect_summary(a, "viaReboundVarAlias");
    const XaEffectSummary *rebound_to_throwing =
        analyzer_function_effect_summary(a, "viaReboundVarAliasToThrowing");
    const XaEffectSummary *unknown_rebound =
        analyzer_function_effect_summary(a, "viaUnknownReboundVarAlias");
    const XaEffectSummary *conditional_rebound =
        analyzer_function_effect_summary(a, "viaConditionalReboundVarAlias");
    const XaEffectSummary *if_else_union =
        analyzer_function_effect_summary(a, "viaIfElseTargetUnion");
    const XaEffectSummary *conditional_unknown =
        analyzer_function_effect_summary(a, "viaConditionalUnknownVarAlias");
    const XaEffectSummary *while_union = analyzer_function_effect_summary(a, "viaWhileTargetUnion");
    const XaEffectSummary *for_union = analyzer_function_effect_summary(a, "viaForTargetUnion");
    const XaEffectSummary *for_increment_union =
        analyzer_function_effect_summary(a, "viaForIncrementTargetUnion");
    const XaEffectSummary *for_in_union =
        analyzer_function_effect_summary(a, "viaForInTargetUnion");
    const XaEffectSummary *loop_unknown =
        analyzer_function_effect_summary(a, "viaLoopUnknownVarAlias");
    const XaEffectSummary *try_catch_union =
        analyzer_function_effect_summary(a, "viaTryCatchTargetUnion");
    const XaEffectSummary *try_catch_base_union =
        analyzer_function_effect_summary(a, "viaTryCatchBaseTargetUnion");
    const XaEffectSummary *try_catch_unknown =
        analyzer_function_effect_summary(a, "viaTryCatchUnknownVarAlias");
    const XaEffectSummary *try_mutated_catch_read =
        analyzer_function_effect_summary(a, "viaTryMutatedAliasReadInCatch");
    const XaEffectSummary *returned_function_value =
        analyzer_function_effect_summary(a, "viaReturnedFunctionValue");
    const XaEffectSummary *returned_function_value_direct =
        analyzer_function_effect_summary(a, "viaReturnedFunctionValueDirect");
    const XaEffectSummary *returned_function_value_base =
        analyzer_function_effect_summary(a, "viaReturnedFunctionValueBase");
    const XaEffectSummary *returned_function_value_unknown =
        analyzer_function_effect_summary(a, "viaReturnedFunctionValueUnknown");
    const XaEffectSummary *captured_function_value =
        analyzer_function_effect_summary(a, "viaCapturedFunctionValue");
    const XaEffectSummary *captured_function_value_direct =
        analyzer_function_effect_summary(a, "viaCapturedFunctionValueDirect");
    const XaEffectSummary *captured_function_value_unknown =
        analyzer_function_effect_summary(a, "viaCapturedFunctionValueUnknown");
    const XaEffectSummary *captured_function_value_current_rebound =
        analyzer_function_effect_summary(a, "viaCapturedFunctionValueCurrentRebound");
    const XaEffectSummary *higher_order_callback =
        analyzer_function_effect_summary(a, "viaHigherOrderCallback");
    const XaEffectSummary *higher_order_function_expr =
        analyzer_function_effect_summary(a, "viaHigherOrderFunctionExpr");
    const XaEffectSummary *higher_order_union =
        analyzer_function_effect_summary(a, "viaHigherOrderUnion");
    const XaEffectSummary *higher_order_unknown =
        analyzer_function_effect_summary(a, "viaHigherOrderUnknown");
    const XaEffectSummary *const_alias =
        analyzer_function_effect_summary(a, "viaConstAliasStillExact");
    ASSERT(stable_var != NULL);
    ASSERT(stable_chain != NULL);
    ASSERT(rebound_var != NULL);
    ASSERT(rebound_to_throwing != NULL);
    ASSERT(unknown_rebound != NULL);
    ASSERT(conditional_rebound != NULL);
    ASSERT(if_else_union != NULL);
    ASSERT(conditional_unknown != NULL);
    ASSERT(while_union != NULL);
    ASSERT(for_union != NULL);
    ASSERT(for_increment_union != NULL);
    ASSERT(for_in_union != NULL);
    ASSERT(loop_unknown != NULL);
    ASSERT(try_catch_union != NULL);
    ASSERT(try_catch_base_union != NULL);
    ASSERT(try_catch_unknown != NULL);
    ASSERT(try_mutated_catch_read != NULL);
    ASSERT(returned_function_value != NULL);
    ASSERT(returned_function_value_direct != NULL);
    ASSERT(returned_function_value_base != NULL);
    ASSERT(returned_function_value_unknown != NULL);
    ASSERT(captured_function_value != NULL);
    ASSERT(captured_function_value_direct != NULL);
    ASSERT(captured_function_value_unknown != NULL);
    ASSERT(captured_function_value_current_rebound != NULL);
    ASSERT(higher_order_callback != NULL);
    ASSERT(higher_order_function_expr != NULL);
    ASSERT(higher_order_union != NULL);
    ASSERT(higher_order_unknown != NULL);
    ASSERT(const_alias != NULL);

    ASSERT(effect_summary_has_enum_named(a, stable_var, "DynamicErr"));
    ASSERT((stable_var->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_chain, "DynamicErr"));
    ASSERT((stable_chain->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(rebound_var->completeness == XA_EFFECT_COMPLETE);
    ASSERT((rebound_var->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, rebound_var, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, rebound_to_throwing, "DynamicErr"));
    ASSERT((rebound_to_throwing->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(unknown_rebound->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((unknown_rebound->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, unknown_rebound, "DynamicErr"));
    ASSERT(conditional_rebound->completeness == XA_EFFECT_COMPLETE);
    ASSERT((conditional_rebound->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, conditional_rebound, "DynamicErr"));
    ASSERT(if_else_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((if_else_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, if_else_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, if_else_union, "OtherDynamicErr"));
    ASSERT(conditional_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((conditional_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(while_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((while_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, while_union, "DynamicErr"));
    ASSERT(for_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, for_union, "DynamicErr"));
    ASSERT(for_increment_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_increment_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, for_increment_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, for_increment_union, "OtherDynamicErr"));
    ASSERT(for_in_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_in_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, for_in_union, "DynamicErr"));
    ASSERT(loop_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((loop_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(try_catch_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((try_catch_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, try_catch_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, try_catch_union, "OtherDynamicErr"));
    ASSERT(try_catch_base_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((try_catch_base_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, try_catch_base_union, "DynamicErr"));
    ASSERT(try_catch_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((try_catch_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(try_mutated_catch_read->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((try_mutated_catch_read->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(returned_function_value->completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, returned_function_value, "OtherDynamicErr"));
    ASSERT(returned_function_value_direct->completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value_direct->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_direct, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_direct, "OtherDynamicErr"));
    ASSERT(returned_function_value_base->completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value_base->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_base, "DynamicErr"));
    ASSERT(!effect_summary_has_enum_named(a, returned_function_value_base, "OtherDynamicErr"));
    ASSERT(returned_function_value_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((returned_function_value_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) !=
           0);
    ASSERT(captured_function_value->completeness == XA_EFFECT_COMPLETE);
    ASSERT((captured_function_value->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, captured_function_value, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, captured_function_value, "OtherDynamicErr"));
    ASSERT(captured_function_value_direct->completeness == XA_EFFECT_COMPLETE);
    ASSERT((captured_function_value_direct->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, captured_function_value_direct, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, captured_function_value_direct, "OtherDynamicErr"));
    ASSERT(captured_function_value_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((captured_function_value_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) !=
           0);
    ASSERT(!effect_summary_has_enum_named(a, captured_function_value_unknown, "DynamicErr"));
    ASSERT(captured_function_value_current_rebound->completeness == XA_EFFECT_COMPLETE);
    ASSERT((captured_function_value_current_rebound->unknown_reasons &
            XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(
        !effect_summary_has_enum_named(a, captured_function_value_current_rebound, "DynamicErr"));
    ASSERT(!effect_summary_has_enum_named(a, captured_function_value_current_rebound,
                                          "OtherDynamicErr"));
    ASSERT(higher_order_callback->completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_callback->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, higher_order_callback, "DynamicErr"));
    ASSERT(!effect_summary_has_enum_named(a, higher_order_callback, "OtherDynamicErr"));
    ASSERT(higher_order_function_expr->completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_function_expr->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, higher_order_function_expr, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, higher_order_function_expr, "OtherDynamicErr"));
    ASSERT(higher_order_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, higher_order_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, higher_order_union, "OtherDynamicErr"));
    ASSERT(higher_order_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((higher_order_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, higher_order_unknown, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, const_alias, "DynamicErr"));
    ASSERT((const_alias->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_immediate_function_expr_calls) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum LambdaErr { Boom }\n"
                         "fn failLambda() { throw LambdaErr.Boom }\n"
                         "fn viaImmediateLambdaThrow() {\n"
                         "  (fn() { throw LambdaErr.Boom })()\n"
                         "}\n"
                         "fn viaImmediateLambdaCall() {\n"
                         "  (fn() { failLambda() })()\n"
                         "}\n"
                         "fn viaConstStoredLambda() {\n"
                         "  const f = fn() { throw LambdaErr.Boom }\n"
                         "  f()\n"
                         "}\n"
                         "fn viaConstStoredLambdaChain() {\n"
                         "  const f = fn() { throw LambdaErr.Boom }\n"
                         "  const g = (f)\n"
                         "  g()\n"
                         "}\n"
                         "fn viaStableStoredLambda() {\n"
                         "  var f = fn() { throw LambdaErr.Boom }\n"
                         "  f()\n"
                         "}\n"
                         "fn viaStableStoredLambdaChain() {\n"
                         "  var f = fn() { throw LambdaErr.Boom }\n"
                         "  var g = f\n"
                         "  g()\n"
                         "}\n"
                         "fn viaReboundStoredLambda() {\n"
                         "  var f = fn() { throw LambdaErr.Boom }\n"
                         "  f = fn() { }\n"
                         "  f()\n"
                         "}\n"
                         "fn viaReboundStoredLambdaToThrowing() {\n"
                         "  var f = fn() { }\n"
                         "  f = fn() { throw LambdaErr.Boom }\n"
                         "  f()\n"
                         "}\n"
                         "fn viaUnknownReboundStoredLambda(cb: () -> ()) {\n"
                         "  var f = fn() { throw LambdaErr.Boom }\n"
                         "  f = cb\n"
                         "  f()\n"
                         "}\n"
                         "fn viaConditionalReboundStoredLambda(flag: bool) {\n"
                         "  var f = fn() { }\n"
                         "  if (flag) { f = fn() { throw LambdaErr.Boom } }\n"
                         "  f()\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_immediate_function_expr.xr", program);

    const XaEffectSummary *immediate_throw =
        analyzer_function_effect_summary(a, "viaImmediateLambdaThrow");
    const XaEffectSummary *immediate_call =
        analyzer_function_effect_summary(a, "viaImmediateLambdaCall");
    const XaEffectSummary *const_stored =
        analyzer_function_effect_summary(a, "viaConstStoredLambda");
    const XaEffectSummary *const_chain =
        analyzer_function_effect_summary(a, "viaConstStoredLambdaChain");
    const XaEffectSummary *stable_stored =
        analyzer_function_effect_summary(a, "viaStableStoredLambda");
    const XaEffectSummary *stable_chain =
        analyzer_function_effect_summary(a, "viaStableStoredLambdaChain");
    const XaEffectSummary *rebound = analyzer_function_effect_summary(a, "viaReboundStoredLambda");
    const XaEffectSummary *rebound_to_throwing =
        analyzer_function_effect_summary(a, "viaReboundStoredLambdaToThrowing");
    const XaEffectSummary *unknown_rebound =
        analyzer_function_effect_summary(a, "viaUnknownReboundStoredLambda");
    const XaEffectSummary *conditional_rebound =
        analyzer_function_effect_summary(a, "viaConditionalReboundStoredLambda");
    ASSERT(immediate_throw != NULL);
    ASSERT(immediate_call != NULL);
    ASSERT(const_stored != NULL);
    ASSERT(const_chain != NULL);
    ASSERT(stable_stored != NULL);
    ASSERT(stable_chain != NULL);
    ASSERT(rebound != NULL);
    ASSERT(rebound_to_throwing != NULL);
    ASSERT(unknown_rebound != NULL);
    ASSERT(conditional_rebound != NULL);

    ASSERT(effect_summary_has_enum_named(a, immediate_throw, "LambdaErr"));
    ASSERT((immediate_throw->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, immediate_call, "LambdaErr"));
    ASSERT((immediate_call->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, const_stored, "LambdaErr"));
    ASSERT((const_stored->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, const_chain, "LambdaErr"));
    ASSERT((const_chain->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_stored, "LambdaErr"));
    ASSERT((stable_stored->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_chain, "LambdaErr"));
    ASSERT((stable_chain->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(rebound->completeness == XA_EFFECT_COMPLETE);
    ASSERT((rebound->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, rebound, "LambdaErr"));
    ASSERT(effect_summary_has_enum_named(a, rebound_to_throwing, "LambdaErr"));
    ASSERT((rebound_to_throwing->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(unknown_rebound->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((unknown_rebound->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, unknown_rebound, "LambdaErr"));
    ASSERT(conditional_rebound->completeness == XA_EFFECT_COMPLETE);
    ASSERT((conditional_rebound->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, conditional_rebound, "LambdaErr"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_direct_method_calls) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum MethodErr { Boom }\n"
                         "enum OtherMethodErr { Boom }\n"
                         "fn failMethodCallback() { throw MethodErr.Boom }\n"
                         "fn failOtherMethodCallback() { throw OtherMethodErr.Boom }\n"
                         "class Thrower {\n"
                         "  constructor() { }\n"
                         "  fail() { throw MethodErr.Boom }\n"
                         "  static failStatic() { throw MethodErr.Boom }\n"
                         "  invoke(cb: () -> ()) { cb() }\n"
                         "  static invokeStatic(cb: () -> ()) { cb() }\n"
                         "  choose(flag: bool, a: () -> (), b: () -> ()) {\n"
                         "    var cb = a\n"
                         "    if (flag) { cb = b }\n"
                         "    cb()\n"
                         "  }\n"
                         "}\n"
                         "fn viaInstanceMethod() {\n"
                         "  var t = Thrower()\n"
                         "  t.fail()\n"
                         "}\n"
                         "fn viaTemporaryMethod() {\n"
                         "  Thrower().fail()\n"
                         "}\n"
                         "fn viaStaticMethod() {\n"
                         "  Thrower.failStatic()\n"
                         "}\n"
                         "fn viaMethodHigherOrder() {\n"
                         "  var t = Thrower()\n"
                         "  t.invoke(failMethodCallback)\n"
                         "}\n"
                         "fn viaStaticMethodHigherOrder() {\n"
                         "  Thrower.invokeStatic(fn() { throw OtherMethodErr.Boom })\n"
                         "}\n"
                         "fn viaMethodHigherOrderUnion(flag: bool) {\n"
                         "  Thrower().choose(flag, failMethodCallback, failOtherMethodCallback)\n"
                         "}\n"
                         "fn viaMethodHigherOrderUnknown(cb: () -> ()) {\n"
                         "  Thrower().invoke(cb)\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_direct_method_calls.xr", program);

    const XaEffectSummary *instance = analyzer_function_effect_summary(a, "viaInstanceMethod");
    const XaEffectSummary *temporary = analyzer_function_effect_summary(a, "viaTemporaryMethod");
    const XaEffectSummary *static_method = analyzer_function_effect_summary(a, "viaStaticMethod");
    const XaEffectSummary *method_hof = analyzer_function_effect_summary(a, "viaMethodHigherOrder");
    const XaEffectSummary *static_method_hof =
        analyzer_function_effect_summary(a, "viaStaticMethodHigherOrder");
    const XaEffectSummary *method_hof_union =
        analyzer_function_effect_summary(a, "viaMethodHigherOrderUnion");
    const XaEffectSummary *method_hof_unknown =
        analyzer_function_effect_summary(a, "viaMethodHigherOrderUnknown");
    ASSERT(instance != NULL);
    ASSERT(temporary != NULL);
    ASSERT(static_method != NULL);
    ASSERT(method_hof != NULL);
    ASSERT(static_method_hof != NULL);
    ASSERT(method_hof_union != NULL);
    ASSERT(method_hof_unknown != NULL);

    ASSERT(effect_summary_has_enum_named(a, instance, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, temporary, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, static_method, "MethodErr"));
    ASSERT(method_hof->completeness == XA_EFFECT_COMPLETE);
    ASSERT((method_hof->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, method_hof, "MethodErr"));
    ASSERT(!effect_summary_has_enum_named(a, method_hof, "OtherMethodErr"));
    ASSERT(static_method_hof->completeness == XA_EFFECT_COMPLETE);
    ASSERT((static_method_hof->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, static_method_hof, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, static_method_hof, "OtherMethodErr"));
    ASSERT(method_hof_union->completeness == XA_EFFECT_COMPLETE);
    ASSERT((method_hof_union->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, method_hof_union, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, method_hof_union, "OtherMethodErr"));
    ASSERT(method_hof_unknown->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((method_hof_unknown->unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, method_hof_unknown, "MethodErr"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_subtracts_typed_catches) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum CatchErr { Boom, Other }\n"
                         "enum OtherErr { Boom }\n"
                         "fn fail() { throw CatchErr.Boom }\n"
                         "fn handled() { try { fail() } catch (e: CatchErr) { } }\n"
                         "fn leaks() { try { fail() } catch (e: OtherErr) { } }\n"
                         "fn catchesAll() { try { fail() } catch (e) { } }\n"
                         "fn catchBodyThrows() { "
                         "  try { fail() } catch (e: CatchErr) { throw OtherErr.Boom } "
                         "}\n"
                         "fn catchAllRethrows() { try { fail() } catch (e) { throw e } }\n"
                         "fn typedRethrows() { try { fail() } catch (e: CatchErr) { throw e } }\n"
                         "fn catchVarReassignedRethrows() { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    e = CatchErr.Other; throw e "
                         "  } "
                         "}\n"
                         "fn catchAssignmentAliasRethrows() { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    var alias: CatchErr = CatchErr.Other; alias = e; throw alias "
                         "  } "
                         "}\n"
                         "fn catchNestedBlockAssignmentAliasRethrows() { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    var alias: CatchErr = CatchErr.Other; { alias = e }; throw alias "
                         "  } "
                         "}\n"
                         "fn catchNestedBlockAliasInvalidates() { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    var alias: CatchErr = CatchErr.Other; alias = e; "
                         "    { alias = CatchErr.Other }; throw alias "
                         "  } "
                         "}\n"
                         "fn catchNestedBlockLocalAliasDoesNotLeak() { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    var alias: CatchErr = CatchErr.Other; { const alias = e }; "
                         "    throw alias "
                         "  } "
                         "}\n"
                         "fn catchIfAliasDoesNotLeak(flag: bool) { "
                         "  try { fail() } catch (e: CatchErr) { "
                         "    var alias: CatchErr = CatchErr.Other; "
                         "    if (flag) { alias = e }; throw alias "
                         "  } "
                         "}\n"
                         "fn wrongTypedRethrow() { "
                         "  try { fail() } catch (e: OtherErr) { throw e } "
                         "}\n"
                         "fn catchAllAliasRethrows() { "
                         "  try { fail() } catch (e) { const alias = e; throw alias } "
                         "}\n"
                         "fn catchAllVarAliasRethrows() { "
                         "  try { fail() } catch (e) { var alias = e; throw alias } "
                         "}\n"
                         "fn typedAliasRethrows() { "
                         "  try { fail() } catch (e: CatchErr) { const alias = (e); throw alias } "
                         "}\n"
                         "fn wrongTypedAliasRethrow() { "
                         "  try { fail() } catch (e: OtherErr) { const alias = e; throw alias } "
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_catch_subtraction.xr", program);

    const XaEffectSummary *fail = analyzer_function_effect_summary(a, "fail");
    const XaEffectSummary *handled = analyzer_function_effect_summary(a, "handled");
    const XaEffectSummary *leaks = analyzer_function_effect_summary(a, "leaks");
    const XaEffectSummary *catches_all = analyzer_function_effect_summary(a, "catchesAll");
    const XaEffectSummary *catch_body_throws =
        analyzer_function_effect_summary(a, "catchBodyThrows");
    const XaEffectSummary *catch_all_rethrows =
        analyzer_function_effect_summary(a, "catchAllRethrows");
    const XaEffectSummary *typed_rethrows = analyzer_function_effect_summary(a, "typedRethrows");
    const XaEffectSummary *catch_var_reassigned_rethrows =
        analyzer_function_effect_summary(a, "catchVarReassignedRethrows");
    const XaEffectSummary *catch_assignment_alias_rethrows =
        analyzer_function_effect_summary(a, "catchAssignmentAliasRethrows");
    const XaEffectSummary *catch_nested_assignment_alias_rethrows =
        analyzer_function_effect_summary(a, "catchNestedBlockAssignmentAliasRethrows");
    const XaEffectSummary *catch_nested_alias_invalidates =
        analyzer_function_effect_summary(a, "catchNestedBlockAliasInvalidates");
    const XaEffectSummary *catch_nested_local_alias_does_not_leak =
        analyzer_function_effect_summary(a, "catchNestedBlockLocalAliasDoesNotLeak");
    const XaEffectSummary *catch_if_alias_does_not_leak =
        analyzer_function_effect_summary(a, "catchIfAliasDoesNotLeak");
    const XaEffectSummary *wrong_typed_rethrow =
        analyzer_function_effect_summary(a, "wrongTypedRethrow");
    const XaEffectSummary *catch_all_alias_rethrows =
        analyzer_function_effect_summary(a, "catchAllAliasRethrows");
    const XaEffectSummary *catch_all_var_alias_rethrows =
        analyzer_function_effect_summary(a, "catchAllVarAliasRethrows");
    const XaEffectSummary *typed_alias_rethrows =
        analyzer_function_effect_summary(a, "typedAliasRethrows");
    const XaEffectSummary *wrong_typed_alias_rethrow =
        analyzer_function_effect_summary(a, "wrongTypedAliasRethrow");
    ASSERT(fail != NULL);
    ASSERT(handled != NULL);
    ASSERT(leaks != NULL);
    ASSERT(catches_all != NULL);
    ASSERT(catch_body_throws != NULL);
    ASSERT(catch_all_rethrows != NULL);
    ASSERT(typed_rethrows != NULL);
    ASSERT(catch_var_reassigned_rethrows != NULL);
    ASSERT(catch_assignment_alias_rethrows != NULL);
    ASSERT(catch_nested_assignment_alias_rethrows != NULL);
    ASSERT(catch_nested_alias_invalidates != NULL);
    ASSERT(catch_nested_local_alias_does_not_leak != NULL);
    ASSERT(catch_if_alias_does_not_leak != NULL);
    ASSERT(wrong_typed_rethrow != NULL);
    ASSERT(catch_all_alias_rethrows != NULL);
    ASSERT(catch_all_var_alias_rethrows != NULL);
    ASSERT(typed_alias_rethrows != NULL);
    ASSERT(wrong_typed_alias_rethrow != NULL);

    ASSERT(effect_summary_has_enum_named(a, fail, "CatchErr"));
    ASSERT(handled->escaping.count == 0);
    ASSERT(effect_summary_has_enum_named(a, leaks, "CatchErr"));
    ASSERT(catches_all->escaping.count == 0);
    ASSERT(!effect_summary_has_enum_named(a, catch_body_throws, "CatchErr"));
    ASSERT(effect_summary_has_enum_named(a, catch_body_throws, "OtherErr"));
    ASSERT(effect_summary_has_enum_named(a, catch_all_rethrows, "CatchErr"));
    ASSERT(effect_summary_has_enum_named(a, typed_rethrows, "CatchErr"));
    const XaErrorTypeSet *reassigned_catch_set =
        effect_summary_enum_set_named(a, catch_var_reassigned_rethrows, "CatchErr");
    ASSERT(reassigned_catch_set != NULL);
    ASSERT(reassigned_catch_set->all_variants);
    const XaErrorTypeSet *assigned_alias_set =
        effect_summary_enum_set_named(a, catch_assignment_alias_rethrows, "CatchErr");
    ASSERT(assigned_alias_set != NULL);
    ASSERT(!assigned_alias_set->all_variants);
    ASSERT(xa_bitset_test(&assigned_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&assigned_alias_set->variants, 1));
    const XaErrorTypeSet *nested_assigned_alias_set =
        effect_summary_enum_set_named(a, catch_nested_assignment_alias_rethrows, "CatchErr");
    ASSERT(nested_assigned_alias_set != NULL);
    ASSERT(!nested_assigned_alias_set->all_variants);
    ASSERT(xa_bitset_test(&nested_assigned_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&nested_assigned_alias_set->variants, 1));
    const XaErrorTypeSet *nested_invalidated_alias_set =
        effect_summary_enum_set_named(a, catch_nested_alias_invalidates, "CatchErr");
    ASSERT(nested_invalidated_alias_set != NULL);
    ASSERT(nested_invalidated_alias_set->all_variants);
    const XaErrorTypeSet *nested_local_alias_set =
        effect_summary_enum_set_named(a, catch_nested_local_alias_does_not_leak, "CatchErr");
    ASSERT(nested_local_alias_set != NULL);
    ASSERT(nested_local_alias_set->all_variants);
    const XaErrorTypeSet *if_alias_set =
        effect_summary_enum_set_named(a, catch_if_alias_does_not_leak, "CatchErr");
    ASSERT(if_alias_set != NULL);
    ASSERT(if_alias_set->all_variants);
    ASSERT(effect_summary_has_enum_named(a, wrong_typed_rethrow, "CatchErr"));
    ASSERT(!effect_summary_has_enum_named(a, wrong_typed_rethrow, "OtherErr"));
    ASSERT(effect_summary_has_enum_named(a, catch_all_alias_rethrows, "CatchErr"));
    ASSERT(effect_summary_has_enum_named(a, catch_all_var_alias_rethrows, "CatchErr"));
    ASSERT(effect_summary_has_enum_named(a, typed_alias_rethrows, "CatchErr"));
    ASSERT(effect_summary_has_enum_named(a, wrong_typed_alias_rethrow, "CatchErr"));
    ASSERT(!effect_summary_has_enum_named(a, wrong_typed_alias_rethrow, "OtherErr"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_empty_array_uses_unsolved_infer_var) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var xs = []\nvar ys: Array<int> = []\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "empty_array_infer_var.xr", program);

    ASSERT(analyzer_diag_contains(a, "cannot infer element type for empty array literal"));
    ASSERT(a->unresolved_inference_count >= 1);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_empty_set_uses_unsolved_infer_var) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var xs = #[]\nvar ys: Set<int> = #[]\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "empty_set_infer_var.xr", program);

    ASSERT(analyzer_diag_contains(a, "cannot infer element type for empty set literal"));
    ASSERT(a->unresolved_inference_count >= 1);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_empty_map_uses_unsolved_infer_var) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var xs = #{}\nvar ys: Map<string, int> = #{}\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "empty_map_infer_var.xr", program);

    ASSERT(analyzer_diag_contains(a, "cannot infer key/value types for empty map literal"));
    ASSERT(a->unresolved_inference_count >= 1);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_rejects_builtin_generic_arity) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "builtin_generic_arity.xr", &scope));

    XrType *bare_array = xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "Array"));
    ASSERT(bare_array != NULL);
    ASSERT(XR_TYPE_IS_ERROR(bare_array));

    XrTypeRef *map_args[] = {xr_tref_string(g_session)};
    XrType *short_map =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Map", map_args, 1));
    ASSERT(short_map != NULL);
    ASSERT(XR_TYPE_IS_ERROR(short_map));

    XrTypeRef *array_args[] = {xr_tref_int(g_session), xr_tref_string(g_session)};
    XrType *wide_array =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Array", array_args, 2));
    ASSERT(wide_array != NULL);
    ASSERT(XR_TYPE_IS_ERROR(wide_array));

    XrType *bare_task = xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "Task"));
    ASSERT(bare_task != NULL);
    ASSERT(XR_TYPE_IS_ERROR(bare_task));

    XrType *bare_iterable = xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "Iterable"));
    ASSERT(bare_iterable != NULL);
    ASSERT(XR_TYPE_IS_ERROR(bare_iterable));

    XrType *hashable = xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "Hashable"));
    ASSERT(hashable != NULL);
    ASSERT(hashable->kind == XR_KIND_INTERFACE);

    ASSERT(analyzer_diag_contains(a, "generic type 'Array' expects 1 type argument, got 0"));
    ASSERT(analyzer_diag_contains(a, "generic type 'Map' expects 2 type arguments, got 1"));
    ASSERT(analyzer_diag_contains(a, "generic type 'Array' expects 1 type argument, got 2"));
    ASSERT(analyzer_diag_contains(a, "generic type 'Task' expects 1 type argument, got 0"));
    ASSERT(analyzer_diag_contains(a, "generic type 'Iterable' expects 1 type argument, got 0"));

    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_rejects_error_type_container_success_types) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "container_error_type_success.xr",
                                          &scope));

    XrTypeRef *err = xr_tref_error(g_session);
    XrTypeRef *array_args[] = {err};
    XrType *array_type =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Array", array_args, 1));
    ASSERT(array_type != NULL);
    ASSERT(XR_TYPE_IS_ERROR(array_type));

    XrTypeRef *map_args[] = {xr_tref_string(g_session), err};
    XrType *map_type =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Map", map_args, 2));
    ASSERT(map_type != NULL);
    ASSERT(XR_TYPE_IS_ERROR(map_type));

    XrType *fixed_type = xr_tref_resolve_in_analyzer(a, xr_tref_fixed_array(g_session, err, 4));
    ASSERT(fixed_type != NULL);
    ASSERT(XR_TYPE_IS_ERROR(fixed_type));

    ASSERT(analyzer_diag_contains(a, "container element type 'Array'"));
    ASSERT(analyzer_diag_contains(a, "container value type 'Map'"));
    ASSERT(analyzer_diag_contains(a, "container element type 'fixed array'"));

    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_rejects_error_type_generic_argument_and_constraint) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    XaSymbol *box = xa_symbol_new("Box", XA_SYM_CLASS);
    ASSERT(box != NULL);
    box->links.type = xr_type_new_class(a->isolate, "Box");
    box->links.class_info = xa_class_info_new("Box");
    xa_scope_add_symbol(a->global_scope, box);

    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "box_error_type_arg.xr", &scope));
    XrTypeRef *err = xr_tref_error(g_session);
    XrTypeRef *box_args[] = {err};
    XrType *box_type =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Box", box_args, 1));
    ASSERT(box_type != NULL);
    ASSERT(XR_TYPE_IS_ERROR(box_type));
    ASSERT(analyzer_diag_contains(a, "generic type argument 'Box'"));
    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);

    xa_analyzer_free(a);
    setup_pool();

    AstNode *program = xr_parse(g_session, "fn id<T: Iterable<int>>(x: T) -> T { return x }\n");
    ASSERT(program != NULL);
    ASSERT(program->type == AST_PROGRAM);
    ASSERT(program->as.program.count == 1);
    AstNode *fn = program->as.program.statements[0];
    ASSERT(fn != NULL);
    ASSERT(fn->type == AST_FUNCTION_DECL);
    ASSERT(fn->as.function_decl.type_param_count == 1);
    XrGenericParam *param = fn->as.function_decl.type_params[0];
    ASSERT(param != NULL);
    ASSERT(param->constraint_count == 1);
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "constraint_error_type_success.xr",
                                          &scope));
    param->constraints[0] = xr_tref_error(g_session);

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    xa_analyzer_analyze(a, "generic_constraint_error_type.xr", program);
    ASSERT(analyzer_diag_contains(a, "generic constraint 'T'"));

    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(export_symbols_invalidate_table_on_nested_error_type) {
    const char *source = "export fn bad() -> int { return 1 }\n"
                         "export fn good() -> int { return 1 }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    ASSERT(program->type == AST_PROGRAM);

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrModuleSpec spec = {.canonical = "export_error_type_test.xr",
                         .source_path = "export_error_type_test.xr",
                         .ast = program};
    XrModuleGraph graph = {.specs = &spec, .spec_count = 1};
    xa_analyzer_set_graph(a, &graph);
    xa_analyzer_analyze(a, "export_error_type_test.xr", program);

    XaSymbol *bad = xa_scope_lookup(a->current_scope, "bad");
    ASSERT(bad != NULL);
    XrType *poisoned_return = xr_type_new_array(a->isolate, xr_type_new_error(NULL));
    XrType *poisoned_fn = xr_type_new_function(a->isolate, NULL, 0, poisoned_return, false);
    ASSERT(poisoned_fn != NULL);
    bad->links.type = poisoned_fn;
    bad->links.declared_type = poisoned_fn;

    XrHashMap *exports = NULL;
    ASSERT(!xa_analyzer_collect_export_symbols_checked(a, (XrAstNode *) program, &exports));
    ASSERT(exports == NULL);
    ASSERT(spec.export_symbols == NULL);
    ASSERT(spec.export_symbols_invalid);

    int diag_count = 0;
    XaDiagnostic *diag = xa_analyzer_get_diagnostics(a, &diag_count);
    ASSERT(diag_count > 0);
    bool saw_export_diag = false;
    for (; diag; diag = diag->next) {
        if (diag->message && strstr(diag->message, "Export 'bad'") &&
            strstr(diag->message, "module export table is invalid")) {
            saw_export_diag = true;
            break;
        }
    }
    ASSERT(saw_export_diag);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(compile_type_class) {
    // Class type using new API
    XrType *cls = xr_type_new_class(g_isolate, "MyClass");
    ASSERT(XR_TYPE_IS_CLASS(cls));
    ASSERT(strcmp(cls->instance.class_name, "MyClass") == 0);
}

TEST(compile_type_optional) {
    // int? -> nullable type (unified representation)
    XrType *opt = xr_type_new_optional(g_isolate, xr_type_new_int(NULL));
    ASSERT(opt->is_nullable);
    ASSERT(XR_TYPE_IS_INT(opt));
}

TEST(type_substitute_preserves_nullable_type_param) {
    XrType *ret = xr_type_new_type_param(g_isolate, "T", 0);
    ret = xr_type_make_nullable(g_isolate, ret);
    XrType *fn = xr_type_new_function(g_isolate, NULL, 0, ret, false);

    const char *names[] = {"T"};
    XrType *actuals[] = {xr_type_new_int(NULL)};
    XrType *subst = xr_type_substitute(g_isolate, fn, names, actuals, 1);

    ASSERT(subst != NULL);
    ASSERT(XR_TYPE_IS_FUNCTION(subst));
    ASSERT(subst->function.return_type != NULL);
    ASSERT(XR_TYPE_IS_INT(subst->function.return_type));
    ASSERT(subst->function.return_type->is_nullable);
}

// ============================================================================
// Edge case tests
// ============================================================================

TEST(type_null_handling) {
    // Operations on NULL should not crash
    ASSERT(xr_type_to_string(NULL) != NULL);
    ASSERT(xr_type_assignable(NULL, NULL) == false);
}

TEST(scope_null_handling) {
    // Lookup on NULL scope should not crash
    ASSERT(xa_scope_lookup(NULL, "x") == NULL);
    ASSERT(xa_scope_lookup_local(NULL, "x") == NULL);
}

TEST(symbol_links_lifecycle) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    XaSymbol *sym = xa_symbol_new("test", XA_SYM_VARIABLE);
    xa_scope_add_symbol(a->global_scope, sym);

    // Get links (should create if not exists)
    XaSymbolLinks *links = xa_analyzer_get_links(a, sym);
    ASSERT(links != NULL);
    ASSERT(links->type == NULL);

    // Set type
    links->type = xr_type_new_int(NULL);

    // Get same links
    XaSymbolLinks *links2 = xa_analyzer_get_links(a, sym);
    ASSERT(links2 == links);
    ASSERT(XR_TYPE_IS_INT(links2->type));

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(deeply_nested_types) {
    // Array<Map<string, Array<int>>>
    XrType *inner_arr = xr_type_new_array(g_isolate, xr_type_new_int(NULL));
    XrType *map = xr_type_new_map(g_isolate, xr_type_new_string(NULL), inner_arr);
    XrType *outer_arr = xr_type_new_array(g_isolate, map);

    ASSERT(XR_TYPE_IS_ARRAY(outer_arr));
    ASSERT(XR_TYPE_IS_MAP(outer_arr->container.element_type));

    const char *str = xr_type_to_string(outer_arr);
    ASSERT(str != NULL);
    ASSERT(strstr(str, "Array") != NULL);
}

TEST(union_type_dedup) {
    // int | int should be int
    XrType *t_int1 = xr_type_new_int(NULL);
    XrType *t_int2 = xr_type_new_int(NULL);
    XrType *u = xr_type_union(g_isolate, t_int1, t_int2);

    ASSERT(XR_TYPE_IS_INT(u));
    // Should not have union flag if types are same
}

TEST(class_info_members) {
    XrClassInfo *info = xa_class_info_new("TestClass");
    ASSERT(info != NULL);
    ASSERT(strcmp(info->name, "TestClass") == 0);

    // Add field
    XaSymbol *field = xa_symbol_new("value", XA_SYM_FIELD);
    xa_class_info_add_field(info, field);
    ASSERT(info->field_count == 1);

    // Add method
    XaSymbol *method = xa_symbol_new("getValue", XA_SYM_METHOD);
    xa_class_info_add_method(info, method);
    ASSERT(info->method_count == 1);

    // Lookup
    ASSERT(xa_class_info_lookup_member(info, "value") == field);
    ASSERT(xa_class_info_lookup_member(info, "getValue") == method);
    ASSERT(xa_class_info_lookup_member(info, "notExist") == NULL);

    xa_class_info_free(info);
}

TEST(builtin_http_old_fd_helpers_removed) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info("http");
    ASSERT(mod != NULL);

    const XaBuiltinMember *parse_req = NULL;
    const XaBuiltinMember *send_resp = NULL;
    const XaBuiltinMember *set_conn_handler = NULL;
    const XaBuiltinMember *get_conn_handler = NULL;

    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *fn = &mod->functions[i];
        if (strcmp(fn->name, "parseRequest") == 0)
            parse_req = fn;
        if (strcmp(fn->name, "sendResponse") == 0)
            send_resp = fn;
        if (strcmp(fn->name, "setConnHandler") == 0)
            set_conn_handler = fn;
        if (strcmp(fn->name, "__getConnHandler") == 0)
            get_conn_handler = fn;
    }

    ASSERT(parse_req == NULL);
    ASSERT(send_resp == NULL);
    ASSERT(set_conn_handler == NULL);
    ASSERT(get_conn_handler == NULL);
}

TEST(builtin_datetime_type_methods_not_from_native_defs) {
    XrType *dt = xr_type_new_named_instance(g_isolate, "DateTime");
    ASSERT(dt != NULL);

    ASSERT(xa_builtin_get_by_name("DateTime") == NULL);
    ASSERT(!xa_builtin_is_method(dt, "format"));
    ASSERT(xa_builtin_get_member_signature(dt, "format") == NULL);
    ASSERT(xa_builtin_get_member_doc(dt, "format") == NULL);
    ASSERT(!xa_builtin_is_method(dt, "daysInMonth"));
    ASSERT(xa_builtin_get_member_signature(dt, "daysInMonth") == NULL);
    ASSERT(xa_builtin_get_member_doc(dt, "daysInMonth") == NULL);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    xr_test_suppress_dialogs();
    printf("Running analyzer unit tests...\n\n");

    // Setup type pool (required for type allocation)
    setup_pool();

    printf("Type tests:\n");
    RUN_TEST(type_primitives);
    RUN_TEST(type_containers);
    RUN_TEST(type_union);
    RUN_TEST(type_error_recovery);
    RUN_TEST(type_assignable);
    RUN_TEST(typecheck_assignable_rejects_unknown_source);
    RUN_TEST(typecheck_assignable_rejects_unknown_container_member);
    RUN_TEST(analyzer_check_assignment_rejects_unknown_source);
    RUN_TEST(type_to_string);
    RUN_TEST(type_narrowing);

    printf("\nSymbol tests:\n");
    RUN_TEST(symbol_create);
    RUN_TEST(scope_basic);
    RUN_TEST(scope_lookup);

    printf("\nAnalyzer tests:\n");
    RUN_TEST(analyzer_create);
    RUN_TEST(analyzer_diagnostics);
    RUN_TEST(analyzer_type_telemetry_splits_unknown_and_error);
    RUN_TEST(analyzer_scope_management);
    RUN_TEST(analyzer_error_effect_records_direct_throw_variant);
    RUN_TEST(analyzer_error_effect_propagates_const_function_value_aliases);
    RUN_TEST(analyzer_error_effect_propagates_stable_var_function_values);
    RUN_TEST(analyzer_error_effect_propagates_immediate_function_expr_calls);
    RUN_TEST(analyzer_error_effect_propagates_direct_method_calls);
    RUN_TEST(analyzer_error_effect_subtracts_typed_catches);

    printf("\nFlow analysis tests:\n");
    RUN_TEST(flow_builder_create);
    RUN_TEST(flow_basic_graph);
    RUN_TEST(flow_condition_branches);
    RUN_TEST(flow_cache);
    RUN_TEST(narrow_by_typeof);
    RUN_TEST(narrow_by_null);

    printf("\nAdditional type tests:\n");
    RUN_TEST(type_class_instance);
    RUN_TEST(type_function_complex);
    RUN_TEST(type_void_never);
    RUN_TEST(type_rejects_invalid_counts);
    RUN_TEST(type_function_copy_preserves_metadata);

    printf("\nInference context tests:\n");
    RUN_TEST(infer_context_create);
    RUN_TEST(infer_return_type_collection);
    RUN_TEST(infer_single_return_type);
    RUN_TEST(infer_no_return_type);

    printf("\nCompile type conversion tests:\n");
    RUN_TEST(compile_type_primitives);
    RUN_TEST(compile_type_containers);
    RUN_TEST(compile_type_function);
    RUN_TEST(compile_type_ref_function_modes);
    RUN_TEST(analyzer_empty_array_uses_unsolved_infer_var);
    RUN_TEST(analyzer_empty_set_uses_unsolved_infer_var);
    RUN_TEST(analyzer_empty_map_uses_unsolved_infer_var);
    RUN_TEST(analyzer_rejects_builtin_generic_arity);
    RUN_TEST(analyzer_rejects_error_type_container_success_types);
    RUN_TEST(analyzer_rejects_error_type_generic_argument_and_constraint);
    RUN_TEST(export_symbols_invalidate_table_on_nested_error_type);
    RUN_TEST(compile_type_class);
    RUN_TEST(compile_type_optional);
    RUN_TEST(type_substitute_preserves_nullable_type_param);

    printf("\nEdge case tests:\n");
    RUN_TEST(type_null_handling);
    RUN_TEST(scope_null_handling);
    RUN_TEST(symbol_links_lifecycle);
    RUN_TEST(deeply_nested_types);
    RUN_TEST(union_type_dedup);
    RUN_TEST(class_info_members);
    RUN_TEST(builtin_http_old_fd_helpers_removed);
    RUN_TEST(builtin_datetime_type_methods_not_from_native_defs);

    printf("\n========================================\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");

    // Cleanup type pool
    teardown_pool();

    return tests_failed > 0 ? 1 : 0;
}
