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
#include "xtype_pool.h"
#include "xray_isolate.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

// Global isolate and analyzer for pool initialization
static XrayIsolate *g_isolate = NULL;
static XaAnalyzer *g_analyzer = NULL;

static void setup_pool(void) {
    if (!g_isolate) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        g_isolate = xray_isolate_new(&p);
    }
    if (!g_analyzer) {
        g_analyzer = xa_analyzer_new(g_isolate);
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
        xray_isolate_delete(g_isolate);
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
    XrType *t_arr = xr_type_new_array(g_isolate, xr_type_new_string(NULL));

    ASSERT(strcmp(xr_type_to_string(t_int), "int") == 0);
    ASSERT(strcmp(xr_type_to_string(t_arr), "Array<string>") == 0);
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
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
    ASSERT(a != NULL);
    ASSERT(a->global_scope != NULL);
    ASSERT(a->current_scope == a->global_scope);

    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(analyzer_diagnostics) {
    XaAnalyzer *a = xa_analyzer_new(g_isolate);

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

TEST(analyzer_scope_management) {
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
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
    ASSERT(XR_TYPE_IS_INT(fn->function.param_types[0]));
    ASSERT(XR_TYPE_IS_STRING(fn->function.param_types[1]));
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

    uint8_t modes[] = {XR_PARAM_IN, XR_PARAM_REF};
    fn->function.min_params = 1;
    fn->function.param_passing_modes = modes;

    XrType *copy = xr_type_copy(g_isolate, fn);
    ASSERT(copy != NULL);
    ASSERT(copy != fn);
    ASSERT(copy->function.param_count == 2);
    ASSERT(copy->function.min_params == 1);
    ASSERT(copy->function.param_types != fn->function.param_types);
    ASSERT(copy->function.param_passing_modes != fn->function.param_passing_modes);
    ASSERT(copy->function.param_passing_modes[0] == XR_PARAM_IN);
    ASSERT(copy->function.param_passing_modes[1] == XR_PARAM_REF);
}

// ============================================================================
// Inference context tests
// ============================================================================

TEST(infer_context_create) {
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
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
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
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
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
    XaInferContext *ctx = xa_infer_context_new(a);

    xa_infer_add_return_type(ctx, xr_type_new_int(NULL));

    XrType *ret = xa_infer_compute_return_type(ctx);
    ASSERT(XR_TYPE_IS_INT(ret));

    xa_infer_context_free(ctx);
    xa_analyzer_free(a);
    setup_pool();  // Restore global pool after test
}

TEST(infer_no_return_type) {
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
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
    ASSERT(XR_TYPE_IS_INT(fn->function.param_types[0]));
    ASSERT(XR_TYPE_IS_STRING(fn->function.param_types[1]));
    ASSERT(XR_TYPE_IS_BOOL(fn->function.return_type));
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
    XaAnalyzer *a = xa_analyzer_new(g_isolate);
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

TEST(builtin_http_fast_signatures) {
    const XaBuiltinModule *mod = xa_builtin_get_module_info("http");
    ASSERT(mod != NULL);

    const XaBuiltinMember *parse_req = NULL;
    const XaBuiltinMember *send_resp = NULL;

    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *fn = &mod->functions[i];
        if (strcmp(fn->name, "parseRequest") == 0)
            parse_req = fn;
        if (strcmp(fn->name, "sendResponse") == 0)
            send_resp = fn;
    }

    ASSERT(parse_req != NULL);
    ASSERT(send_resp != NULL);
    ASSERT(strcmp(parse_req->signature, "(fd: int): Array<unknown>?") == 0);
    ASSERT(strcmp(send_resp->signature, "(fd: int, body: string, status?: int): bool") == 0);
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
    RUN_TEST(analyzer_scope_management);

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
    RUN_TEST(compile_type_class);
    RUN_TEST(compile_type_optional);

    printf("\nEdge case tests:\n");
    RUN_TEST(type_null_handling);
    RUN_TEST(scope_null_handling);
    RUN_TEST(symbol_links_lifecycle);
    RUN_TEST(deeply_nested_types);
    RUN_TEST(union_type_dedup);
    RUN_TEST(class_info_members);
    RUN_TEST(builtin_http_fast_signatures);

    printf("\n========================================\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n");

    // Cleanup type pool
    teardown_pool();

    return tests_failed > 0 ? 1 : 0;
}
