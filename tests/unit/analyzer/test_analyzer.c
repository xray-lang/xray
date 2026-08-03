/*
 * test_analyzer.c - Unit tests for static type analyzer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Only include analyzer headers (avoid GC type conflicts)
#include "xtype.h"
#include "xtype_names.h"
#include "xanalyzer_symbol.h"
#include "xanalyzer.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_flow.h"
#include "xanalyzer_infer.h"
#include "xanalyzer_mono.h"
#include "xanalyzer_visitor.h"
#include "xanalyzer_xrd.h"
#include "xast_nodes.h"
#include "xparse.h"
#include "xtype_ref.h"
#include "xtype_ref_resolve.h"
#include "xtype_pool.h"
#include "xhashmap.h"
#include "xmalloc.h"
#include "xarena.h"
#include "xray_vm.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "toolchain/xcompiler_session.h"
#include "../test_win_compat.h"

#include <stdint.h>

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

TEST(type_scalar_alias_identity) {
    XrType *int_default = xr_type_new_int(NULL);
    XrType *int_exact = xr_type_new_int_width(NULL, XR_NATIVE_I64);
    XrType *float_default = xr_type_new_float(NULL);
    XrType *float_exact = xr_type_new_float_width(NULL, XR_NATIVE_F64);
    XrType *byte_type = xr_type_new_int_width(NULL, XR_NATIVE_U8);
    XrType *u8_type = xr_type_new_int_width(NULL, XR_NATIVE_U8);

    ASSERT(int_default == int_exact);
    ASSERT(float_default == float_exact);
    ASSERT(xr_type_equals(byte_type, u8_type));
    ASSERT(int_default->scalar_rep == XR_NATIVE_I64);
    ASSERT(float_default->scalar_rep == XR_NATIVE_F64);
    ASSERT(byte_type->scalar_rep == XR_NATIVE_U8);

    ASSERT(xr_type_equals(xr_type_new_array(g_isolate, int_default),
                          xr_type_new_array(g_isolate, int_exact)));
    ASSERT(xr_type_equals(xr_type_new_slice(g_isolate, byte_type),
                          xr_type_new_slice(g_isolate, u8_type)));
    XrType *default_params[] = {int_default};
    XrType *exact_params[] = {int_exact};
    ASSERT(xr_type_equals(xr_type_new_function(g_isolate, default_params, 1, byte_type, false),
                          xr_type_new_function(g_isolate, exact_params, 1, u8_type, false)));

    ASSERT(xr_type_from_name("int") == XR_TID_INT);
    ASSERT(xr_type_from_name("i64") == XR_TID_INT);
    ASSERT(xr_type_from_name("float") == XR_TID_FLOAT);
    ASSERT(xr_type_from_name("f64") == XR_TID_FLOAT);
    ASSERT(xr_type_from_name("byte") == XR_TID_U8);
    ASSERT(xr_type_from_name("u8") == XR_TID_U8);
    static const char *retired[] = {
        "int8",   "int16",  "int32",   "int64",   "uint8",   "uint16",
        "uint32", "uint64", "float32", "float64", "intsize", "uintsize",
    };
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        ASSERT(xr_type_from_name(retired[i]) == -1);
}

TEST(type_const_capability_is_part_of_identity_and_format) {
    setup_pool();
    XrType *mutable_array = xr_type_new_array(g_isolate, xr_type_new_int(NULL));
    XrType *const_array = xr_type_make_const(g_isolate, mutable_array);
    ASSERT(mutable_array != const_array);
    ASSERT(!xr_type_equals(mutable_array, const_array));
    ASSERT(xr_type_equals(const_array, xr_type_make_const(g_isolate, const_array)));
    ASSERT(strcmp(xr_type_to_string(const_array), "const Array<int>") == 0);

    XrType *scalar = xr_type_new_int(NULL);
    ASSERT(xr_type_make_const(g_isolate, scalar) == scalar);
    ASSERT(xr_type_equals(scalar, xr_type_make_const(g_isolate, scalar)));
}

TEST(type_nullable_qualifier_does_not_mutate_declaration_identity) {
    setup_pool();
    XrType *enum_type = xr_type_new_enum(g_isolate, "NetError");
    XrType *nullable_enum = xr_type_make_nullable(g_isolate, enum_type);

    ASSERT(enum_type != nullable_enum);
    ASSERT(!enum_type->is_nullable);
    ASSERT(nullable_enum->is_nullable);
    ASSERT(nullable_enum->kind == XR_KIND_ENUM);
    ASSERT(strcmp(nullable_enum->enum_type.enum_name, "NetError") == 0);
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

    // Numeric unions preserve the source-level alternatives.  Converting
    // int to float is explicit and union construction must not reintroduce
    // the removed implicit promotion rule.
    XrType *t_float = xr_type_new_float(NULL);
    XrType *numeric = xr_type_union(g_isolate, t_int, t_float);
    ASSERT(XR_TYPE_IS_UNION(numeric));
    ASSERT(xr_type_union_count(numeric) == 2);
    ASSERT(xr_type_union_contains(numeric, XR_KIND_INT));
    ASSERT(xr_type_union_contains(numeric, XR_KIND_FLOAT));
}

TEST(type_error_recovery) {
    XrType *t_error = xr_type_new_error(NULL);
    XrType *t_int = xr_type_new_int(NULL);
    ASSERT(t_error != NULL);
    ASSERT(XR_TYPE_IS_ERROR(t_error));
    ASSERT(XR_TYPE_IS_UNKNOWN_OR_ERROR(t_error));
    ASSERT(!XR_TYPE_IS_UNKNOWN(t_error));
    ASSERT(strcmp(xr_type_to_string(t_error), "<error>") == 0);
    ASSERT(xr_type_equals(t_error, xr_type_new_error(NULL)));
    ASSERT(!xr_type_equals(t_error, xr_type_new_unknown(NULL)));
    ASSERT(strcmp(xr_type_to_string(xr_type_new_unknown(NULL)), "<error>") == 0);

    XrTypeRef error_ref = {.kind = XR_TREF_ERROR};
    XrType *resolved = xr_tref_resolve(g_isolate, &error_ref);
    ASSERT(resolved != NULL);
    ASSERT(XR_TYPE_IS_ERROR(resolved));
    ASSERT(resolved == t_error);

    XrType *poisoned = xr_type_union(g_isolate, xr_type_new_int(NULL), t_error);
    ASSERT(poisoned != NULL);
    ASSERT(XR_TYPE_IS_ERROR(poisoned));

    ASSERT(!xr_type_assignable(t_int, t_error));
    ASSERT(!xr_type_assignable(t_error, t_int));
    ASSERT(xa_recovery_compatible(t_int, t_error));
    ASSERT(xa_recovery_compatible(t_error, t_int));
    ASSERT(xa_typecheck_assignable(t_int, t_error));
    ASSERT(xa_typecheck_assignable(t_error, t_int));
}

TEST(type_assignable) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_unknown = xr_type_new_unknown(NULL);
    XrType *t_never = xr_type_new_never(NULL);

    // int assignable to int
    ASSERT(xr_type_assignable(t_int, t_int));

    // Integer/float conversion is always explicit.
    ASSERT(!xr_type_assignable(t_float, t_int));

    XrType *t_i8 = xr_type_new_int_width(NULL, XR_NATIVE_I8);
    XrType *t_i16 = xr_type_new_int_width(NULL, XR_NATIVE_I16);
    XrType *t_u8 = xr_type_new_int_width(NULL, XR_NATIVE_U8);
    XrType *t_isize = xr_type_new_int_width(NULL, XR_NATIVE_ISIZE);
    XrType *t_f32 = xr_type_new_float_width(NULL, XR_NATIVE_F32);

    ASSERT(xr_type_numeric_conversion_kind(t_i8, t_i8) == XR_CONVERSION_IDENTITY);
    ASSERT(xr_type_numeric_conversion_kind(t_i16, t_i8) == XR_CONVERSION_LOSSLESS_WIDEN);
    ASSERT(xr_type_numeric_conversion_kind(t_i8, t_i16) == XR_CONVERSION_EXPLICIT_TRUNCATE);
    ASSERT(xr_type_numeric_conversion_kind(t_u8, t_i8) == XR_CONVERSION_EXPLICIT_SIGN_CHANGE);
    ASSERT(xr_type_numeric_conversion_kind(t_isize, t_int) == XR_CONVERSION_EXPLICIT_TARGET_WIDTH);
    ASSERT(xr_type_numeric_conversion_kind(t_float, t_f32) == XR_CONVERSION_LOSSLESS_WIDEN);
    ASSERT(xr_type_numeric_conversion_kind(t_f32, t_float) == XR_CONVERSION_EXPLICIT_TRUNCATE);
    ASSERT(xr_type_numeric_conversion_kind(t_float, t_int) == XR_CONVERSION_EXPLICIT_INT_FLOAT);
    ASSERT(xr_type_assignable(t_i16, t_i8));
    ASSERT(!xr_type_assignable(t_i8, t_i16));
    ASSERT(!xr_type_assignable(t_u8, t_i8));
    ASSERT(!xr_type_assignable(t_isize, t_int));
    ASSERT(xr_type_assignable(t_float, t_f32));
    ASSERT(!xr_type_assignable(t_f32, t_float));

    // Internal lattice keeps unknown as a permissive top type.
    ASSERT(xr_type_assignable(t_unknown, t_int));

    // never assignable to anything
    ASSERT(xr_type_assignable(t_int, t_never));

    XrType *mutable_array = xr_type_new_array(g_isolate, xr_type_new_int(g_isolate));
    XrType *const_array = xr_type_make_const(g_isolate, mutable_array);
    ASSERT(xr_type_assignable(const_array, mutable_array));
    ASSERT(!xr_type_assignable(mutable_array, const_array));
}

TEST(record_nullable_field_accepts_explicit_null) {
    const char *names[] = {"secret"};
    XrType *target_fields[] = {
        xr_type_make_nullable(g_isolate, xr_type_new_string(g_isolate)),
    };
    XrType *source_fields[] = {xr_type_new_null(g_isolate)};
    XrType *target = xr_type_new_record_with_fields(g_isolate, names, target_fields, 1, true);
    XrType *source = xr_type_new_record_with_fields(g_isolate, names, source_fields, 1, true);

    ASSERT(xr_type_assignable(target, source));
    ASSERT(xa_typecheck_assignable(target, source));
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
    XrType *t_byte_slice = xr_type_new_slice(g_isolate, t_u8);
    XrType *fn_params[] = {xr_type_new_int_width(NULL, XR_NATIVE_I32)};
    XrType *t_cfn = xr_type_new_function(g_isolate, fn_params, 1,
                                         xr_type_new_int_width(NULL, XR_NATIVE_I32), false);
    t_cfn->function.is_c_abi = true;
    XrType *byte_fn_params[] = {t_u8};
    XrType *t_byte_fn = xr_type_new_function(g_isolate, byte_fn_params, 1, t_u8, false);
    XrType *mode_fn_params[] = {t_int, xr_type_new_string(NULL), xr_type_new_bool(NULL)};
    XrType *t_mode_fn =
        xr_type_new_function(g_isolate, mode_fn_params, 3, xr_type_new_unit(NULL), false);
    ASSERT(xr_type_function_set_param_mode(t_mode_fn, 0, XR_PARAM_READ));
    ASSERT(xr_type_function_set_param_mode(t_mode_fn, 1, XR_PARAM_REF));
    ASSERT(xr_type_function_set_param_mode(t_mode_fn, 2, XR_PARAM_MOVE));

    ASSERT(strcmp(xr_type_to_string(t_int), "int") == 0);
    ASSERT(strcmp(xr_type_to_string(t_u8), "byte") == 0);
    ASSERT(strcmp(xr_type_to_string(t_u64), "u64") == 0);
    ASSERT(strcmp(xr_type_to_string(t_arr), "Array<string>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_arr), "Array<byte>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_slice), "Slice<byte>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_cfn), "CFn<fn(i32): i32>") == 0);
    ASSERT(strcmp(xr_type_to_string(t_byte_fn), "fn(byte): byte") == 0);
    ASSERT(strcmp(xr_type_to_string(t_mode_fn), "fn(int, ref string, move bool): ()") == 0);
}

TEST(type_string_parser_uses_error_recovery_for_invalid_types) {
    XrType *unknown_name = xa_builtin_parse_type_string(g_isolate, "unknown");
    ASSERT(unknown_name != NULL);
    ASSERT(XR_TYPE_IS_ERROR(unknown_name));

    XrType *unregistered_name = xa_builtin_parse_type_string(g_isolate, "NoSuchType");
    ASSERT(unregistered_name != NULL);
    ASSERT(XR_TYPE_IS_ERROR(unregistered_name));

    XrType *nested_unregistered_name = xa_builtin_parse_type_string(g_isolate, "Array<NoSuchType>");
    ASSERT(nested_unregistered_name != NULL);
    ASSERT(XR_TYPE_IS_ARRAY(nested_unregistered_name));
    ASSERT(XR_TYPE_IS_ERROR(nested_unregistered_name->container.element_type));

    XrType *missing = xa_builtin_parse_type_string(g_isolate, NULL);
    ASSERT(missing != NULL);
    ASSERT(XR_TYPE_IS_ERROR(missing));

    XrType *empty = xa_builtin_parse_type_string(g_isolate, "");
    ASSERT(empty != NULL);
    ASSERT(XR_TYPE_IS_ERROR(empty));

    XrType *empty_union = xa_builtin_parse_type_string(g_isolate, "|");
    ASSERT(empty_union != NULL);
    ASSERT(XR_TYPE_IS_ERROR(empty_union));

    XrType *bad_map = xa_builtin_parse_type_string(g_isolate, "Map<int>");
    ASSERT(bad_map != NULL);
    ASSERT(XR_TYPE_IS_ERROR(bad_map));

    XrType *bad_fn = xa_builtin_parse_type_string(g_isolate, "fn(");
    ASSERT(bad_fn != NULL);
    ASSERT(XR_TYPE_IS_ERROR(bad_fn));

    XrType *bad_param = xa_builtin_parse_type_string(g_isolate, "fn(value): int");
    ASSERT(bad_param != NULL);
    ASSERT(XR_TYPE_IS_FUNCTION(bad_param));
    ASSERT(bad_param->function.param_count == 1);
    ASSERT(XR_TYPE_IS_ERROR(bad_param->function.params[0].type));

    XrType *missing_signature = xa_builtin_parse_full_signature(g_isolate, NULL);
    ASSERT(missing_signature != NULL);
    ASSERT(XR_TYPE_IS_FUNCTION(missing_signature));
    ASSERT(XR_TYPE_IS_ERROR(missing_signature->function.return_type));

    XrType *missing_parens = xa_builtin_parse_full_signature(g_isolate, "value: int");
    ASSERT(missing_parens != NULL);
    ASSERT(XR_TYPE_IS_FUNCTION(missing_parens));
    ASSERT(XR_TYPE_IS_ERROR(missing_parens->function.return_type));

    XrType *bad_signature_param = xa_builtin_parse_full_signature(g_isolate, "(value): int");
    ASSERT(bad_signature_param != NULL);
    ASSERT(XR_TYPE_IS_FUNCTION(bad_signature_param));
    ASSERT(bad_signature_param->function.param_count == 1);
    ASSERT(XR_TYPE_IS_ERROR(bad_signature_param->function.params[0].type));
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

TEST(scope_owns_replaced_symbols_until_teardown) {
    XaScope *scope = xa_scope_new(XA_SCOPE_GLOBAL, NULL);
    XaSymbol *first = xa_symbol_new("value", XA_SYM_VARIABLE);
    XaSymbol *replacement = xa_symbol_new("value", XA_SYM_VARIABLE);

    xa_scope_add_symbol(scope, first);
    xa_scope_add_symbol(scope, replacement);

    ASSERT(xa_scope_lookup_local(scope, "value") == replacement);
    ASSERT(first->scope == scope);
    ASSERT(replacement->scope == scope);

    ASSERT(xa_scope_remove_symbol(scope, "value"));
    ASSERT(replacement->scope == NULL);
    xa_symbol_free(replacement);

    // LeakSanitizer verifies that the replaced first binding is still owned
    // and reclaimed by the scope even though it is no longer in the lookup map.
    xa_scope_free(scope);
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

    AstNode *missing_program = xr_parse(g_session, "missing.field(otherMissing);");
    ASSERT(missing_program != NULL);
    xa_analyzer_analyze(a, "telemetry_error_chain.xr", missing_program);
    int missing_diag_count = 0;
    XaDiagnostic *missing_diag = xa_analyzer_get_diagnostics(a, &missing_diag_count);
    ASSERT(missing_diag_count == 2);
    ASSERT(missing_diag != NULL);
    ASSERT(missing_diag->code == XR_ERR_ANALYZE_UNDEFINED_VAR);
    ASSERT(missing_diag->next != NULL);
    ASSERT(missing_diag->next->code == XR_ERR_ANALYZE_UNDEFINED_VAR);
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count >= 1);
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

TEST(flow_binding_use_join_and_reassignment) {
    XaFlowBuilder *fb = xa_flow_builder_new();
    ASSERT(fb != NULL);
    XaFlowNode *start = xa_flow_create_start(fb);
    ASSERT(start != NULL);
    ASSERT(xa_flow_binding_use_state(fb, "payload", start) == XA_BINDING_LIVE);

    XaFlowNode *moved = xa_flow_create_move(fb, "payload");
    ASSERT(moved != NULL);
    ASSERT(xa_flow_binding_use_state(fb, "payload", moved) == XA_BINDING_MOVED);

    fb->current_flow = start;
    XaFlowNode *live = xa_flow_create_condition(fb, NULL, false);
    XaFlowNode *merge = xa_flow_create_branch_label(fb);
    xa_flow_add_antecedent(merge, moved);
    xa_flow_add_antecedent(merge, live);
    ASSERT(xa_flow_binding_use_state(fb, "payload", merge) == XA_BINDING_MAYBE_MOVED);

    fb->current_flow = merge;
    XaFlowNode *assigned = xa_flow_create_assignment(fb, NULL, "payload", xr_type_new_string(NULL));
    ASSERT(assigned != NULL);
    ASSERT(xa_flow_binding_use_state(fb, "payload", assigned) == XA_BINDING_LIVE);

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

TEST(type_function_throw_effect_covariance) {
    XrType *params[] = {xr_type_new_int(NULL)};
    XrType *may = xr_type_new_function(g_isolate, params, 1, xr_type_new_int(NULL), false);
    XrType *no = xr_type_copy(g_isolate, may);
    XrType *poly = xr_type_copy(g_isolate, may);
    ASSERT(may != NULL && no != NULL && poly != NULL);
    ASSERT(xr_type_function_set_throw_effect(no, XR_FN_EFFECT_NO_THROW));
    ASSERT(xr_type_function_set_throw_effect(poly, XR_FN_EFFECT_POLY));

    ASSERT(!xr_type_equals(may, no));
    ASSERT(!xr_type_equals(no, poly));
    ASSERT(xr_type_assignable(may, no));
    ASSERT(!xr_type_assignable(no, may));
    ASSERT(xr_type_assignable(no, poly));
    ASSERT(xr_type_assignable(may, poly));
    ASSERT(strstr(xr_type_to_string(no), "@") == NULL);
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
    ASSERT(xr_type_function_set_param_mode(fn, 0, XR_PARAM_READ));
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
    ASSERT(xr_type_function_param_mode(copy, 0) == XR_PARAM_READ);
    ASSERT(xr_type_function_param_mode(copy, 1) == XR_PARAM_REF);
    ASSERT(copy->function.is_c_abi);

    XrType *normal = xr_type_new_function(g_isolate, param_types, 2, xr_type_new_bool(NULL), false);
    ASSERT(!xr_type_equals(fn, normal));
    ASSERT(!xr_type_assignable(fn, normal));
    ASSERT(!xr_type_assignable(normal, fn));

    XrType *mode_only =
        xr_type_new_function(g_isolate, param_types, 2, xr_type_new_bool(NULL), false);
    ASSERT(mode_only != NULL);
    ASSERT(xr_type_function_set_param_mode(mode_only, 0, XR_PARAM_READ));
    ASSERT(xr_type_function_set_param_mode(mode_only, 1, XR_PARAM_REF));
    ASSERT(!xr_type_equals(mode_only, normal));
    ASSERT(!xr_type_assignable(mode_only, normal));
    ASSERT(!xr_type_assignable(normal, mode_only));
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
    AstNode *program =
        xr_parse(g_session, "type Handler = (int, ref string, move Array<bool>) -> int");
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
    ASSERT(XR_TYPE_IS_ARRAY(xr_type_function_param_type(fn, 2)));
    ASSERT(XR_TYPE_IS_BOOL(xr_type_function_param_type(fn, 2)->container.element_type));
    ASSERT(xr_type_function_param_mode(fn, 0) == XR_PARAM_READ);
    ASSERT(xr_type_function_param_mode(fn, 1) == XR_PARAM_REF);
    ASSERT(xr_type_function_param_mode(fn, 2) == XR_PARAM_MOVE);
    ASSERT(XR_TYPE_IS_INT(fn->function.return_type));
}

/* ========== L0 cycle-candidate marking ========== */

/* is_cycle_candidate is the ONLY truth about L0 marking. Runtime residue is a
 * downstream proxy that stops working once removal of the trial-deletion collector removes the cycle
 * collector, so these assert the flag directly. */
static bool analyzer_class_is_cycle_candidate(XaAnalyzer *analyzer, const char *class_name) {
    XaSymbol *sym = xa_analyzer_lookup(analyzer, class_name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, class_name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, class_name);
    if (!sym)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links && links->type && links->type->is_cycle_candidate;
}

static XaAnalyzer *analyzer_run_source(const char *file, const char *source) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    if (!a)
        return NULL;
    AstNode *program = xr_parse(g_session, source);
    if (!program) {
        xa_analyzer_free(a);
        return NULL;
    }
    xa_analyzer_analyze(a, file, program);
    return a;
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

static const XaAllocationSummary *analyzer_function_allocation_summary(XaAnalyzer *analyzer,
                                                                       const char *name) {
    XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, name);
    if (!sym || sym->links.alloc_effect_id == XA_ALLOC_EFFECT_NONE)
        return NULL;
    return xa_allocation_db_get(analyzer->allocation_db, sym->links.alloc_effect_id);
}

static const XaMemoryEffectSummary *analyzer_function_memory_effect_summary(XaAnalyzer *analyzer,
                                                                            const char *name) {
    XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, name);
    if (!sym || sym->links.memory_effect_id == XA_MEMORY_EFFECT_NONE)
        return NULL;
    return xa_memory_effect_db_get(analyzer->memory_effect_db, sym->links.memory_effect_id);
}

static const XaMemoryRootEffect *memory_effect_root(const XaMemoryEffectSummary *summary,
                                                    XaMemoryRootKind kind, uint32_t index) {
    if (!summary)
        return NULL;
    for (uint32_t i = 0; i < summary->root_count; i++) {
        if (summary->roots[i].root.kind == kind && summary->roots[i].root.index == index)
            return &summary->roots[i];
    }
    return NULL;
}

TEST(analyzer_inferred_unique_alias_nll_guards_move) {
    XaAnalyzer *bad = xa_analyzer_new(g_session);
    ASSERT(bad != NULL);
    const char *bad_source = "fn consume(xs: move Array<int>) -> int { return len(xs) }\n"
                             "fn bad() {\n"
                             "  var data = [1, 2]\n"
                             "  var alias = data\n"
                             "  var out = consume(move data)\n"
                             "  print(len(alias))\n"
                             "  print(out)\n"
                             "}\n";
    AstNode *bad_program = xr_parse(g_session, bad_source);
    ASSERT(bad_program != NULL);
    xa_analyzer_analyze(bad, "alias_live_move.xr", bad_program);
    XaSymbol *bad_data = xa_analyzer_lookup_deep(bad, "data");
    XaSymbol *bad_alias = xa_analyzer_lookup_deep(bad, "alias");
    ASSERT(bad_data != NULL && bad_alias != NULL);
    ASSERT(bad_data->links.root_id != 0);
    ASSERT(bad_data->links.root_id == bad_alias->links.root_id);
    ASSERT(analyzer_diag_contains(bad, "OWN-E-LIVE-ALIAS"));
    xr_program_destroy(bad_program);
    xa_analyzer_free(bad);

    XaAnalyzer *ok = xa_analyzer_new(g_session);
    ASSERT(ok != NULL);
    const char *ok_source = "fn consume(xs: move Array<int>) -> int { return len(xs) }\n"
                            "fn ok() {\n"
                            "  var data = [1, 2]\n"
                            "  var alias = data\n"
                            "  print(len(alias))\n"
                            "  var out = consume(move data)\n"
                            "  print(out)\n"
                            "}\n";
    AstNode *ok_program = xr_parse(g_session, ok_source);
    ASSERT(ok_program != NULL);
    xa_analyzer_analyze(ok, "alias_dead_move.xr", ok_program);
    ASSERT(!analyzer_diag_contains(ok, "OWN-E-LIVE-ALIAS"));
    ASSERT(!analyzer_diag_contains(ok, "used after move"));
    xr_program_destroy(ok_program);
    xa_analyzer_free(ok);
    setup_pool();
}

TEST(analyzer_parameter_effect_is_canonical_product) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source =
        "fn summarize(input: Array<int>, target: ref Array<int>, job: move Array<int>) "
        "-> Array<int> {\n"
        "  target.push(1)\n"
        "  var alias = input\n"
        "  return alias\n"
        "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "param_effect_product.xr", program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    XaSymbol *symbol = xa_analyzer_lookup_deep(a, "summarize");
    ASSERT(symbol != NULL);
    ASSERT(symbol->links.param_effects != NULL);
    ASSERT(symbol->links.param_effect_count == 3);

    const XaParamEffectSummary *read = &symbol->links.param_effects[0];
    const XaParamEffectSummary *ref = &symbol->links.param_effects[1];
    const XaParamEffectSummary *move = &symbol->links.param_effects[2];
    ASSERT(read->formal_mode == XR_PARAM_READ);
    ASSERT(read->capability == XA_CAPABILITY_READONLY);
    ASSERT(read->retain == XA_RETAIN_LOCAL_ALIAS);
    ASSERT((read->returns & XA_RETURN_PROVENANCE_ALIAS) != 0);
    ASSERT(ref->formal_mode == XR_PARAM_REF);
    ASSERT(ref->capability == XA_CAPABILITY_EXCLUSIVE_WRITE);
    ASSERT((ref->access & XA_PARAM_ACCESS_WRITE) != 0);
    ASSERT((ref->mutation_paths & XA_MUTATION_PATH_WILDCARD) != 0);
    ASSERT(move->formal_mode == XR_PARAM_MOVE);
    ASSERT(move->capability == XA_CAPABILITY_UNIQUE_OWNER);
    for (int i = 0; i < symbol->links.param_effect_count; i++) {
        ASSERT(symbol->links.param_effects[i].complete);
        ASSERT(symbol->links.param_effects[i].callable_effects == symbol->links.effect_id);
        ASSERT(symbol->links.param_effects[i].memory_effects == symbol->links.memory_effect_id);
    }

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_memory_effect_infers_and_instantiates_root_relative_facts) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source =
        "fn grow(data: ref Array<int>) { data.push(1) }\n"
        "fn growViaCall(data: ref Array<int>) { grow(data) }\n"
        "fn shrink(data: ref Array<int>) { data.pop() }\n"
        "fn rebind(data: ref Array<int>) { data = [1, 2] }\n"
        "fn dynamic(cb: (ref Array<int>) -> (), data: ref Array<int>) { cb(data) }\n"
        "fn readOnly(data: ref Array<int>) -> int { return data.length }\n"
        "fn sliceLen(data: Slice<byte>) -> int { return len(data) }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "memory_effect_summary.xr", program);
    ASSERT(!analyzer_diag_contains(a, "analysis resource failure"));

    const XaMemoryEffectSummary *grow = analyzer_function_memory_effect_summary(a, "grow");
    const XaMemoryEffectSummary *via = analyzer_function_memory_effect_summary(a, "growViaCall");
    const XaMemoryEffectSummary *shrink = analyzer_function_memory_effect_summary(a, "shrink");
    const XaMemoryEffectSummary *rebind = analyzer_function_memory_effect_summary(a, "rebind");
    const XaMemoryEffectSummary *dynamic = analyzer_function_memory_effect_summary(a, "dynamic");
    const XaMemoryEffectSummary *read_only = analyzer_function_memory_effect_summary(a, "readOnly");
    const XaMemoryEffectSummary *slice_len = analyzer_function_memory_effect_summary(a, "sliceLen");
    ASSERT(grow && via && shrink && rebind && dynamic && read_only && slice_len);

    const XaMemoryRootEffect *grow_root = memory_effect_root(grow, XA_MEMORY_ROOT_PARAM, 0);
    const XaMemoryRootEffect *via_root = memory_effect_root(via, XA_MEMORY_ROOT_PARAM, 0);
    const XaMemoryRootEffect *shrink_root = memory_effect_root(shrink, XA_MEMORY_ROOT_PARAM, 0);
    const XaMemoryRootEffect *rebind_root = memory_effect_root(rebind, XA_MEMORY_ROOT_PARAM, 0);
    const XaMemoryRootEffect *dynamic_root = memory_effect_root(dynamic, XA_MEMORY_ROOT_PARAM, 1);
    ASSERT(grow_root && grow_root->write_count == 1);
    ASSERT(grow_root->relocation == XA_MEMORY_MAY_RELOCATE);
    ASSERT(via_root && via_root->relocation == XA_MEMORY_MAY_RELOCATE);
    ASSERT(shrink_root && shrink_root->shortening == XA_MEMORY_MAY_SHORTEN);
    ASSERT(rebind_root && rebind_root->descriptor_rebind);
    ASSERT(xa_memory_effect_summary_is_complete(dynamic));
    ASSERT(dynamic_root && dynamic_root->invalidation == XA_MEMORY_INVALIDATES_VIEWS);
    ASSERT(read_only->root_count == 0);
    ASSERT(xa_memory_effect_summary_is_complete(slice_len));
    ASSERT(slice_len->root_count == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_mem_scalar_access_is_stable_for_pointer_owner_borrows) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "import mem\n"
                         "fn loadU64(p: Ptr<byte>) -> u64 {\n"
                         "  return unsafe { mem.load<u64>(p, 0, Endian.LE) }\n"
                         "}\n"
                         "fn storeU64(p: MutPtr<byte>, value: u64) {\n"
                         "  unsafe { mem.store<u64>(p, 0, value, Endian.LE) }\n"
                         "}\n"
                         "fn isPresent(p: Ptr<byte>) -> bool { return !p.isNull() }\n"
                         "fn exercise() -> u64 {\n"
                         "  var data = Array<byte>(8)\n"
                         "  var p = unsafe { data.mutPtr() }\n"
                         "  storeU64(p, 7)\n"
                         "  var read: Ptr<byte> = p\n"
                         "  return loadU64(read)\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "mem_scalar_pointer_borrow.xr", program);
    ASSERT(!analyzer_diag_contains(a, "incomplete view-invalidation effects"));
    ASSERT(!analyzer_diag_contains(a, "which invalidates views"));

    const XaMemoryEffectSummary *load = analyzer_function_memory_effect_summary(a, "loadU64");
    const XaMemoryEffectSummary *store = analyzer_function_memory_effect_summary(a, "storeU64");
    const XaMemoryEffectSummary *present = analyzer_function_memory_effect_summary(a, "isPresent");
    ASSERT(load && store && present);
    ASSERT(xa_memory_effect_summary_is_complete(load));
    ASSERT(xa_memory_effect_summary_is_complete(store));
    ASSERT(load->root_count == 0);
    ASSERT(xa_memory_effect_summary_is_complete(present));
    ASSERT(present->root_count == 0);
    const XaMemoryRootEffect *store_root = memory_effect_root(store, XA_MEMORY_ROOT_PARAM, 0);
    ASSERT(store_root && store_root->write_count == 1);
    ASSERT(store_root->invalidation == XA_MEMORY_NEVER_INVALIDATES);
    ASSERT(store_root->relocation == XA_MEMORY_ADDRESS_STABLE);
    ASSERT(store_root->shortening == XA_MEMORY_NEVER_SHORTENS);
    ASSERT(!store_root->descriptor_rebind);

    xr_program_destroy(program);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(symbol_export_metadata_reinterns_analyzer_local_sidecars) {
    XaAnalyzer *source = xa_analyzer_new(g_session);
    XaAnalyzer *target = xa_analyzer_new(g_session);
    ASSERT(source != NULL && target != NULL);
    XaSymbol *source_symbol = xa_symbol_new("foreignRead", XA_SYM_FUNCTION);
    XaSymbol *target_symbol = xa_symbol_new("foreignRead", XA_SYM_IMPORT);
    ASSERT(source_symbol != NULL && target_symbol != NULL);
    XaSymbolLinks *source_links = xa_analyzer_get_links(source, source_symbol);
    XaSymbolLinks *target_links = xa_analyzer_get_links(target, target_symbol);

    XaEffectSummary effect;
    xa_effect_summary_init(&effect);
    xa_effect_summary_add_semantic_effects(&effect, XA_SEM_EFFECT_FOREIGN | XA_SEM_EFFECT_IO);
    source_links->effect_id = xa_effect_db_intern(source->effect_db, &effect);
    xa_effect_summary_clear(&effect);
    ASSERT(source_links->effect_id != XA_EFFECT_NONE);

    XaMemoryEffectSummary source_memory;
    xa_memory_effect_summary_init(&source_memory);
    source_links->memory_effect_id =
        xa_memory_effect_db_intern(source->memory_effect_db, &source_memory);
    xa_memory_effect_summary_clear(&source_memory);
    ASSERT(source_links->memory_effect_id != XA_MEMORY_EFFECT_NONE);
    source_links->return_ownership = (XaReturnOwnershipSummary) {
        .kind = XA_RETURN_OWNERSHIP_BORROWED_PARAM, .param_index = 3, .complete = true};
    source_links->return_ownership_scanned = true;
    xa_symbol_links_set_deprecated(source_links, true, "use safeRead");

    /* Occupy the same target-local numeric id with an incompatible summary.
     * Import must re-intern the source semantics instead of copying that id. */
    XaMemoryEffectSummary collision;
    xa_memory_effect_summary_init(&collision);
    ASSERT(xa_memory_effect_summary_mark_invalidation(
        &collision, (XaMemoryRootRef) {.kind = XA_MEMORY_ROOT_PARAM, .index = 0}));
    ASSERT(xa_memory_effect_db_intern(target->memory_effect_db, &collision) ==
           source_links->memory_effect_id);
    xa_memory_effect_summary_clear(&collision);

    /* A graph namespace import may temporarily consume the exported links
     * directly. Its id must still be resolved in the exporting analyzer. */
    const XaMemoryEffectSummary *foreign_memory =
        xa_symbol_links_memory_effect_summary(source_links);
    ASSERT(foreign_memory != NULL);
    ASSERT(foreign_memory->root_count == 0);

    xa_symbol_links_copy_export_metadata(target, target_links, source_links);
    const XaEffectSummary *imported_effect =
        xa_effect_db_get(target->effect_db, target_links->effect_id);
    const XaMemoryEffectSummary *imported_memory =
        xa_memory_effect_db_get(target->memory_effect_db, target_links->memory_effect_id);
    ASSERT(imported_effect != NULL);
    ASSERT(xa_effect_summary_has_semantic_effect(imported_effect, XA_SEM_EFFECT_FOREIGN));
    ASSERT(xa_effect_summary_has_semantic_effect(imported_effect, XA_SEM_EFFECT_IO));
    ASSERT(imported_memory != NULL);
    ASSERT(imported_memory->root_count == 0);
    ASSERT(target_links->return_ownership_scanned);
    ASSERT(target_links->return_ownership.complete);
    ASSERT(target_links->return_ownership.kind == XA_RETURN_OWNERSHIP_BORROWED_PARAM);
    ASSERT(target_links->return_ownership.param_index == 3);
    ASSERT(target_links->is_deprecated);
    ASSERT(target_links->deprecated_message != NULL);
    ASSERT(strcmp(target_links->deprecated_message, "use safeRead") == 0);

    xa_symbol_free(target_symbol);
    xa_symbol_free(source_symbol);
    xa_analyzer_free(target);
    xa_analyzer_free(source);
    setup_pool();
}

TEST(analyzer_slice_mutator_effect_is_independent_of_discarded_result) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn keepResult(dst: ref Slice<u32>) -> int {\n"
                         "  var filled: Slice<u32> = dst.fill(7)\n"
                         "  return len(filled)\n"
                         "}\n"
                         "fn discardResult(dst: ref Slice<u32>) -> int {\n"
                         "  dst.fill(0)\n"
                         "  return len(dst)\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "slice_mutator_memory_effect.xr", program);
    ASSERT(!analyzer_diag_contains(a, "analysis resource failure"));

    const XaMemoryEffectSummary *keep = analyzer_function_memory_effect_summary(a, "keepResult");
    const XaMemoryEffectSummary *discard =
        analyzer_function_memory_effect_summary(a, "discardResult");
    ASSERT(keep && discard);
    ASSERT(xa_memory_effect_summary_is_complete(keep));
    ASSERT(xa_memory_effect_summary_is_complete(discard));
    const XaMemoryRootEffect *keep_root = memory_effect_root(keep, XA_MEMORY_ROOT_PARAM, 0);
    const XaMemoryRootEffect *discard_root = memory_effect_root(discard, XA_MEMORY_ROOT_PARAM, 0);
    ASSERT(keep_root && keep_root->write_count == 1);
    ASSERT(discard_root && discard_root->write_count == 1);
    ASSERT(keep_root->invalidation == XA_MEMORY_NEVER_INVALIDATES);
    ASSERT(discard_root->invalidation == XA_MEMORY_NEVER_INVALIDATES);
    ASSERT(!keep_root->descriptor_rebind && !discard_root->descriptor_rebind);

    xr_program_destroy(program);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_canonical_effect_product_publishes_suspend_fixpoint) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn worker() -> int { return 1 }\n"
                         "fn suspends() { var task = go worker(); await task }\n"
                         "fn transitive() { suspends() }\n"
                         "fn dynamic(cb: () -> ()) { cb() }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "canonical_suspend_effect.xr", program);
    ASSERT(!analyzer_diag_contains(a, "analysis resource failure"));

    const XaEffectSummary *worker = analyzer_function_effect_summary(a, "worker");
    const XaEffectSummary *suspends = analyzer_function_effect_summary(a, "suspends");
    const XaEffectSummary *transitive = analyzer_function_effect_summary(a, "transitive");
    const XaEffectSummary *dynamic = analyzer_function_effect_summary(a, "dynamic");
    ASSERT(worker && suspends && transitive && dynamic);
    ASSERT(!xa_effect_summary_has_semantic_effect(worker, XA_SEM_EFFECT_SCHED_SUSPEND));
    ASSERT(xa_effect_summary_has_semantic_effect(suspends, XA_SEM_EFFECT_SCHED_SUSPEND));
    ASSERT(xa_effect_summary_has_semantic_effect(transitive, XA_SEM_EFFECT_SCHED_SUSPEND));
    ASSERT((dynamic->unknown_semantic_effects & XA_SEM_EFFECT_SCHED_SUSPEND) != 0);
    ASSERT(dynamic->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((dynamic->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);

    xa_analyzer_free(a);
    setup_pool();
}

/* `yield expr` hands control to the iterator driving the generator; it never
 * reaches the scheduler.  Publishing it as one undifferentiated "suspend" made
 * every generator, and every function that merely drives one, fail `no_suspend`
 * and be treated as a cancellation point. */
TEST(analyzer_generator_suspend_is_separate_from_scheduler_suspend) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn counter(n: int) -> Iterator<int> {\n"
                         "    for (var i = 0; i < n; i++) { yield i }\n"
                         "}\n"
                         "fn drive() -> int {\n"
                         "    var sum = 0\n"
                         "    for (x in counter(3)) { sum = sum + x }\n"
                         "    return sum\n"
                         "}\n"
                         "fn asyncGen() -> Iterator<int> {\n"
                         "    var task = go drive()\n"
                         "    yield await task\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "generator_suspend_effect.xr", program);
    ASSERT(!analyzer_diag_contains(a, "analysis resource failure"));

    const XaEffectSummary *counter = analyzer_function_effect_summary(a, "counter");
    const XaEffectSummary *drive = analyzer_function_effect_summary(a, "drive");
    const XaEffectSummary *async_gen = analyzer_function_effect_summary(a, "asyncGen");
    ASSERT(counter && drive && async_gen);

    /* A pure generator: generator suspension only. */
    ASSERT(xa_effect_summary_has_semantic_effect(counter, XA_SEM_EFFECT_GEN_SUSPEND));
    ASSERT(!xa_effect_summary_has_semantic_effect(counter, XA_SEM_EFFECT_SCHED_SUSPEND));
    ASSERT((counter->unknown_semantic_effects & XA_SEM_EFFECT_SCHED_SUSPEND) == 0);

    /* Driving a generator resumes the generator's frame, not the driver's, so
     * neither dimension propagates to the caller. */
    ASSERT(!xa_effect_summary_has_semantic_effect(drive, XA_SEM_EFFECT_GEN_SUSPEND));
    ASSERT(!xa_effect_summary_has_semantic_effect(drive, XA_SEM_EFFECT_SCHED_SUSPEND));

    /* A generator that also awaits publishes both, independently. */
    ASSERT(xa_effect_summary_has_semantic_effect(async_gen, XA_SEM_EFFECT_GEN_SUSPEND));
    ASSERT(xa_effect_summary_has_semantic_effect(async_gen, XA_SEM_EFFECT_SCHED_SUSPEND));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_allocation_effect_propagates_and_validates_contracts) {
    XaAllocationSummary fingerprint_a = {
        .state = XA_ALLOC_MAY,
        .reason_bits = XA_ALLOC_REASON_CONTAINER,
        .first_site_node_id = 7,
        .first_callee_symbol_id = 11,
        .line = 42,
        .column = 3,
        .callee_effect_id = 5,
        .cause_kind = "literal",
        .cause_detail = "Array",
        .callee_name = "allocateLeaf",
    };
    XaAllocationSummary fingerprint_b = fingerprint_a;
    fingerprint_b.first_site_node_id = 7001;
    fingerprint_b.first_callee_symbol_id = 9001;
    fingerprint_b.callee_effect_id = 81;
    ASSERT(xa_allocation_summary_fingerprint(&fingerprint_a) ==
           xa_allocation_summary_fingerprint(&fingerprint_b));
    fingerprint_b.line++;
    ASSERT(xa_allocation_summary_fingerprint(&fingerprint_a) !=
           xa_allocation_summary_fingerprint(&fingerprint_b));

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "import mem\n"
                         "fn scalar(x: int) -> int { return x + 1 }\n"
                         "fn cycleA(n: int) -> int {\n"
                         "  if (n <= 0) { return 0 }\n"
                         "  return cycleB(n - 1)\n"
                         "}\n"
                         "fn cycleB(n: int) -> int {\n"
                         "  if (n <= 0) { return 0 }\n"
                         "  return cycleA(n - 1)\n"
                         "}\n"
                         "fn cycleEntry(n: int) -> int { return cycleA(n) }\n"
                         "fn allocateLeaf() { var values = [1, 2, 3] }\n"
                         "fn twoHop() { allocateLeaf() }\n"
                         "fn unknownCall(cb: () -> ()) { cb() }\n"
                         "fn callbackOk(xs: Array<int>) {\n"
                         "  xs.forEach(fn(value: int, index: int) { var sum = value + index })\n"
                         "}\n"
                         "fn callbackBad(xs: Array<int>) {\n"
                         "  xs.forEach(fn(value: int, index: int) { var copy = [value, index] })\n"
                         "}\n"
                         "fn slicePointerViews(data: Slice<byte>) {\n"
                         "  unsafe {\n"
                         "    var readPtr = data.ptr()\n"
                         "    var writePtr = data.mutPtr()\n"
                         "  }\n"
                         "}\n"
                         "fn pointerOffsetNoHeap(data: Ptr<byte>, write: MutPtr<byte>) {\n"
                         "  var nextRead = data.offset(1)\n"
                         "  var nextWrite = write.offset(1)\n"
                         "}\n"
                         "fn rawSliceProjection(data: Ptr<byte>, count: int) -> Slice<byte> {\n"
                         "  return unsafe { mem.slice<byte>(data, count, data) }\n"
                         "}\n"
                         "enum ValueError { Bad(actual: int, minimum: int) }\n"
                         "fn fixedValueCopy(data: [byte; 4]) -> [byte; 4] {\n"
                         "  return copy(data)\n"
                         "}\n"
                         "fn valueError(actual: int) {\n"
                         "  if (actual < 4) { throw ValueError.Bad(actual, 4) }\n"
                         "}\n"
                         "struct Counter {\n"
                         "  value: int\n"
                         "  read() -> int { return this.value }\n"
                         "  bad() { var copy = [this.value] }\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "allocation_effect_contracts.xr", program);

    const XaAllocationSummary *scalar = analyzer_function_allocation_summary(a, "scalar");
    const XaAllocationSummary *raw_slice =
        analyzer_function_allocation_summary(a, "rawSliceProjection");
    const XaAllocationSummary *cycle_entry = analyzer_function_allocation_summary(a, "cycleEntry");
    const XaAllocationSummary *leaf = analyzer_function_allocation_summary(a, "allocateLeaf");
    const XaAllocationSummary *two_hop = analyzer_function_allocation_summary(a, "twoHop");
    const XaAllocationSummary *unknown = analyzer_function_allocation_summary(a, "unknownCall");
    const XaAllocationSummary *callback_ok = analyzer_function_allocation_summary(a, "callbackOk");
    const XaAllocationSummary *callback_bad =
        analyzer_function_allocation_summary(a, "callbackBad");
    const XaAllocationSummary *slice_pointer_views =
        analyzer_function_allocation_summary(a, "slicePointerViews");
    const XaAllocationSummary *pointer_offset_noheap =
        analyzer_function_allocation_summary(a, "pointerOffsetNoHeap");
    const XaAllocationSummary *fixed_value_copy =
        analyzer_function_allocation_summary(a, "fixedValueCopy");
    const XaAllocationSummary *value_error = analyzer_function_allocation_summary(a, "valueError");
    const XaEffectSummary *scalar_effect = analyzer_function_effect_summary(a, "scalar");
    const XaEffectSummary *leaf_effect = analyzer_function_effect_summary(a, "allocateLeaf");
    const XaEffectSummary *two_hop_effect = analyzer_function_effect_summary(a, "twoHop");
    const XaEffectSummary *unknown_effect = analyzer_function_effect_summary(a, "unknownCall");
    ASSERT(scalar && scalar->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(raw_slice && raw_slice->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(cycle_entry && cycle_entry->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(leaf && leaf->state == XA_ALLOC_MAY);
    ASSERT(two_hop && two_hop->state == XA_ALLOC_MAY);
    ASSERT(unknown && unknown->state == XA_ALLOC_UNKNOWN);
    ASSERT(callback_ok && callback_ok->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(callback_bad && callback_bad->state == XA_ALLOC_MAY);
    ASSERT(slice_pointer_views && slice_pointer_views->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(pointer_offset_noheap && pointer_offset_noheap->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(fixed_value_copy && fixed_value_copy->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(value_error && value_error->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(scalar_effect && leaf_effect && two_hop_effect && unknown_effect);
    ASSERT(!xa_effect_summary_has_semantic_effect(scalar_effect, XA_SEM_EFFECT_ALLOC));
    ASSERT(xa_effect_summary_has_semantic_effect(leaf_effect, XA_SEM_EFFECT_ALLOC));
    ASSERT(xa_effect_summary_has_semantic_effect(two_hop_effect, XA_SEM_EFFECT_ALLOC));
    ASSERT((unknown_effect->unknown_semantic_effects & XA_SEM_EFFECT_ALLOC) != 0);
    ASSERT(scalar->stable_fingerprint != 0);
    /* Effect facts are inferred unconditionally. Guarantee failures are now
     * produced by `xray verify`, not by source attributes. */

    xa_analyzer_free(a);
    setup_pool();
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

static bool write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    if (content)
        fputs(content, f);
    return fclose(f) == 0;
}

static XaSymbol *analyzer_function_symbol(XaAnalyzer *analyzer, const char *name) {
    XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, name);
    return sym;
}

/* Task 216 P0 invariant: the typed throw-effect bit published on each function
 * symbol (and mirrored onto its function type) must agree with the analyzer's
 * effect summary — NO_THROW iff the summary is complete with an empty escaping
 * set, MAY_THROW otherwise (fail-closed). Pass expected = -1 to check only the
 * bit<->summary and symbol<->type consistency without pinning an expectation.
 * This guards the publication path against drift between the effect DB (the set
 * truth source) and the type-system bit. */
static void check_throw_effect_consistency(XaAnalyzer *a, const char *name, int expected) {
    XaSymbol *sym = analyzer_function_symbol(a, name);
    if (!sym) {
        printf("FAILED: throw-effect symbol '%s' not found\n", name);
        tests_failed++;
        return;
    }
    const XaEffectSummary *summary = analyzer_function_effect_summary(a, name);
    XrFnThrowEffect from_summary =
        xa_effect_summary_is_nothrow(summary) ? XR_FN_EFFECT_NO_THROW : XR_FN_EFFECT_MAY_THROW;
    if (sym->links.throw_effect != from_summary) {
        printf("FAILED: '%s' symbol throw bit %d != summary-derived %d\n", name,
               (int) sym->links.throw_effect, (int) from_summary);
        tests_failed++;
        return;
    }
    if (sym->links.type && sym->links.type->kind == XR_KIND_FUNCTION &&
        sym->links.type->function.throw_effect != sym->links.throw_effect) {
        printf("FAILED: '%s' function-type throw bit %d != symbol bit %d\n", name,
               (int) sym->links.type->function.throw_effect, (int) sym->links.throw_effect);
        tests_failed++;
        return;
    }
    if (expected >= 0 && sym->links.throw_effect != (XrFnThrowEffect) expected) {
        printf("FAILED: '%s' throw effect %d != expected %d\n", name, (int) sym->links.throw_effect,
               expected);
        tests_failed++;
        return;
    }
}

TEST(analyzer_throw_effect_bit_matches_effect_summary) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum IoErr { Boom }\n"
                         "fn pureAdd(x: int, y: int) -> int { return x + y }\n"
                         "fn throwsDirect() { throw IoErr.Boom }\n"
                         "fn propagates() { throwsDirect() }\n"
                         "fn guarded() { try { throwsDirect() } catch (e: IoErr) {} }\n"
                         "fn viaDynamic(f: () -> ()) { f() }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "throw_effect_bit.xr", program);

    /* Pure arithmetic: proven NO_THROW. */
    check_throw_effect_consistency(a, "pureAdd", XR_FN_EFFECT_NO_THROW);
    /* Direct throw and its transitive propagation: MAY_THROW. */
    check_throw_effect_consistency(a, "throwsDirect", XR_FN_EFFECT_MAY_THROW);
    check_throw_effect_consistency(a, "propagates", XR_FN_EFFECT_MAY_THROW);
    /* Catch subtraction that fully consumes the escaping set: check the
     * invariant without pinning (behaviour owned by the error-set pass). */
    check_throw_effect_consistency(a, "guarded", -1);
    /* Dynamic (function-value) call is incomplete → fail-closed MAY_THROW. */
    check_throw_effect_consistency(a, "viaDynamic", XR_FN_EFFECT_MAY_THROW);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_inferred_effects_accept_function_values) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn increment(value: int) -> int { return value + 1 }\n"
                         "fn applyPure(callback: (int) -> int, value: int) -> int {\n"
                         "  return callback(value)\n"
                         "}\n"
                         "fn main() -> int {\n"
                         "  const local: (int) -> int = fn(value: int) -> int {\n"
                         "    return value * 2\n"
                         "  }\n"
                         "  return applyPure(increment, local(2))\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "no_throw_constraints.xr", program);
    int diagnostic_count = 0;
    xa_analyzer_get_diagnostics(a, &diagnostic_count);
    ASSERT(diagnostic_count == 0);
    check_throw_effect_consistency(a, "increment", XR_FN_EFFECT_NO_THROW);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_codegen_controls_are_semantic_neutral_and_type_closed) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    AstNode *program = xr_parse(g_session, "import codegen\n"
                                           "fn guarded(value: u64, pointer: Ptr<byte>) -> u64 {\n"
                                           "  var hidden = codegen.opaque(value)\n"
                                           "  var samePointer = codegen.opaque(pointer)\n"
                                           "  codegen.compilerFence()\n"
                                           "  return hidden\n"
                                           "}\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "codegen_controls_positive.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);
    const XaEffectSummary *effects = analyzer_function_effect_summary(a, "guarded");
    const XaMemoryEffectSummary *memory = analyzer_function_memory_effect_summary(a, "guarded");
    const XaAllocationSummary *allocation = analyzer_function_allocation_summary(a, "guarded");
    ASSERT(effects != NULL && effects->semantic_effects == 0);
    ASSERT(memory != NULL && xa_memory_effect_summary_is_complete(memory) &&
           memory->root_count == 0);
    ASSERT(allocation != NULL && allocation->state == XA_ALLOC_PROVEN_NONE);
    xa_analyzer_free(a);
    setup_pool();

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "import codegen\n"
                                  "var text = codegen.opaque(\"x\")\n"
                                  "var floating = codegen.opaque(1.25)\n"
                                  "var truth = codegen.opaque(true)\n"
                                  "var aggregate = codegen.opaque([1, 2])\n"
                                  "var nullable: u64? = null\n"
                                  "var maybe = codegen.opaque(nullable)\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "codegen_controls_negative.xr", program);
    ASSERT(analyzer_diag_contains(a, "codegen.opaque accepts only non-null integer scalars"));
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count >= 5);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_call_context_accepts_u64_only_literals) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn take(value: u64) -> u64 { return value }\n"
                         "fn check(got: u64, want: u64) { print(got == want) }\n"
                         "check(take(11400714785074694791), 15845353736191888555)\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "contextual_u64_call.xr", program);
    int diagnostic_count = 0;
    xa_analyzer_get_diagnostics(a, &diagnostic_count);
    ASSERT(diagnostic_count == 0);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_effect_inference_handles_redundant_try_catch) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn guarded(value: int) -> int {\n"
                         "  try {\n"
                         "    return value + 1\n"
                         "  } catch (e) {\n"
                         "    return value\n"
                         "  }\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "no_throw_redundant_try.xr", program);
    int diagnostic_count = 0;
    xa_analyzer_get_diagnostics(a, &diagnostic_count);
    ASSERT(diagnostic_count == 0);
    check_throw_effect_consistency(a, "guarded", XR_FN_EFFECT_NO_THROW);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_deprecated_message_reaches_use_diagnostic) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "@deprecated(\"use modern\")\n"
                         "fn legacy(value: int) -> int { return value }\n"
                         "fn caller() -> int { return legacy(1) }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "deprecated_message.xr", program);
    XaSymbol *legacy = analyzer_function_symbol(a, "legacy");
    ASSERT(legacy != NULL);
    ASSERT(legacy->links.is_deprecated);
    ASSERT(legacy->links.deprecated_message != NULL);
    ASSERT(strcmp(legacy->links.deprecated_message, "use modern") == 0);
    ASSERT(analyzer_diag_contains(a, "use of deprecated declaration 'legacy': use modern"));
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_stored_function_value_defaults_may_throw) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "enum StoredEffectErr { Boom }\n"
                         "fn pure(value: int) -> int { return value + 1 }\n"
                         "fn checked(value: int) -> int {\n"
                         "  if (value < 0) { throw StoredEffectErr.Boom }\n"
                         "  return value\n"
                         "}\n"
                         "var callback = pure\n"
                         "callback = checked\n"
                         "print(callback(1))\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "stored_function_effect.xr", program);
    int diagnostic_count = 0;
    xa_analyzer_get_diagnostics(a, &diagnostic_count);
    ASSERT(diagnostic_count == 0);

    XaSymbol *callback = analyzer_function_symbol(a, "callback");
    ASSERT(callback != NULL);
    ASSERT(callback->links.type != NULL);
    ASSERT(callback->links.type->kind == XR_KIND_FUNCTION);
    ASSERT(callback->links.type->function.throw_effect == XR_FN_EFFECT_MAY_THROW);
    check_throw_effect_consistency(a, "pure", XR_FN_EFFECT_NO_THROW);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_generic_hof_splits_throw_effect_dimension) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source =
        "enum HofEffectErr { Boom }\n"
        "fn apply<T>(callback: (T) -> T, value: T) -> T { return callback(value) }\n"
        "fn plusOne(value: int) -> int { return value + 1 }\n"
        "fn plusTwo(value: int) -> int { return value + 2 }\n"
        "fn checked(value: int) -> int {\n"
        "  if (value < 0) { throw HofEffectErr.Boom }\n"
        "  return value\n"
        "}\n"
        "fn viaPureOne(value: int) -> int { return apply<int>(plusOne, value) }\n"
        "fn viaPureTwo(value: int) -> int { return apply<int>(plusTwo, value) }\n"
        "fn viaChecked(value: int) -> int { return apply<int>(checked, value) }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);

    xa_analyzer_analyze(a, "hof_effect_specialization.xr", program);
    xa_mono_pass(program, NULL, 0, g_isolate, a);
    xa_analyzer_analyze(a, "hof_effect_specialization.xr", program);

    int diagnostic_count = 0;
    xa_analyzer_get_diagnostics(a, &diagnostic_count);
    ASSERT(diagnostic_count == 0);
    check_throw_effect_consistency(a, "apply$i64$nothrow", XR_FN_EFFECT_NO_THROW);
    check_throw_effect_consistency(a, "apply$i64", XR_FN_EFFECT_MAY_THROW);
    check_throw_effect_consistency(a, "viaPureOne", XR_FN_EFFECT_NO_THROW);
    check_throw_effect_consistency(a, "viaPureTwo", XR_FN_EFFECT_NO_THROW);
    check_throw_effect_consistency(a, "viaChecked", XR_FN_EFFECT_MAY_THROW);

    const XaEffectSummary *checked = analyzer_function_effect_summary(a, "apply$i64");
    ASSERT(checked != NULL);
    ASSERT(effect_summary_has_enum_named(a, checked, "HofEffectErr"));

    xa_analyzer_free(a);
    setup_pool();
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
    ASSERT(higher_order_callback != NULL);
    ASSERT(higher_order_function_expr != NULL);
    ASSERT(higher_order_union != NULL);
    ASSERT(higher_order_unknown != NULL);
    ASSERT(const_alias != NULL);

    ASSERT(effect_summary_has_enum_named(a, stable_var, "DynamicErr"));
    ASSERT((stable_var->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_chain, "DynamicErr"));
    ASSERT((stable_chain->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(rebound_var->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((rebound_var->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, rebound_var, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, rebound_to_throwing, "DynamicErr"));
    ASSERT((rebound_to_throwing->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(unknown_rebound->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((unknown_rebound->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, unknown_rebound, "DynamicErr"));
    ASSERT(conditional_rebound->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((conditional_rebound->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, conditional_rebound, "DynamicErr"));
    ASSERT(if_else_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((if_else_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, if_else_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, if_else_union, "OtherDynamicErr"));
    ASSERT(conditional_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((conditional_unknown->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(while_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((while_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, while_union, "DynamicErr"));
    ASSERT(for_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, for_union, "DynamicErr"));
    ASSERT(for_increment_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_increment_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, for_increment_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, for_increment_union, "OtherDynamicErr"));
    ASSERT(for_in_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((for_in_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, for_in_union, "DynamicErr"));
    ASSERT(loop_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((loop_unknown->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(try_catch_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((try_catch_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, try_catch_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, try_catch_union, "OtherDynamicErr"));
    ASSERT(try_catch_base_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((try_catch_base_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, try_catch_base_union, "DynamicErr"));
    ASSERT(try_catch_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((try_catch_unknown->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(try_mutated_catch_read->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((try_mutated_catch_read->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(returned_function_value->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, returned_function_value, "OtherDynamicErr"));
    ASSERT(returned_function_value_direct->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value_direct->error_unknown_reasons &
            XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_direct, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_direct, "OtherDynamicErr"));
    ASSERT(returned_function_value_base->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((returned_function_value_base->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) ==
           0);
    ASSERT(effect_summary_has_enum_named(a, returned_function_value_base, "DynamicErr"));
    ASSERT(!effect_summary_has_enum_named(a, returned_function_value_base, "OtherDynamicErr"));
    ASSERT(returned_function_value_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((returned_function_value_unknown->error_unknown_reasons &
            XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(captured_function_value->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((captured_function_value->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, captured_function_value, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, captured_function_value, "OtherDynamicErr"));
    ASSERT(captured_function_value_direct->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((captured_function_value_direct->error_unknown_reasons &
            XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, captured_function_value_direct, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, captured_function_value_direct, "OtherDynamicErr"));
    ASSERT(captured_function_value_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((captured_function_value_unknown->error_unknown_reasons &
            XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, captured_function_value_unknown, "DynamicErr"));
    ASSERT(higher_order_callback->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_callback->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, higher_order_callback, "DynamicErr"));
    ASSERT(!effect_summary_has_enum_named(a, higher_order_callback, "OtherDynamicErr"));
    ASSERT(higher_order_function_expr->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_function_expr->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) ==
           0);
    ASSERT(!effect_summary_has_enum_named(a, higher_order_function_expr, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, higher_order_function_expr, "OtherDynamicErr"));
    ASSERT(higher_order_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((higher_order_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, higher_order_union, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, higher_order_union, "OtherDynamicErr"));
    ASSERT(higher_order_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((higher_order_unknown->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, higher_order_unknown, "DynamicErr"));
    ASSERT(effect_summary_has_enum_named(a, const_alias, "DynamicErr"));
    ASSERT((const_alias->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_generic_specialization_target_sets) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source =
        "enum GenericIntErr { Boom }\n"
        "enum GenericOtherIntErr { Boom }\n"
        "enum GenericStringErr { Boom }\n"
        "fn failGenericInt(x: int) -> int { throw GenericIntErr.Boom }\n"
        "fn failGenericOtherInt(x: int) -> int { throw GenericOtherIntErr.Boom }\n"
        "fn failGenericString(x: string) -> string { throw GenericStringErr.Boom }\n"
        "fn runGeneric<T>(x: T, cb: (T) -> T) -> T {\n"
        "  return cb(x)\n"
        "}\n"
        "fn viaGenericInt() {\n"
        "  runGeneric<int>(1, failGenericInt)\n"
        "}\n"
        "fn viaGenericIntOther() {\n"
        "  runGeneric<int>(2, failGenericOtherInt)\n"
        "}\n"
        "fn viaGenericString() {\n"
        "  runGeneric<string>(\"x\", failGenericString)\n"
        "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);

    xa_analyzer_analyze(a, "effect_generic_specialization.xr", program);
    ASSERT(!analyzer_diag_contains(a, "error"));
    xa_mono_pass(program, NULL, 0, g_isolate, a);
    xa_analyzer_analyze(a, "effect_generic_specialization.xr", program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    const XaEffectSummary *generic_int = analyzer_function_effect_summary(a, "viaGenericInt");
    const XaEffectSummary *generic_int_other =
        analyzer_function_effect_summary(a, "viaGenericIntOther");
    const XaEffectSummary *generic_string = analyzer_function_effect_summary(a, "viaGenericString");
    const XaEffectSummary *specialized_int = analyzer_function_effect_summary(a, "runGeneric$i64");
    const XaEffectSummary *specialized_string =
        analyzer_function_effect_summary(a, "runGeneric$str");
    ASSERT(generic_int != NULL);
    ASSERT(generic_int_other != NULL);
    ASSERT(generic_string != NULL);
    ASSERT(specialized_int != NULL);
    ASSERT(specialized_string != NULL);

    ASSERT(generic_int->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((generic_int->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, generic_int, "GenericIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, generic_int, "GenericOtherIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, generic_int, "GenericStringErr"));
    ASSERT(generic_int_other->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((generic_int_other->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, generic_int_other, "GenericIntErr"));
    ASSERT(effect_summary_has_enum_named(a, generic_int_other, "GenericOtherIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, generic_int_other, "GenericStringErr"));
    ASSERT(generic_string->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((generic_string->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, generic_string, "GenericIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, generic_string, "GenericOtherIntErr"));
    ASSERT(effect_summary_has_enum_named(a, generic_string, "GenericStringErr"));
    ASSERT(specialized_int->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((specialized_int->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, specialized_int, "GenericIntErr"));
    ASSERT(effect_summary_has_enum_named(a, specialized_int, "GenericOtherIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, specialized_int, "GenericStringErr"));
    ASSERT(specialized_string->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((specialized_string->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, specialized_string, "GenericIntErr"));
    ASSERT(!effect_summary_has_enum_named(a, specialized_string, "GenericOtherIntErr"));
    ASSERT(effect_summary_has_enum_named(a, specialized_string, "GenericStringErr"));

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
    ASSERT((immediate_throw->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, immediate_call, "LambdaErr"));
    ASSERT((immediate_call->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, const_stored, "LambdaErr"));
    ASSERT((const_stored->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, const_chain, "LambdaErr"));
    ASSERT((const_chain->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_stored, "LambdaErr"));
    ASSERT((stable_stored->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, stable_chain, "LambdaErr"));
    ASSERT((stable_chain->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(rebound->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((rebound->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, rebound, "LambdaErr"));
    ASSERT(effect_summary_has_enum_named(a, rebound_to_throwing, "LambdaErr"));
    ASSERT((rebound_to_throwing->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(unknown_rebound->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((unknown_rebound->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, unknown_rebound, "LambdaErr"));
    ASSERT(conditional_rebound->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((conditional_rebound->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, conditional_rebound, "LambdaErr"));

    xa_analyzer_free(a);
    setup_pool();
}

/* These six field shapes all reach a class. Every one of
 * these was a MISSED mark before — an unmarked class never becomes a cycle
 * candidate, so neither the collector nor the development detector can
 * see its cycles. */
TEST(cycle_candidate_marks_every_field_shape) {
    XaAnalyzer *a = analyzer_run_source("cycle_field_shapes.xr",
                                        /* direct: was already marked */
                                        "class DirectA { peer: DirectB? }\n"
                                        "class DirectB { peer: DirectA? }\n"
                                        /* Array element: was already marked */
                                        "class ArrA { peers: Array<ArrB> }\n"
                                        "class ArrB { peers: Array<ArrA> }\n"
                                        /* Map value: G1, produced no edge */
                                        "class MapA { peers: Map<string, MapB> }\n"
                                        "class MapB { peers: Map<string, MapA> }\n"
                                        /* Map key: G1, produced no edge */
                                        "class KeyA { peers: Map<KeyB, int> }\n"
                                        "class KeyB { peers: Map<KeyA, int> }\n"
                                        /* Set element: G1, produced no edge */
                                        "class SetA { peers: Set<SetB> }\n"
                                        "class SetB { peers: Set<SetA> }\n"
                                        /* union beyond the first member: G3 */
                                        "class UniA { peer: int | string | UniB | null }\n"
                                        "class UniB { peer: int | string | UniA | null }\n"
                                        /* tuple member */
                                        "class TupA { peer: (int, TupB)? }\n"
                                        "class TupB { peer: (int, TupA)? }\n");
    ASSERT(a != NULL);

    ASSERT(analyzer_class_is_cycle_candidate(a, "DirectA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "DirectB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "ArrA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "ArrB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "MapA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "MapB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "KeyA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "KeyB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "SetA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "SetB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "UniA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "UniB"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "TupA"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "TupB"));

    xa_analyzer_free(a);
    setup_pool();
}

/* `weak` is the L0/L1 interface. A weak field does not keep
 * its target alive, so it cannot close a cycle and must produce no edge —
 * annotating one is exactly how a user takes their class out of the candidate
 * set. The unannotated twin alongside is what makes this a real assertion. */
TEST(cycle_candidate_weak_field_breaks_the_edge) {
    XaAnalyzer *a = analyzer_run_source("cycle_weak_edge.xr",
                                        "class Parent { children: Array<Child>\n"
                                        "  constructor() { this.children = [] } }\n"
                                        "class Child { weak parent: Parent?\n"
                                        "  constructor() { this.parent = null } }\n"
                                        /* same shape, no weak: must stay a candidate */
                                        "class StrongParent { children: Array<StrongChild>\n"
                                        "  constructor() { this.children = [] } }\n"
                                        "class StrongChild { parent: StrongParent?\n"
                                        "  constructor() { this.parent = null } }\n");
    ASSERT(a != NULL);

    ASSERT(!analyzer_class_is_cycle_candidate(a, "Parent"));
    ASSERT(!analyzer_class_is_cycle_candidate(a, "Child"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "StrongParent"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "StrongChild"));

    xa_analyzer_free(a);
    setup_pool();
}

/* A self-referential class annotated weak likewise stops being a candidate. */
TEST(cycle_candidate_weak_self_reference_breaks_the_edge) {
    XaAnalyzer *a =
        analyzer_run_source("cycle_weak_self.xr", "class WeakList { weak next: WeakList?\n"
                                                  "  constructor() { this.next = null } }\n"
                                                  "class StrongList { next: StrongList?\n"
                                                  "  constructor() { this.next = null } }\n");
    ASSERT(a != NULL);

    ASSERT(!analyzer_class_is_cycle_candidate(a, "WeakList"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "StrongList"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(cycle_candidate_follows_inherited_fields) {
    XaAnalyzer *a = analyzer_run_source("cycle_inherited_field.xr",
                                        "class Base { peer: Derived?\n"
                                        "  constructor() { this.peer = null } }\n"
                                        "class Derived extends Base { n: int\n"
                                        "  constructor() { super(); this.n = 0 } }\n"
                                        "class LoneBase { n: int\n"
                                        "  constructor() { this.n = 0 } }\n"
                                        "class LoneDerived extends LoneBase { label: string\n"
                                        "  constructor() { super(); this.label = \"\" } }\n");
    ASSERT(a != NULL);

    ASSERT(analyzer_class_is_cycle_candidate(a, "Base"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "Derived"));
    ASSERT(!analyzer_class_is_cycle_candidate(a, "LoneBase"));
    ASSERT(!analyzer_class_is_cycle_candidate(a, "LoneDerived"));

    xa_analyzer_free(a);
}

/* A.5's second requirement, written down so nobody "optimizes" it away:
 *
 * `class Node { children: Array<Node> }` is a TREE, but the TYPE graph has a
 * self-loop, and it must still be marked. L0 is a type-level approximation; it
 * cannot tell a downward edge from an upward one, and any heuristic that tried
 * would start missing real cycles. This is also what bounds the
 * no_reference_cycles contract: a recursive type does not
 * pass, and the diagnostic says "cannot prove", not "cycle detected". */
TEST(cycle_candidate_marks_recursive_tree_types) {
    XaAnalyzer *a = analyzer_run_source("cycle_recursive_tree.xr",
                                        "class TreeNode { children: Array<TreeNode>\n"
                                        "  constructor() { this.children = [] } }\n"
                                        "class ListNode { next: ListNode?\n"
                                        "  constructor() { this.next = null } }\n"
                                        "class Leaf { value: int\n"
                                        "  constructor() { this.value = 0 } }\n");
    ASSERT(a != NULL);

    ASSERT(analyzer_class_is_cycle_candidate(a, "TreeNode"));
    ASSERT(analyzer_class_is_cycle_candidate(a, "ListNode"));
    ASSERT(!analyzer_class_is_cycle_candidate(a, "Leaf"));

    xa_analyzer_free(a);
}

TEST(analyzer_error_effect_handles_recursive_function_expr_cycles) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum RecursiveLambdaErr { Boom }\n"
                         "fn viaRecursiveLambdaNoThrow() {\n"
                         "  var loop = fn(n: int) -> int {\n"
                         "    if (n <= 0) { return 0 }\n"
                         "    return loop(n - 1)\n"
                         "  }\n"
                         "  loop(2)\n"
                         "}\n"
                         "fn viaRecursiveLambdaThrow() {\n"
                         "  var loop = fn(n: int) -> int {\n"
                         "    if (n <= 0) { throw RecursiveLambdaErr.Boom }\n"
                         "    return loop(n - 1)\n"
                         "  }\n"
                         "  loop(2)\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_recursive_function_expr.xr", program);

    const XaEffectSummary *no_throw =
        analyzer_function_effect_summary(a, "viaRecursiveLambdaNoThrow");
    const XaEffectSummary *throwing =
        analyzer_function_effect_summary(a, "viaRecursiveLambdaThrow");
    ASSERT(no_throw != NULL);
    ASSERT(throwing != NULL);
    ASSERT(no_throw->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(no_throw->escaping.count == 0);
    ASSERT(throwing->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(effect_summary_has_enum_named(a, throwing, "RecursiveLambdaErr"));

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
                         "interface EffectRunner {\n"
                         "  failEffect()\n"
                         "}\n"
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
                         "final class FinalThrower {\n"
                         "  failFinal() { throw MethodErr.Boom }\n"
                         "}\n"
                         "class BaseThrower {\n"
                         "  failVirtual() { throw MethodErr.Boom }\n"
                         "}\n"
                         "class ChildThrower extends BaseThrower {\n"
                         "  failVirtual() { throw OtherMethodErr.Boom }\n"
                         "}\n"
                         "class InterfaceThrower implements EffectRunner {\n"
                         "  failEffect() { throw OtherMethodErr.Boom }\n"
                         "}\n"
                         "class OtherInterfaceThrower implements EffectRunner {\n"
                         "  failEffect() { throw MethodErr.Boom }\n"
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
                         "}\n"
                         "fn viaFinalMethod() {\n"
                         "  FinalThrower().failFinal()\n"
                         "}\n"
                         "fn viaOpenBaseMethod(b: BaseThrower) {\n"
                         "  b.failVirtual()\n"
                         "}\n"
                         "fn viaInterfaceMethod(r: EffectRunner) {\n"
                         "  r.failEffect()\n"
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
    const XaEffectSummary *final_method = analyzer_function_effect_summary(a, "viaFinalMethod");
    const XaEffectSummary *open_base = analyzer_function_effect_summary(a, "viaOpenBaseMethod");
    const XaEffectSummary *interface_method =
        analyzer_function_effect_summary(a, "viaInterfaceMethod");
    ASSERT(instance != NULL);
    ASSERT(temporary != NULL);
    ASSERT(static_method != NULL);
    ASSERT(method_hof != NULL);
    ASSERT(static_method_hof != NULL);
    ASSERT(method_hof_union != NULL);
    ASSERT(method_hof_unknown != NULL);
    ASSERT(final_method != NULL);
    ASSERT(open_base != NULL);
    ASSERT(interface_method != NULL);

    ASSERT(effect_summary_has_enum_named(a, instance, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, temporary, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, static_method, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, final_method, "MethodErr"));
    ASSERT(final_method->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((final_method->error_unknown_reasons & XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH) == 0);
    ASSERT(method_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((method_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, method_hof, "MethodErr"));
    ASSERT(!effect_summary_has_enum_named(a, method_hof, "OtherMethodErr"));
    ASSERT(static_method_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((static_method_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(!effect_summary_has_enum_named(a, static_method_hof, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, static_method_hof, "OtherMethodErr"));
    ASSERT(method_hof_union->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((method_hof_union->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT(effect_summary_has_enum_named(a, method_hof_union, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, method_hof_union, "OtherMethodErr"));
    ASSERT(method_hof_unknown->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((method_hof_unknown->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!effect_summary_has_enum_named(a, method_hof_unknown, "MethodErr"));
    ASSERT(open_base->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((open_base->error_unknown_reasons & XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH) == 0);
    ASSERT(effect_summary_has_enum_named(a, open_base, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, open_base, "OtherMethodErr"));
    ASSERT(interface_method->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((interface_method->error_unknown_reasons & XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH) == 0);
    ASSERT(effect_summary_has_enum_named(a, interface_method, "MethodErr"));
    ASSERT(effect_summary_has_enum_named(a, interface_method, "OtherMethodErr"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_propagates_module_export_calls) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *lib_source = "export enum ImportedErr { Selective, Namespace }\n"
                             "export fn failSelective() { throw ImportedErr.Selective }\n"
                             "export fn failNamespace() { throw ImportedErr.Namespace }\n"
                             "export fn applyImported(cb: () -> ()) { cb() }\n"
                             "export fn importedScalar(x: int) -> int { return x + 1 }\n"
                             "export fn importedAlloc() { var values = [1, 2, 3] }\n";
    const char *reexport_source =
        "export { failSelective as failReexported, failNamespace, applyImported as "
        "applyReexported } from "
        "\"./effect_export_module\"\n";
    const char *star_source = "export * from \"./effect_export_module\"\n";
    const char *callback_source = "export enum CallbackErr { Foreign, Local }\n"
                                  "export fn failForeignCallback() { throw CallbackErr.Foreign }\n";
    const char *entry_source = "import { failSelective, applyImported } from "
                               "\"./effect_export_module\"\n"
                               "import { importedScalar, importedAlloc } from "
                               "\"./effect_export_module\"\n"
                               "import \"./effect_export_module\" as effects\n"
                               "import { failForeignCallback, CallbackErr } from "
                               "\"./effect_callback_module\"\n"
                               "import \"./effect_callback_module\" as callbacks\n"
                               "import { failReexported, applyReexported } from "
                               "\"./effect_reexport_module\"\n"
                               "import \"./effect_reexport_module\" as facade\n"
                               "import { failNamespace as failStarNamespace, applyImported as "
                               "applyStar } from "
                               "\"./effect_star_module\"\n"
                               "import \"./effect_star_module\" as star\n"
                               "fn viaSelective() { failSelective() }\n"
                               "fn viaNamespace() { effects.failNamespace() }\n"
                               "fn viaReexportedSelective() { failReexported() }\n"
                               "fn viaReexportedNamespace() { facade.failNamespace() }\n"
                               "fn viaStarSelective() { star.failSelective() }\n"
                               "fn viaStarNamespace() { failStarNamespace() }\n"
                               "fn viaImportedHigherOrder() { applyImported(failSelective) }\n"
                               "fn viaNamespaceHigherOrder() { "
                               "effects.applyImported(failSelective) }\n"
                               "fn viaReexportedHigherOrder() { "
                               "applyReexported(failReexported) }\n"
                               "fn viaReexportedNamespaceHigherOrder() { "
                               "facade.applyReexported(failReexported) }\n"
                               "fn viaStarHigherOrder() { applyStar(failStarNamespace) }\n"
                               "fn localCallbackForImportedHof() { throw CallbackErr.Local }\n"
                               "fn viaImportedHigherOrderLocalCallback() { "
                               "applyImported(localCallbackForImportedHof) }\n"
                               "fn viaImportedHigherOrderForeignCallback() { "
                               "applyImported(failForeignCallback) }\n"
                               "fn viaImportedHigherOrderNamespaceCallback() { "
                               "effects.applyImported(callbacks.failForeignCallback) }\n"
                               "fn viaImportedNoAlloc() { importedScalar(1) }\n"
                               "fn viaImportedAlloc() { importedAlloc() }\n";

    char tmpdir[] = "/tmp/xray_effect_reexport_XXXXXX";
    ASSERT(xr_test_mkdtemp(tmpdir) != NULL);
    char lib_path[512];
    char reexport_path[512];
    char star_path[512];
    char callback_path[512];
    char entry_path[512];
    snprintf(lib_path, sizeof(lib_path), "%s/effect_export_module.xr", tmpdir);
    snprintf(reexport_path, sizeof(reexport_path), "%s/effect_reexport_module.xr", tmpdir);
    snprintf(star_path, sizeof(star_path), "%s/effect_star_module.xr", tmpdir);
    snprintf(callback_path, sizeof(callback_path), "%s/effect_callback_module.xr", tmpdir);
    snprintf(entry_path, sizeof(entry_path), "%s/entry_effect_module.xr", tmpdir);
    ASSERT(write_text_file(lib_path, lib_source));
    ASSERT(write_text_file(reexport_path, reexport_source));
    ASSERT(write_text_file(star_path, star_source));
    ASSERT(write_text_file(callback_path, callback_source));
    ASSERT(write_text_file(entry_path, entry_source));

    AstNode *lib_program = xr_parse(g_session, lib_source);
    AstNode *reexport_program = xr_parse(g_session, reexport_source);
    AstNode *star_program = xr_parse(g_session, star_source);
    AstNode *callback_program = xr_parse(g_session, callback_source);
    AstNode *entry_program = xr_parse(g_session, entry_source);
    ASSERT(lib_program != NULL);
    ASSERT(reexport_program != NULL);
    ASSERT(star_program != NULL);
    ASSERT(callback_program != NULL);
    ASSERT(entry_program != NULL);

    XrModuleResolverConfig cfg = {0};
    XrModuleResolver *resolver = xr_module_resolver_new(&cfg);
    ASSERT(resolver != NULL);
    XrModuleId lib_id;
    XrModuleId reexport_id;
    XrModuleId star_id;
    XrModuleId callback_id;
    char *resolve_err = NULL;
    ASSERT(xr_module_resolver_resolve(resolver, "./effect_export_module", false, entry_path,
                                      &lib_id, &resolve_err) == 0);
    xr_free(resolve_err);
    resolve_err = NULL;
    ASSERT(xr_module_resolver_resolve(resolver, "./effect_reexport_module", false, entry_path,
                                      &reexport_id, &resolve_err) == 0);
    xr_free(resolve_err);
    resolve_err = NULL;
    ASSERT(xr_module_resolver_resolve(resolver, "./effect_star_module", false, entry_path, &star_id,
                                      &resolve_err) == 0);
    xr_free(resolve_err);
    resolve_err = NULL;
    ASSERT(xr_module_resolver_resolve(resolver, "./effect_callback_module", false, entry_path,
                                      &callback_id, &resolve_err) == 0);
    xr_free(resolve_err);
    char *entry_canonical = xr_test_realpath_alloc(entry_path);
    ASSERT(entry_canonical != NULL);

    XrModuleSpec specs[5] = {{.canonical = lib_id.canonical,
                              .source_path = lib_id.source_path,
                              .ast = lib_program,
                              .status = XR_MODSPEC_RESOLVED,
                              .topo_index = 0},
                             {.canonical = reexport_id.canonical,
                              .source_path = reexport_id.source_path,
                              .ast = reexport_program,
                              .status = XR_MODSPEC_RESOLVED,
                              .topo_index = 1},
                             {.canonical = star_id.canonical,
                              .source_path = star_id.source_path,
                              .ast = star_program,
                              .status = XR_MODSPEC_RESOLVED,
                              .topo_index = 2},
                             {.canonical = callback_id.canonical,
                              .source_path = callback_id.source_path,
                              .ast = callback_program,
                              .status = XR_MODSPEC_RESOLVED,
                              .topo_index = 3},
                             {.canonical = entry_canonical,
                              .source_path = entry_canonical,
                              .ast = entry_program,
                              .status = XR_MODSPEC_RESOLVED,
                              .topo_index = 4}};
    int topo_order[5] = {0, 1, 2, 3, 4};
    XrHashMap *id_index = xr_hashmap_new();
    ASSERT(id_index != NULL);
    ASSERT(xr_hashmap_set(id_index, specs[0].canonical, (void *) (intptr_t) 1));
    ASSERT(xr_hashmap_set(id_index, specs[1].canonical, (void *) (intptr_t) 2));
    ASSERT(xr_hashmap_set(id_index, specs[2].canonical, (void *) (intptr_t) 3));
    ASSERT(xr_hashmap_set(id_index, specs[3].canonical, (void *) (intptr_t) 4));
    ASSERT(xr_hashmap_set(id_index, specs[4].canonical, (void *) (intptr_t) 5));
    XrModuleGraph graph = {.specs = specs,
                           .spec_count = 5,
                           .id_index = id_index,
                           .topo_order = topo_order,
                           .topo_count = 5,
                           .resolver = resolver,
                           .entry_index = 4};

    xa_analyzer_set_graph(a, &graph);
    xa_analyzer_analyze(a, specs[0].source_path, lib_program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    XrHashMap *lib_exports = NULL;
    ASSERT(xa_analyzer_collect_export_symbols_checked(a, (XrAstNode *) lib_program, &lib_exports));
    ASSERT(lib_exports != NULL);
    specs[0].export_symbols = lib_exports;
    xa_analyzer_clear_diagnostics(a);

    xa_analyzer_analyze(a, specs[1].source_path, reexport_program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    XrHashMap *reexport_exports = NULL;
    ASSERT(xa_analyzer_collect_export_symbols_checked(a, (XrAstNode *) reexport_program,
                                                      &reexport_exports));
    ASSERT(reexport_exports != NULL);
    specs[1].export_symbols = reexport_exports;
    xa_analyzer_clear_diagnostics(a);

    xa_analyzer_analyze(a, specs[2].source_path, star_program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    XrHashMap *star_exports = NULL;
    ASSERT(
        xa_analyzer_collect_export_symbols_checked(a, (XrAstNode *) star_program, &star_exports));
    ASSERT(star_exports != NULL);
    specs[2].export_symbols = star_exports;
    xa_analyzer_clear_diagnostics(a);

    xa_analyzer_analyze(a, specs[3].source_path, callback_program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    XrHashMap *callback_exports = NULL;
    ASSERT(xa_analyzer_collect_export_symbols_checked(a, (XrAstNode *) callback_program,
                                                      &callback_exports));
    ASSERT(callback_exports != NULL);
    specs[3].export_symbols = callback_exports;
    xa_analyzer_clear_diagnostics(a);

    xa_analyzer_analyze(a, specs[4].source_path, entry_program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    const XaEffectSummary *selective = analyzer_function_effect_summary(a, "viaSelective");
    const XaEffectSummary *ns = analyzer_function_effect_summary(a, "viaNamespace");
    const XaEffectSummary *reexported =
        analyzer_function_effect_summary(a, "viaReexportedSelective");
    const XaEffectSummary *reexported_ns =
        analyzer_function_effect_summary(a, "viaReexportedNamespace");
    const XaEffectSummary *star_selective = analyzer_function_effect_summary(a, "viaStarSelective");
    const XaEffectSummary *star_ns = analyzer_function_effect_summary(a, "viaStarNamespace");
    const XaEffectSummary *imported_hof =
        analyzer_function_effect_summary(a, "viaImportedHigherOrder");
    const XaEffectSummary *namespace_hof =
        analyzer_function_effect_summary(a, "viaNamespaceHigherOrder");
    const XaEffectSummary *reexported_hof =
        analyzer_function_effect_summary(a, "viaReexportedHigherOrder");
    const XaEffectSummary *reexported_namespace_hof =
        analyzer_function_effect_summary(a, "viaReexportedNamespaceHigherOrder");
    const XaEffectSummary *star_hof = analyzer_function_effect_summary(a, "viaStarHigherOrder");
    const XaEffectSummary *local_callback_hof =
        analyzer_function_effect_summary(a, "viaImportedHigherOrderLocalCallback");
    const XaEffectSummary *foreign_callback_hof =
        analyzer_function_effect_summary(a, "viaImportedHigherOrderForeignCallback");
    const XaEffectSummary *namespace_callback_hof =
        analyzer_function_effect_summary(a, "viaImportedHigherOrderNamespaceCallback");
    const XaAllocationSummary *imported_noalloc =
        analyzer_function_allocation_summary(a, "viaImportedNoAlloc");
    const XaAllocationSummary *imported_alloc =
        analyzer_function_allocation_summary(a, "viaImportedAlloc");
    ASSERT(selective != NULL);
    ASSERT(ns != NULL);
    ASSERT(reexported != NULL);
    ASSERT(reexported_ns != NULL);
    ASSERT(star_selective != NULL);
    ASSERT(star_ns != NULL);
    ASSERT(imported_hof != NULL);
    ASSERT(namespace_hof != NULL);
    ASSERT(reexported_hof != NULL);
    ASSERT(reexported_namespace_hof != NULL);
    ASSERT(star_hof != NULL);
    ASSERT(local_callback_hof != NULL);
    ASSERT(foreign_callback_hof != NULL);
    ASSERT(namespace_callback_hof != NULL);
    ASSERT(imported_noalloc && imported_noalloc->state == XA_ALLOC_PROVEN_NONE);
    ASSERT(imported_alloc && imported_alloc->state == XA_ALLOC_MAY);

    ASSERT(selective->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(ns->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(reexported->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(reexported_ns->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(star_selective->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(star_ns->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(imported_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(namespace_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(reexported_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(reexported_namespace_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(star_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(local_callback_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(foreign_callback_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(namespace_callback_hof->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT((selective->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((ns->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((reexported->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((reexported_ns->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((star_selective->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((star_ns->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((imported_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((namespace_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((reexported_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((reexported_namespace_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) ==
           0);
    ASSERT((star_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((local_callback_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((foreign_callback_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) == 0);
    ASSERT((namespace_callback_hof->error_unknown_reasons & XA_UNKNOWN_MISSING_IMPORTED_EFFECT) ==
           0);
    ASSERT((imported_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((namespace_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((reexported_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((reexported_namespace_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((star_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((local_callback_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((foreign_callback_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);
    ASSERT((namespace_callback_hof->error_unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) == 0);

    const XaErrorTypeSet *selective_set =
        effect_summary_enum_set_named(a, selective, "ImportedErr");
    const XaErrorTypeSet *namespace_set = effect_summary_enum_set_named(a, ns, "ImportedErr");
    const XaErrorTypeSet *reexported_set =
        effect_summary_enum_set_named(a, reexported, "ImportedErr");
    const XaErrorTypeSet *reexported_namespace_set =
        effect_summary_enum_set_named(a, reexported_ns, "ImportedErr");
    const XaErrorTypeSet *star_selective_set =
        effect_summary_enum_set_named(a, star_selective, "ImportedErr");
    const XaErrorTypeSet *star_namespace_set =
        effect_summary_enum_set_named(a, star_ns, "ImportedErr");
    const XaErrorTypeSet *imported_hof_set =
        effect_summary_enum_set_named(a, imported_hof, "ImportedErr");
    const XaErrorTypeSet *namespace_hof_set =
        effect_summary_enum_set_named(a, namespace_hof, "ImportedErr");
    const XaErrorTypeSet *reexported_hof_set =
        effect_summary_enum_set_named(a, reexported_hof, "ImportedErr");
    const XaErrorTypeSet *reexported_namespace_hof_set =
        effect_summary_enum_set_named(a, reexported_namespace_hof, "ImportedErr");
    const XaErrorTypeSet *star_hof_set = effect_summary_enum_set_named(a, star_hof, "ImportedErr");
    const XaErrorTypeSet *local_callback_hof_set =
        effect_summary_enum_set_named(a, local_callback_hof, "CallbackErr");
    const XaErrorTypeSet *foreign_callback_hof_set =
        effect_summary_enum_set_named(a, foreign_callback_hof, "CallbackErr");
    const XaErrorTypeSet *namespace_callback_hof_set =
        effect_summary_enum_set_named(a, namespace_callback_hof, "CallbackErr");
    ASSERT(selective_set != NULL);
    ASSERT(namespace_set != NULL);
    ASSERT(reexported_set != NULL);
    ASSERT(reexported_namespace_set != NULL);
    ASSERT(star_selective_set != NULL);
    ASSERT(star_namespace_set != NULL);
    ASSERT(imported_hof_set != NULL);
    ASSERT(namespace_hof_set != NULL);
    ASSERT(reexported_hof_set != NULL);
    ASSERT(reexported_namespace_hof_set != NULL);
    ASSERT(star_hof_set != NULL);
    ASSERT(local_callback_hof_set != NULL);
    ASSERT(foreign_callback_hof_set != NULL);
    ASSERT(namespace_callback_hof_set != NULL);
    ASSERT(!selective_set->all_variants);
    ASSERT(!namespace_set->all_variants);
    ASSERT(!reexported_set->all_variants);
    ASSERT(!reexported_namespace_set->all_variants);
    ASSERT(!star_selective_set->all_variants);
    ASSERT(!star_namespace_set->all_variants);
    ASSERT(!imported_hof_set->all_variants);
    ASSERT(!namespace_hof_set->all_variants);
    ASSERT(!reexported_hof_set->all_variants);
    ASSERT(!reexported_namespace_hof_set->all_variants);
    ASSERT(!star_hof_set->all_variants);
    ASSERT(!local_callback_hof_set->all_variants);
    ASSERT(!foreign_callback_hof_set->all_variants);
    ASSERT(!namespace_callback_hof_set->all_variants);
    ASSERT(xa_bitset_test(&selective_set->variants, 0));
    ASSERT(!xa_bitset_test(&selective_set->variants, 1));
    ASSERT(!xa_bitset_test(&namespace_set->variants, 0));
    ASSERT(xa_bitset_test(&namespace_set->variants, 1));
    ASSERT(xa_bitset_test(&reexported_set->variants, 0));
    ASSERT(!xa_bitset_test(&reexported_set->variants, 1));
    ASSERT(!xa_bitset_test(&reexported_namespace_set->variants, 0));
    ASSERT(xa_bitset_test(&reexported_namespace_set->variants, 1));
    ASSERT(xa_bitset_test(&star_selective_set->variants, 0));
    ASSERT(!xa_bitset_test(&star_selective_set->variants, 1));
    ASSERT(!xa_bitset_test(&star_namespace_set->variants, 0));
    ASSERT(xa_bitset_test(&star_namespace_set->variants, 1));
    ASSERT(xa_bitset_test(&imported_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&imported_hof_set->variants, 1));
    ASSERT(xa_bitset_test(&namespace_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&namespace_hof_set->variants, 1));
    ASSERT(xa_bitset_test(&reexported_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&reexported_hof_set->variants, 1));
    ASSERT(xa_bitset_test(&reexported_namespace_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&reexported_namespace_hof_set->variants, 1));
    ASSERT(!xa_bitset_test(&star_hof_set->variants, 0));
    ASSERT(xa_bitset_test(&star_hof_set->variants, 1));
    ASSERT(!xa_bitset_test(&local_callback_hof_set->variants, 0));
    ASSERT(xa_bitset_test(&local_callback_hof_set->variants, 1));
    ASSERT(xa_bitset_test(&foreign_callback_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&foreign_callback_hof_set->variants, 1));
    ASSERT(xa_bitset_test(&namespace_callback_hof_set->variants, 0));
    ASSERT(!xa_bitset_test(&namespace_callback_hof_set->variants, 1));

    xr_hashmap_free(lib_exports);
    specs[0].export_symbols = NULL;
    xr_hashmap_free(reexport_exports);
    specs[1].export_symbols = NULL;
    xr_hashmap_free(star_exports);
    specs[2].export_symbols = NULL;
    xr_hashmap_free(callback_exports);
    specs[3].export_symbols = NULL;
    xr_hashmap_free(id_index);
    xr_module_resolver_free(resolver);
    xr_module_id_cleanup(&lib_id);
    xr_module_id_cleanup(&reexport_id);
    xr_module_id_cleanup(&star_id);
    xr_module_id_cleanup(&callback_id);
    free(entry_canonical);
    xr_test_unlink(lib_path);
    xr_test_unlink(reexport_path);
    xr_test_unlink(star_path);
    xr_test_unlink(callback_path);
    xr_test_unlink(entry_path);
    xr_test_rmdir(tmpdir);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_xrd_signatures_fail_closed_without_typed_contracts) {
    char tmpdir_template[] = "/tmp/xray_effect_xrd_XXXXXX";
    char *tmpdir = xr_test_mkdtemp(tmpdir_template);
    ASSERT(tmpdir != NULL);

    char xrd_path[256];
    snprintf(xrd_path, sizeof(xrd_path), "%s/native_effects.xrd", tmpdir);
    ASSERT(write_text_file(xrd_path, "export fn failNative(): int\n"
                                     "export fn noThrowNative(): int\n"
                                     "export fn missingNative(): int\n"
                                     "export fn makeBox(): NativeBox\n"
                                     "type NativeBox = { const id: int }\n"
                                     "fn NativeBox.failMethod(): int\n"));

    const char *old_typepath = getenv("XRAY_TYPEPATH");
    char *old_typepath_copy = old_typepath ? strdup(old_typepath) : NULL;
    ASSERT(xr_test_setenv("XRAY_TYPEPATH", tmpdir, 1) == 0);
    xa_xrd_cleanup();

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "enum NativeErr { Boom, Other }\n"
                         "import native_effects\n"
                         "import { failNative, noThrowNative, missingNative } from "
                         "\"native_effects\"\n"
                         "fn viaNamespace() { native_effects.failNative() }\n"
                         "fn viaSelective() { failNative() }\n"
                         "fn viaNoThrow() { noThrowNative() }\n"
                         "fn viaMissing() { missingNative() }\n"
                         "fn viaHandle() { native_effects.makeBox().failMethod() }\n"
                         "fn allocNativeOk() { noThrowNative() }\n"
                         "fn allocNativeBad() { native_effects.makeBox() }\n"
                         "fn allocNativeUnknown() { missingNative() }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_xrd_native_contracts.xr", program);

    const XaEffectSummary *namespace_call = analyzer_function_effect_summary(a, "viaNamespace");
    const XaEffectSummary *selective_call = analyzer_function_effect_summary(a, "viaSelective");
    const XaEffectSummary *nothrow_call = analyzer_function_effect_summary(a, "viaNoThrow");
    const XaEffectSummary *missing_call = analyzer_function_effect_summary(a, "viaMissing");
    const XaEffectSummary *handle_call = analyzer_function_effect_summary(a, "viaHandle");
    ASSERT(namespace_call != NULL);
    ASSERT(selective_call != NULL);
    ASSERT(nothrow_call != NULL);
    ASSERT(missing_call != NULL);
    ASSERT(handle_call != NULL);
    ASSERT(namespace_call->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT(selective_call->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT(nothrow_call->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT(missing_call->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT(handle_call->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((namespace_call->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    ASSERT((selective_call->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    ASSERT((nothrow_call->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    ASSERT((missing_call->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    ASSERT((handle_call->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);

    const XaAllocationSummary *alloc_ok = analyzer_function_allocation_summary(a, "allocNativeOk");
    const XaAllocationSummary *alloc_bad =
        analyzer_function_allocation_summary(a, "allocNativeBad");
    const XaAllocationSummary *alloc_unknown =
        analyzer_function_allocation_summary(a, "allocNativeUnknown");
    ASSERT(alloc_ok && alloc_ok->state == XA_ALLOC_UNKNOWN);
    ASSERT(alloc_bad && alloc_bad->state == XA_ALLOC_UNKNOWN);
    ASSERT(alloc_unknown && alloc_unknown->state == XA_ALLOC_UNKNOWN);

    xa_analyzer_free(a);
    xa_xrd_cleanup();
    if (old_typepath_copy) {
        ASSERT(xr_test_setenv("XRAY_TYPEPATH", old_typepath_copy, 1) == 0);
        free(old_typepath_copy);
    } else {
        xr_test_unsetenv("XRAY_TYPEPATH");
    }
    xr_test_unlink(xrd_path);
    xr_test_rmdir(tmpdir);
    setup_pool();
}

TEST(analyzer_xrd_native_typed_byte_contracts_reject_legacy_aliases) {
    char tmpdir_template[] = "/tmp/xray_effect_xrd_byte_XXXXXX";
    char *tmpdir = xr_test_mkdtemp(tmpdir_template);
    ASSERT(tmpdir != NULL);

    char ok_path[256];
    char legacy_path[256];
    char span_only_path[256];
    char view_only_path[256];
    snprintf(ok_path, sizeof(ok_path), "%s/native_byte_effects.xrd", tmpdir);
    snprintf(legacy_path, sizeof(legacy_path), "%s/native_legacy_byte_effects.xrd", tmpdir);
    snprintf(span_only_path, sizeof(span_only_path), "%s/native_legacy_bytespan_only.xrd", tmpdir);
    snprintf(view_only_path, sizeof(view_only_path), "%s/native_legacy_byteview_only.xrd", tmpdir);
    ASSERT(write_text_file(ok_path, "export fn decode(input: Slice<byte>): Array<byte>\n"));
    ASSERT(write_text_file(legacy_path, "export fn decodeOld(input: ByteSlice): Bytes\n"
                                        "export fn viewOld(input: ByteView): int\n"));
    ASSERT(write_text_file(span_only_path, "export fn spanOnly(input: Byte"
                                           "Slice): int\n"));
    ASSERT(write_text_file(view_only_path, "export fn viewOnly(input: Byte"
                                           "View): int\n"));

    const char *old_typepath = getenv("XRAY_TYPEPATH");
    char *old_typepath_copy = old_typepath ? strdup(old_typepath) : NULL;
    ASSERT(xr_test_setenv("XRAY_TYPEPATH", tmpdir, 1) == 0);
    xa_xrd_cleanup();

    XaAnalyzer *ok = xa_analyzer_new(g_session);
    ASSERT(ok != NULL);
    const char *ok_source = "enum NativeByteErr { BadInput }\n"
                            "import { decode } from \"native_byte_effects\"\n"
                            "fn viaNative(input: Slice<byte>) { decode(input) }\n";
    AstNode *ok_program = xr_parse(g_session, ok_source);
    ASSERT(ok_program != NULL);
    xa_analyzer_analyze(ok, "effect_xrd_native_byte_contracts.xr", ok_program);
    ASSERT(!analyzer_diag_contains(ok, "error"));
    const XaEffectSummary *ok_effect = analyzer_function_effect_summary(ok, "viaNative");
    ASSERT(ok_effect != NULL);
    ASSERT(ok_effect->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((ok_effect->error_unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    xa_analyzer_free(ok);

    XaAnalyzer *legacy = xa_analyzer_new(g_session);
    ASSERT(legacy != NULL);
    const char *legacy_source = "enum NativeByteErr { BadInput }\n"
                                "import { decodeOld, viewOld } from "
                                "\"native_legacy_byte_effects\"\n"
                                "fn viaOld(input: ByteSlice) { decodeOld(input) }\n"
                                "fn viaView(input: ByteView) { viewOld(input) }\n";
    AstNode *legacy_program = xr_parse(g_session, legacy_source);
    ASSERT(legacy_program != NULL);
    xa_analyzer_analyze(legacy, "effect_xrd_native_legacy_byte_contracts.xr", legacy_program);
    ASSERT(analyzer_diag_contains(legacy, "invalid XRD descriptor"));
    ASSERT(analyzer_diag_contains(legacy, "removed byte alias 'Bytes'"));
    ASSERT(analyzer_diag_contains(legacy, "undefined type 'ByteSlice'"));
    ASSERT(analyzer_diag_contains(legacy, "undefined type 'ByteView'"));
    xa_analyzer_free(legacy);

    XaAnalyzer *span_only = xa_analyzer_new(g_session);
    ASSERT(span_only != NULL);
    const char *span_only_source = "import { spanOnly } from \"native_legacy_bytespan_only\"\n"
                                   "fn viaSlice(input: Slice<byte>) { spanOnly(input) }\n";
    AstNode *span_only_program = xr_parse(g_session, span_only_source);
    ASSERT(span_only_program != NULL);
    xa_analyzer_analyze(span_only, "effect_xrd_legacy_bytespan_only.xr", span_only_program);
    ASSERT(analyzer_diag_contains(span_only, "invalid XRD descriptor"));
    ASSERT(analyzer_diag_contains(span_only, "removed byte alias 'ByteSlice'"));
    xa_analyzer_free(span_only);

    XaAnalyzer *view_only = xa_analyzer_new(g_session);
    ASSERT(view_only != NULL);
    const char *view_only_source = "import { viewOnly } from \"native_legacy_byteview_only\"\n"
                                   "fn viaViewOnly(input: Slice<byte>) { viewOnly(input) }\n";
    AstNode *view_only_program = xr_parse(g_session, view_only_source);
    ASSERT(view_only_program != NULL);
    xa_analyzer_analyze(view_only, "effect_xrd_legacy_byteview_only.xr", view_only_program);
    ASSERT(analyzer_diag_contains(view_only, "invalid XRD descriptor"));
    ASSERT(analyzer_diag_contains(view_only, "removed byte alias 'ByteView'"));
    xa_analyzer_free(view_only);

    xa_xrd_cleanup();
    if (old_typepath_copy) {
        ASSERT(xr_test_setenv("XRAY_TYPEPATH", old_typepath_copy, 1) == 0);
        free(old_typepath_copy);
    } else {
        xr_test_unsetenv("XRAY_TYPEPATH");
    }
    xr_test_unlink(ok_path);
    xr_test_unlink(legacy_path);
    xr_test_unlink(span_only_path);
    xr_test_unlink(view_only_path);
    xr_test_rmdir(tmpdir);
    setup_pool();
}

TEST(analyzer_xrd_handle_fields_reject_legacy_byte_aliases) {
    char tmpdir_template[] = "/tmp/xray_xrd_handle_byte_XXXXXX";
    char *tmpdir = xr_test_mkdtemp(tmpdir_template);
    ASSERT(tmpdir != NULL);

    char xrd_path[256];
    snprintf(xrd_path, sizeof(xrd_path), "%s/native_handle_bytes.xrd", tmpdir);
    ASSERT(write_text_file(xrd_path, "type NativeBox = { const payload: Bytes }\n"
                                     "export fn makeBox(): NativeBox @nothrow\n"));

    const char *old_typepath = getenv("XRAY_TYPEPATH");
    char *old_typepath_copy = old_typepath ? strdup(old_typepath) : NULL;
    ASSERT(xr_test_setenv("XRAY_TYPEPATH", tmpdir, 1) == 0);
    xa_xrd_cleanup();

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "import { makeBox } from \"native_handle_bytes\"\n"
                         "fn payload() { makeBox().payload }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_xrd_handle_legacy_byte_field.xr", program);
    ASSERT(analyzer_diag_contains(a, "invalid XRD descriptor"));
    ASSERT(analyzer_diag_contains(a, "removed byte alias 'Bytes'"));
    xa_analyzer_free(a);

    xa_xrd_cleanup();
    if (old_typepath_copy) {
        ASSERT(xr_test_setenv("XRAY_TYPEPATH", old_typepath_copy, 1) == 0);
        free(old_typepath_copy);
    } else {
        xr_test_unsetenv("XRAY_TYPEPATH");
    }
    xr_test_unlink(xrd_path);
    xr_test_rmdir(tmpdir);
    setup_pool();
}

TEST(analyzer_xrd_namespace_reports_invalid_descriptor) {
    char tmpdir_template[] = "/tmp/xray_xrd_namespace_byte_XXXXXX";
    char *tmpdir = xr_test_mkdtemp(tmpdir_template);
    ASSERT(tmpdir != NULL);

    char xrd_path[256];
    snprintf(xrd_path, sizeof(xrd_path), "%s/native_handle_namespace_bytes.xrd", tmpdir);
    ASSERT(write_text_file(xrd_path, "type NativeBox = { const payload: Bytes }\n"
                                     "export fn makeBox(): NativeBox @nothrow\n"));

    const char *old_typepath = getenv("XRAY_TYPEPATH");
    char *old_typepath_copy = old_typepath ? strdup(old_typepath) : NULL;
    ASSERT(xr_test_setenv("XRAY_TYPEPATH", tmpdir, 1) == 0);
    xa_xrd_cleanup();

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "import native_handle_namespace_bytes\n"
                         "fn payload() { native_handle_namespace_bytes.makeBox().payload }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_xrd_namespace_invalid_descriptor.xr", program);
    ASSERT(analyzer_diag_contains(a, "invalid XRD descriptor"));
    ASSERT(analyzer_diag_contains(a, "removed byte alias 'Bytes'"));
    xa_analyzer_free(a);

    xa_xrd_cleanup();
    if (old_typepath_copy) {
        ASSERT(xr_test_setenv("XRAY_TYPEPATH", old_typepath_copy, 1) == 0);
        free(old_typepath_copy);
    } else {
        xr_test_unsetenv("XRAY_TYPEPATH");
    }
    xr_test_unlink(xrd_path);
    xr_test_rmdir(tmpdir);
    setup_pool();
}

static XaBuiltinMember *mutable_builtin_type_member(XrType *type, const char *name,
                                                    bool is_static) {
    const XaBuiltinMember *members = NULL;
    int count = xa_builtin_get_members_for_type(type, &members);
    if (!members)
        return NULL;
    for (int i = 0; i < count; i++) {
        if (members[i].is_static == is_static && members[i].name &&
            strcmp(members[i].name, name) == 0)
            return (XaBuiltinMember *) &members[i];
    }
    return NULL;
}

TEST(analyzer_error_effect_consumes_builtin_type_member_contracts) {
    XrType *string_type = xr_type_new_string(g_isolate);
    ASSERT(string_type != NULL);
    XaBuiltinMember *from_utf8 = mutable_builtin_type_member(string_type, "fromUtf8", true);
    XaBuiltinMember *slice_bytes = mutable_builtin_type_member(string_type, "sliceBytes", false);
    ASSERT(from_utf8 != NULL);
    ASSERT(slice_bytes != NULL);
    const XaEffectContract *lossy_contract =
        xa_builtin_get_type_member_effect_contract(string_type, "fromUtf8Lossy", true);
    ASSERT(lossy_contract != NULL);
    ASSERT(lossy_contract->kind == XA_EFFECT_CONTRACT_NOTHROW);
    const XaEffectContract *from_utf8_contract =
        xa_builtin_get_type_member_effect_contract(string_type, "fromUtf8", true);
    const XaEffectContract *slice_bytes_contract =
        xa_builtin_get_type_member_effect_contract(string_type, "sliceBytes", false);
    const XaEffectContract *gunzip_contract =
        xa_builtin_get_module_func_effect_contract("compress", "gunzip");
    const XaEffectContract *decrypt_contract =
        xa_builtin_get_module_func_effect_contract("crypto", "decrypt");
    const XaEffectContract *private_copy_contract =
        xa_builtin_get_module_func_abi_effect_contract("net", "__copyBidirectional");
    ASSERT(from_utf8_contract != NULL);
    ASSERT(slice_bytes_contract != NULL);
    ASSERT(gunzip_contract != NULL);
    ASSERT(decrypt_contract != NULL);
    ASSERT(private_copy_contract != NULL);
    ASSERT(xa_builtin_get_module_func_effect_contract("net", "__copyBidirectional") == NULL);
    ASSERT(from_utf8_contract->kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(slice_bytes_contract->kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(gunzip_contract->kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(decrypt_contract->kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(private_copy_contract->kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(from_utf8_contract->error_count == 1);
    ASSERT(slice_bytes_contract->error_count == 1);
    ASSERT(gunzip_contract->error_count == 1);
    ASSERT(decrypt_contract->error_count == 1);
    ASSERT(private_copy_contract->error_count == 10);
    ASSERT(strcmp(from_utf8_contract->errors[0], "Utf8Error.InvalidUtf8") == 0);
    ASSERT(strcmp(slice_bytes_contract->errors[0], "StringSliceError.InvalidByteRange") == 0);
    ASSERT(strcmp(gunzip_contract->errors[0], "CompressionError.InvalidData") == 0);
    ASSERT(strcmp(decrypt_contract->errors[0], "CryptoError.InvalidLength") == 0);

    XaAnalyzer *current = xa_analyzer_new(g_session);
    ASSERT(current != NULL);
    const char *current_source = "import crypto\n"
                                 "fn currentStatic(bytes: Slice<byte>) { string.fromUtf8(bytes) }\n"
                                 "fn currentInstance(s: string) { s.sliceBytes(0, 1) }\n"
                                 "fn currentDecrypt(ciphertext: string) { "
                                 "crypto.decrypt(\"secret\", ciphertext) }\n"
                                 "fn currentLossy(bytes: Slice<byte>) { "
                                 "string.fromUtf8Lossy(bytes) }\n";
    AstNode *current_program = xr_parse(g_session, current_source);
    ASSERT(current_program != NULL);
    xa_analyzer_analyze(current, "effect_builtin_type_member_current_contracts.xr",
                        current_program);
    ASSERT(!analyzer_diag_contains(current, "error"));
    const XaEffectSummary *current_static =
        analyzer_function_effect_summary(current, "currentStatic");
    const XaEffectSummary *current_instance =
        analyzer_function_effect_summary(current, "currentInstance");
    const XaEffectSummary *current_decrypt =
        analyzer_function_effect_summary(current, "currentDecrypt");
    const XaEffectSummary *current_lossy =
        analyzer_function_effect_summary(current, "currentLossy");
    ASSERT(current_static != NULL);
    ASSERT(current_instance != NULL);
    ASSERT(current_decrypt != NULL);
    ASSERT(current_lossy != NULL);
    ASSERT(current_static->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(current_instance->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(current_decrypt->error_set_completeness == XA_EFFECT_COMPLETE);
    const XaErrorTypeSet *current_static_set =
        effect_summary_enum_set_named(current, current_static, "Utf8Error");
    const XaErrorTypeSet *current_instance_set =
        effect_summary_enum_set_named(current, current_instance, "StringSliceError");
    const XaErrorTypeSet *current_decrypt_set =
        effect_summary_enum_set_named(current, current_decrypt, "CryptoError");
    ASSERT(current_static_set != NULL);
    ASSERT(current_instance_set != NULL);
    ASSERT(current_decrypt_set != NULL);
    ASSERT(!current_static_set->all_variants);
    ASSERT(!current_instance_set->all_variants);
    ASSERT(!current_decrypt_set->all_variants);
    ASSERT(xa_bitset_test(&current_static_set->variants, 0));
    ASSERT(xa_bitset_test(&current_instance_set->variants, 0));
    ASSERT(xa_bitset_test(&current_decrypt_set->variants, 0));
    ASSERT(xa_effect_summary_is_nothrow(current_lossy));
    xa_analyzer_free(current);

    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    const char *source = "fn viaStatic(bytes: Slice<byte>) { string.fromUtf8(bytes) }\n"
                         "fn viaInstance(s: string) { s.sliceBytes(0, 1) }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_builtin_type_member_contracts.xr", program);
    ASSERT(!analyzer_diag_contains(a, "error"));

    const XaEffectSummary *static_call = analyzer_function_effect_summary(a, "viaStatic");
    const XaEffectSummary *instance_call = analyzer_function_effect_summary(a, "viaInstance");
    ASSERT(static_call != NULL);
    ASSERT(instance_call != NULL);
    ASSERT(static_call->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(instance_call->error_set_completeness == XA_EFFECT_COMPLETE);

    const XaErrorTypeSet *static_set = effect_summary_enum_set_named(a, static_call, "Utf8Error");
    const XaErrorTypeSet *instance_set =
        effect_summary_enum_set_named(a, instance_call, "StringSliceError");
    ASSERT(static_set != NULL);
    ASSERT(instance_set != NULL);
    ASSERT(!static_set->all_variants);
    ASSERT(!instance_set->all_variants);
    ASSERT(xa_bitset_test(&static_set->variants, 0));
    ASSERT(xa_bitset_test(&instance_set->variants, 0));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_subtracts_typed_catches) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source =
        "enum CatchErr { Boom, Other }\n"
        "enum OtherErr { Boom }\n"
        "enum PayloadErr { Boom, Other, Payload(int) }\n"
        "struct CatchBox { value: CatchErr }\n"
        "struct CatchPair { kept: CatchErr; changed: CatchErr }\n"
        "fn fail() { throw CatchErr.Boom }\n"
        "fn failOther() { throw OtherErr.Boom }\n"
        "fn failPayloadBoom() { throw PayloadErr.Boom }\n"
        "fn failPayloadOther() { throw PayloadErr.Other }\n"
        "fn failPayloadCase() { throw PayloadErr.Payload(1) }\n"
        "fn failPayloadAny(e: PayloadErr) { throw e }\n"
        "fn handled() { try { fail() } catch (e: CatchErr) { } }\n"
        "fn leaks() { try { fail() } catch (e: OtherErr) { } }\n"
        "fn catchesAll() { try { fail() } catch (e) { } }\n"
        "fn bareEnumPatternHandlesAll() { try { failPayloadBoom() } catch PayloadErr { } }\n"
        "fn variantPatternHandlesOnlyBoom() { "
        "  try { failPayloadBoom() } catch PayloadErr.Boom { } "
        "}\n"
        "fn variantPatternLeaksOther() { "
        "  try { failPayloadOther() } catch PayloadErr.Boom { } "
        "}\n"
        "fn payloadPatternHandlesPayload() { "
        "  try { failPayloadCase() } catch PayloadErr.Payload(code) { } "
        "}\n"
        "fn variantPatternLeavesRest(e: PayloadErr) { "
        "  try { failPayloadAny(e) } catch PayloadErr.Boom { } "
        "}\n"
        "fn variantPatternBodyThrows() { "
        "  try { failPayloadBoom() } catch PayloadErr.Boom { throw OtherErr.Boom } "
        "}\n"
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
        "fn catchIfElseAliasRethrows(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; "
        "    if (flag) { alias = e } else { alias = e }; throw alias "
        "  } "
        "}\n"
        "fn catchIfElseAliasInvalidates(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    if (flag) { alias = CatchErr.Other } else { alias = CatchErr.Other }; "
        "    throw alias "
        "  } "
        "}\n"
        "fn catchWhileAliasDoesNotLeak(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; "
        "    while (flag) { alias = e }; throw alias "
        "  } "
        "}\n"
        "fn catchWhileAliasPreserves(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    while (flag) { alias = e }; throw alias "
        "  } "
        "}\n"
        "fn catchWhileAliasInvalidates(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    while (flag) { alias = CatchErr.Other }; throw alias "
        "  } "
        "}\n"
        "fn catchTryAliasPreserves(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    try { if (flag) { failOther() }; alias = e } catch (other: OtherErr) { }; "
        "    throw alias "
        "  } "
        "}\n"
        "fn catchTryInnerAliasDoesNotClobberOuter(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    try { if (flag) { failOther() } } catch (other: OtherErr) { const inner = other }; "
        "    throw alias "
        "  } "
        "}\n"
        "fn catchTryAliasDoesNotLeak(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; "
        "    try { if (flag) { failOther() }; alias = e } catch (other: OtherErr) { }; "
        "    throw alias "
        "  } "
        "}\n"
        "fn catchTryAliasInvalidates(flag: bool) { "
        "  try { fail() } catch (e: CatchErr) { "
        "    var alias: CatchErr = CatchErr.Other; alias = e; "
        "    try { if (flag) { failOther() }; alias = CatchErr.Other } "
        "    catch (other: OtherErr) { }; throw alias "
        "  } "
        "}\n"
        "fn makeCatchBindingClosure() -> () -> () {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    return fn() { throw e }\n"
        "  }\n"
        "  return fn() { }\n"
        "}\n"
        "fn catchClosureBindingRethrows() {\n"
        "  var run = makeCatchBindingClosure()\n"
        "  run()\n"
        "}\n"
        "fn makeCatchAliasClosure() -> () -> () {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    const alias = e\n"
        "    return fn() { throw alias }\n"
        "  }\n"
        "  return fn() { }\n"
        "}\n"
        "fn catchClosureAliasRethrows() {\n"
        "  var run = makeCatchAliasClosure()\n"
        "  run()\n"
        "}\n"
        "fn makeCatchAliasClosureInvalidated() -> () -> () {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var alias = e\n"
        "    alias = CatchErr.Other\n"
        "    return fn() { throw alias }\n"
        "  }\n"
        "  return fn() { }\n"
        "}\n"
        "fn catchClosureAliasInvalidates() {\n"
        "  var run = makeCatchAliasClosureInvalidated()\n"
        "  run()\n"
        "}\n"
        "fn catchArrayAliasRethrows() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    const box = [e]\n"
        "    throw box[0]\n"
        "  }\n"
        "}\n"
        "fn catchArrayAliasInvalidates() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = [e]\n"
        "    box[0] = CatchErr.Other\n"
        "    throw box[0]\n"
        "  }\n"
        "}\n"
        "fn catchDynamicIndexRethrows(i: int) {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = [CatchErr.Other, CatchErr.Other]\n"
        "    box[i] = e\n"
        "    throw box[i]\n"
        "  }\n"
        "}\n"
        "fn catchDynamicIndexMismatchRethrows(i: int, j: int) {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = [CatchErr.Other, CatchErr.Other]\n"
        "    box[i] = e\n"
        "    throw box[j]\n"
        "  }\n"
        "}\n"
        "fn catchReassignedDynamicIndexInvalidates() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var i = 0\n"
        "    var box = [CatchErr.Other, CatchErr.Other]\n"
        "    box[i] = e\n"
        "    i = 1\n"
        "    throw box[i]\n"
        "  }\n"
        "}\n"
        "fn catchArrayOtherSlotMutationPreserves() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = [e, e]\n"
        "    box[1] = CatchErr.Other\n"
        "    throw box[0]\n"
        "  }\n"
        "}\n"
        "fn catchStructFieldAliasRethrows() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    const box = CatchBox{value: e}\n"
        "    throw box.value\n"
        "  }\n"
        "}\n"
        "fn catchStructFieldAliasInvalidates() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = CatchBox{value: e}\n"
        "    box.value = CatchErr.Other\n"
        "    throw box.value\n"
        "  }\n"
        "}\n"
        "fn catchStructOtherFieldMutationPreserves() {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    var box = CatchPair{kept: e, changed: e}\n"
        "    box.changed = CatchErr.Other\n"
        "    throw box.kept\n"
        "  }\n"
        "}\n"
        "fn makeCatchArrayClosure() -> () -> () {\n"
        "  try { fail() } catch (e: CatchErr) {\n"
        "    const box = [e]\n"
        "    return fn() { throw box[0] }\n"
        "  }\n"
        "  return fn() { }\n"
        "}\n"
        "fn catchClosureArrayRethrows() {\n"
        "  var run = makeCatchArrayClosure()\n"
        "  run()\n"
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
    const XaEffectSummary *bare_enum_pattern_handles_all =
        analyzer_function_effect_summary(a, "bareEnumPatternHandlesAll");
    const XaEffectSummary *variant_pattern_handles_only_boom =
        analyzer_function_effect_summary(a, "variantPatternHandlesOnlyBoom");
    const XaEffectSummary *variant_pattern_leaks_other =
        analyzer_function_effect_summary(a, "variantPatternLeaksOther");
    const XaEffectSummary *payload_pattern_handles_payload =
        analyzer_function_effect_summary(a, "payloadPatternHandlesPayload");
    const XaEffectSummary *variant_pattern_leaves_rest =
        analyzer_function_effect_summary(a, "variantPatternLeavesRest");
    const XaEffectSummary *variant_pattern_body_throws =
        analyzer_function_effect_summary(a, "variantPatternBodyThrows");
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
    const XaEffectSummary *catch_if_else_alias_rethrows =
        analyzer_function_effect_summary(a, "catchIfElseAliasRethrows");
    const XaEffectSummary *catch_if_else_alias_invalidates =
        analyzer_function_effect_summary(a, "catchIfElseAliasInvalidates");
    const XaEffectSummary *catch_while_alias_does_not_leak =
        analyzer_function_effect_summary(a, "catchWhileAliasDoesNotLeak");
    const XaEffectSummary *catch_while_alias_preserves =
        analyzer_function_effect_summary(a, "catchWhileAliasPreserves");
    const XaEffectSummary *catch_while_alias_invalidates =
        analyzer_function_effect_summary(a, "catchWhileAliasInvalidates");
    const XaEffectSummary *catch_try_alias_preserves =
        analyzer_function_effect_summary(a, "catchTryAliasPreserves");
    const XaEffectSummary *catch_try_inner_alias_does_not_clobber_outer =
        analyzer_function_effect_summary(a, "catchTryInnerAliasDoesNotClobberOuter");
    const XaEffectSummary *catch_try_alias_does_not_leak =
        analyzer_function_effect_summary(a, "catchTryAliasDoesNotLeak");
    const XaEffectSummary *catch_try_alias_invalidates =
        analyzer_function_effect_summary(a, "catchTryAliasInvalidates");
    const XaEffectSummary *catch_closure_binding_rethrows =
        analyzer_function_effect_summary(a, "catchClosureBindingRethrows");
    const XaEffectSummary *catch_closure_alias_rethrows =
        analyzer_function_effect_summary(a, "catchClosureAliasRethrows");
    const XaEffectSummary *catch_closure_alias_invalidates =
        analyzer_function_effect_summary(a, "catchClosureAliasInvalidates");
    const XaEffectSummary *catch_array_alias_rethrows =
        analyzer_function_effect_summary(a, "catchArrayAliasRethrows");
    const XaEffectSummary *catch_array_alias_invalidates =
        analyzer_function_effect_summary(a, "catchArrayAliasInvalidates");
    const XaEffectSummary *catch_dynamic_index_rethrows =
        analyzer_function_effect_summary(a, "catchDynamicIndexRethrows");
    const XaEffectSummary *catch_dynamic_index_mismatch_rethrows =
        analyzer_function_effect_summary(a, "catchDynamicIndexMismatchRethrows");
    const XaEffectSummary *catch_reassigned_dynamic_index_invalidates =
        analyzer_function_effect_summary(a, "catchReassignedDynamicIndexInvalidates");
    const XaEffectSummary *catch_array_other_slot_mutation_preserves =
        analyzer_function_effect_summary(a, "catchArrayOtherSlotMutationPreserves");
    const XaEffectSummary *catch_struct_field_alias_rethrows =
        analyzer_function_effect_summary(a, "catchStructFieldAliasRethrows");
    const XaEffectSummary *catch_struct_field_alias_invalidates =
        analyzer_function_effect_summary(a, "catchStructFieldAliasInvalidates");
    const XaEffectSummary *catch_struct_other_field_mutation_preserves =
        analyzer_function_effect_summary(a, "catchStructOtherFieldMutationPreserves");
    const XaEffectSummary *catch_closure_array_rethrows =
        analyzer_function_effect_summary(a, "catchClosureArrayRethrows");
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
    ASSERT(bare_enum_pattern_handles_all != NULL);
    ASSERT(variant_pattern_handles_only_boom != NULL);
    ASSERT(variant_pattern_leaks_other != NULL);
    ASSERT(payload_pattern_handles_payload != NULL);
    ASSERT(variant_pattern_leaves_rest != NULL);
    ASSERT(variant_pattern_body_throws != NULL);
    ASSERT(catch_all_rethrows != NULL);
    ASSERT(typed_rethrows != NULL);
    ASSERT(catch_var_reassigned_rethrows != NULL);
    ASSERT(catch_assignment_alias_rethrows != NULL);
    ASSERT(catch_nested_assignment_alias_rethrows != NULL);
    ASSERT(catch_nested_alias_invalidates != NULL);
    ASSERT(catch_nested_local_alias_does_not_leak != NULL);
    ASSERT(catch_if_alias_does_not_leak != NULL);
    ASSERT(catch_if_else_alias_rethrows != NULL);
    ASSERT(catch_if_else_alias_invalidates != NULL);
    ASSERT(catch_while_alias_does_not_leak != NULL);
    ASSERT(catch_while_alias_preserves != NULL);
    ASSERT(catch_while_alias_invalidates != NULL);
    ASSERT(catch_try_alias_preserves != NULL);
    ASSERT(catch_try_inner_alias_does_not_clobber_outer != NULL);
    ASSERT(catch_try_alias_does_not_leak != NULL);
    ASSERT(catch_try_alias_invalidates != NULL);
    ASSERT(catch_closure_binding_rethrows != NULL);
    ASSERT(catch_closure_alias_rethrows != NULL);
    ASSERT(catch_closure_alias_invalidates != NULL);
    ASSERT(catch_array_alias_rethrows != NULL);
    ASSERT(catch_array_alias_invalidates != NULL);
    ASSERT(catch_dynamic_index_rethrows != NULL);
    ASSERT(catch_dynamic_index_mismatch_rethrows != NULL);
    ASSERT(catch_reassigned_dynamic_index_invalidates != NULL);
    ASSERT(catch_array_other_slot_mutation_preserves != NULL);
    ASSERT(catch_struct_field_alias_rethrows != NULL);
    ASSERT(catch_struct_field_alias_invalidates != NULL);
    ASSERT(catch_struct_other_field_mutation_preserves != NULL);
    ASSERT(catch_closure_array_rethrows != NULL);
    ASSERT(wrong_typed_rethrow != NULL);
    ASSERT(catch_all_alias_rethrows != NULL);
    ASSERT(catch_all_var_alias_rethrows != NULL);
    ASSERT(typed_alias_rethrows != NULL);
    ASSERT(wrong_typed_alias_rethrow != NULL);

    ASSERT(effect_summary_has_enum_named(a, fail, "CatchErr"));
    ASSERT(handled->escaping.count == 0);
    ASSERT(effect_summary_has_enum_named(a, leaks, "CatchErr"));
    ASSERT(catches_all->escaping.count == 0);
    ASSERT(bare_enum_pattern_handles_all->escaping.count == 0);
    ASSERT(variant_pattern_handles_only_boom->escaping.count == 0);
    ASSERT(payload_pattern_handles_payload->escaping.count == 0);
    const XaErrorTypeSet *variant_leak_set =
        effect_summary_enum_set_named(a, variant_pattern_leaks_other, "PayloadErr");
    ASSERT(variant_leak_set != NULL);
    ASSERT(!variant_leak_set->all_variants);
    ASSERT(!xa_bitset_test(&variant_leak_set->variants, 0));
    ASSERT(xa_bitset_test(&variant_leak_set->variants, 1));
    ASSERT(!xa_bitset_test(&variant_leak_set->variants, 2));
    const XaErrorTypeSet *variant_rest_set =
        effect_summary_enum_set_named(a, variant_pattern_leaves_rest, "PayloadErr");
    ASSERT(variant_rest_set != NULL);
    ASSERT(!variant_rest_set->all_variants);
    ASSERT(!xa_bitset_test(&variant_rest_set->variants, 0));
    ASSERT(xa_bitset_test(&variant_rest_set->variants, 1));
    ASSERT(xa_bitset_test(&variant_rest_set->variants, 2));
    ASSERT(!effect_summary_has_enum_named(a, variant_pattern_body_throws, "PayloadErr"));
    ASSERT(effect_summary_has_enum_named(a, variant_pattern_body_throws, "OtherErr"));
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
    const XaErrorTypeSet *if_else_alias_set =
        effect_summary_enum_set_named(a, catch_if_else_alias_rethrows, "CatchErr");
    ASSERT(if_else_alias_set != NULL);
    ASSERT(!if_else_alias_set->all_variants);
    ASSERT(xa_bitset_test(&if_else_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&if_else_alias_set->variants, 1));
    const XaErrorTypeSet *if_else_invalidated_alias_set =
        effect_summary_enum_set_named(a, catch_if_else_alias_invalidates, "CatchErr");
    ASSERT(if_else_invalidated_alias_set != NULL);
    ASSERT(if_else_invalidated_alias_set->all_variants);
    const XaErrorTypeSet *while_leak_alias_set =
        effect_summary_enum_set_named(a, catch_while_alias_does_not_leak, "CatchErr");
    ASSERT(while_leak_alias_set != NULL);
    ASSERT(while_leak_alias_set->all_variants);
    const XaErrorTypeSet *while_preserved_alias_set =
        effect_summary_enum_set_named(a, catch_while_alias_preserves, "CatchErr");
    ASSERT(while_preserved_alias_set != NULL);
    ASSERT(!while_preserved_alias_set->all_variants);
    ASSERT(xa_bitset_test(&while_preserved_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&while_preserved_alias_set->variants, 1));
    const XaErrorTypeSet *while_invalidated_alias_set =
        effect_summary_enum_set_named(a, catch_while_alias_invalidates, "CatchErr");
    ASSERT(while_invalidated_alias_set != NULL);
    ASSERT(while_invalidated_alias_set->all_variants);
    const XaErrorTypeSet *try_preserved_alias_set =
        effect_summary_enum_set_named(a, catch_try_alias_preserves, "CatchErr");
    ASSERT(try_preserved_alias_set != NULL);
    ASSERT(!try_preserved_alias_set->all_variants);
    ASSERT(xa_bitset_test(&try_preserved_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&try_preserved_alias_set->variants, 1));
    const XaErrorTypeSet *try_inner_alias_set =
        effect_summary_enum_set_named(a, catch_try_inner_alias_does_not_clobber_outer, "CatchErr");
    ASSERT(try_inner_alias_set != NULL);
    ASSERT(!try_inner_alias_set->all_variants);
    ASSERT(xa_bitset_test(&try_inner_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&try_inner_alias_set->variants, 1));
    const XaErrorTypeSet *try_leak_alias_set =
        effect_summary_enum_set_named(a, catch_try_alias_does_not_leak, "CatchErr");
    ASSERT(try_leak_alias_set != NULL);
    ASSERT(try_leak_alias_set->all_variants);
    const XaErrorTypeSet *try_invalidated_alias_set =
        effect_summary_enum_set_named(a, catch_try_alias_invalidates, "CatchErr");
    ASSERT(try_invalidated_alias_set != NULL);
    ASSERT(try_invalidated_alias_set->all_variants);
    const XaErrorTypeSet *closure_binding_set =
        effect_summary_enum_set_named(a, catch_closure_binding_rethrows, "CatchErr");
    ASSERT(closure_binding_set != NULL);
    ASSERT(!closure_binding_set->all_variants);
    ASSERT(xa_bitset_test(&closure_binding_set->variants, 0));
    ASSERT(!xa_bitset_test(&closure_binding_set->variants, 1));
    const XaErrorTypeSet *closure_alias_set =
        effect_summary_enum_set_named(a, catch_closure_alias_rethrows, "CatchErr");
    ASSERT(closure_alias_set != NULL);
    ASSERT(!closure_alias_set->all_variants);
    ASSERT(xa_bitset_test(&closure_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&closure_alias_set->variants, 1));
    const XaErrorTypeSet *closure_invalidated_set =
        effect_summary_enum_set_named(a, catch_closure_alias_invalidates, "CatchErr");
    ASSERT(closure_invalidated_set != NULL);
    ASSERT(closure_invalidated_set->all_variants);
    const XaErrorTypeSet *array_alias_set =
        effect_summary_enum_set_named(a, catch_array_alias_rethrows, "CatchErr");
    ASSERT(array_alias_set != NULL);
    ASSERT(!array_alias_set->all_variants);
    ASSERT(xa_bitset_test(&array_alias_set->variants, 0));
    ASSERT(!xa_bitset_test(&array_alias_set->variants, 1));
    const XaErrorTypeSet *array_invalidated_set =
        effect_summary_enum_set_named(a, catch_array_alias_invalidates, "CatchErr");
    ASSERT(array_invalidated_set != NULL);
    ASSERT(array_invalidated_set->all_variants);
    const XaErrorTypeSet *dynamic_index_set =
        effect_summary_enum_set_named(a, catch_dynamic_index_rethrows, "CatchErr");
    ASSERT(dynamic_index_set != NULL);
    ASSERT(!dynamic_index_set->all_variants);
    ASSERT(xa_bitset_test(&dynamic_index_set->variants, 0));
    ASSERT(!xa_bitset_test(&dynamic_index_set->variants, 1));
    const XaErrorTypeSet *dynamic_index_mismatch_set =
        effect_summary_enum_set_named(a, catch_dynamic_index_mismatch_rethrows, "CatchErr");
    ASSERT(dynamic_index_mismatch_set != NULL);
    ASSERT(dynamic_index_mismatch_set->all_variants);
    const XaErrorTypeSet *reassigned_dynamic_index_set =
        effect_summary_enum_set_named(a, catch_reassigned_dynamic_index_invalidates, "CatchErr");
    ASSERT(reassigned_dynamic_index_set != NULL);
    ASSERT(reassigned_dynamic_index_set->all_variants);
    const XaErrorTypeSet *other_slot_preserved_set =
        effect_summary_enum_set_named(a, catch_array_other_slot_mutation_preserves, "CatchErr");
    ASSERT(other_slot_preserved_set != NULL);
    ASSERT(!other_slot_preserved_set->all_variants);
    ASSERT(xa_bitset_test(&other_slot_preserved_set->variants, 0));
    ASSERT(!xa_bitset_test(&other_slot_preserved_set->variants, 1));
    const XaErrorTypeSet *struct_field_set =
        effect_summary_enum_set_named(a, catch_struct_field_alias_rethrows, "CatchErr");
    ASSERT(struct_field_set != NULL);
    ASSERT(!struct_field_set->all_variants);
    ASSERT(xa_bitset_test(&struct_field_set->variants, 0));
    ASSERT(!xa_bitset_test(&struct_field_set->variants, 1));
    const XaErrorTypeSet *struct_field_invalidated_set =
        effect_summary_enum_set_named(a, catch_struct_field_alias_invalidates, "CatchErr");
    ASSERT(struct_field_invalidated_set != NULL);
    ASSERT(struct_field_invalidated_set->all_variants);
    const XaErrorTypeSet *other_field_preserved_set =
        effect_summary_enum_set_named(a, catch_struct_other_field_mutation_preserves, "CatchErr");
    ASSERT(other_field_preserved_set != NULL);
    ASSERT(!other_field_preserved_set->all_variants);
    ASSERT(xa_bitset_test(&other_field_preserved_set->variants, 0));
    ASSERT(!xa_bitset_test(&other_field_preserved_set->variants, 1));
    const XaErrorTypeSet *closure_array_set =
        effect_summary_enum_set_named(a, catch_closure_array_rethrows, "CatchErr");
    ASSERT(closure_array_set != NULL);
    ASSERT(!closure_array_set->all_variants);
    ASSERT(xa_bitset_test(&closure_array_set->variants, 0));
    ASSERT(!xa_bitset_test(&closure_array_set->variants, 1));
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

TEST(analyzer_error_effect_marks_invalid_program_partial_facts) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum PartialErr { Boom, Other }\n"
                         "fn invalidPartial() { throw PartialErr.Boom; missingPartial() }\n"
                         "fn invalidEmpty() { missingEmpty() }\n"
                         "fn callsInvalid() { invalidEmpty() }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_invalid_partial.xr", program);

    const XaEffectSummary *partial = analyzer_function_effect_summary(a, "invalidPartial");
    const XaEffectSummary *empty = analyzer_function_effect_summary(a, "invalidEmpty");
    const XaEffectSummary *caller = analyzer_function_effect_summary(a, "callsInvalid");
    ASSERT(partial != NULL);
    ASSERT(empty != NULL);
    ASSERT(caller != NULL);
    ASSERT(partial->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((partial->error_unknown_reasons & XA_UNKNOWN_INVALID_PROGRAM) != 0);
    const XaErrorTypeSet *partial_set = effect_summary_enum_set_named(a, partial, "PartialErr");
    ASSERT(partial_set != NULL);
    ASSERT(!partial_set->all_variants);
    ASSERT(xa_bitset_test(&partial_set->variants, 0));
    ASSERT(!xa_bitset_test(&partial_set->variants, 1));
    ASSERT(empty->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((empty->error_unknown_reasons & XA_UNKNOWN_INVALID_PROGRAM) != 0);
    ASSERT(!xa_effect_summary_is_nothrow(empty));
    ASSERT(caller->error_set_completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((caller->error_unknown_reasons & XA_UNKNOWN_INVALID_PROGRAM) != 0);
    ASSERT(!xa_effect_summary_is_nothrow(caller));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_tracks_map_catch_aliases) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source =
        "enum MapErr { Boom, Other }\n"
        "fn failMap() { throw MapErr.Boom }\n"
        "fn failMapEither(flag: bool) {\n"
        "  if (flag) { throw MapErr.Boom } else { throw MapErr.Other }\n"
        "}\n"
        "fn mapLiteralRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapOtherKeyMutationPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e, \"other\": e}\n"
        "    box[\"other\"] = MapErr.Other\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapCaughtKeyMutationInvalidates() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box[\"caught\"] = MapErr.Other\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapDynamicKeyRethrows(key: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box[key] = e\n"
        "    throw box[key]\n"
        "  }\n"
        "}\n"
        "fn mapStableLocalKeyRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const key = \"caught\"\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box[key] = e\n"
        "    throw box[key]\n"
        "  }\n"
        "}\n"
        "fn mapDynamicKeyMutationInvalidates(key: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box[key] = e\n"
        "    box[key] = MapErr.Other\n"
        "    throw box[key]\n"
        "  }\n"
        "}\n"
        "fn mapDynamicMethodSetInvalidatesSlot(key: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.set(key, MapErr.Other)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapDynamicDeleteInvalidatesSlot(key: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.delete(key)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapDynamicDeleteValuesPreserve(key: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.delete(key)\n"
        "    const values = box.values()\n"
        "    for (item in values) { throw item }\n"
        "  }\n"
        "}\n"
        "fn mapDynamicKeyMismatch(key: string, other: string) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box[key] = e\n"
        "    throw box[other]\n"
        "  }\n"
        "}\n"
        "fn mapContainsKeyPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.containsKey(\"other\")\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapReadOnlyViewsPreserve() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.get(\"other\")\n"
        "    box.containsValue(MapErr.Other)\n"
        "    box.keys()\n"
        "    box.values()\n"
        "    box.entries()\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapLenPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    len(box)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapSetCaughtKeyPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box.set(\"caught\", e)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapSetOtherKeyPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.set(\"other\", MapErr.Other)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapSetCaughtKeyInvalidates() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.set(\"caught\", MapErr.Other)\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapDeleteOtherKeyPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    box.delete(\"other\")\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapDeleteOtherKeyValuesIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    box.delete(\"other\")\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteOtherKeyKeysIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other, MapErr.Other: MapErr.Boom}\n"
        "    box.delete(MapErr.Other)\n"
        "    for (key in box.keys()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteOtherKeyEntriesKeyIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other, MapErr.Other: MapErr.Boom}\n"
        "    box.delete(MapErr.Other)\n"
        "    for (entry in box.entries()) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteOtherKeyMayAliasCaughtKeyFallsBack(flag: bool) {\n"
        "  try { failMapEither(flag) } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other, MapErr.Other: MapErr.Boom}\n"
        "    box.delete(MapErr.Other)\n"
        "    for (key in box.keys()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteCaughtKeyInvalidates() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.delete(\"caught\")\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapClearInvalidates() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.clear()\n"
        "    throw box[\"caught\"]\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueMixedIteratorFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapSingleKeyIteratorDoesNotAliasValue() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{MapErr.Other: e}\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapSingleKeyIteratorRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for (key, value in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorDoesNotAliasCaughtKeyToValue() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorDoesNotAliasCaughtValueToKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{MapErr.Other: e}\n"
        "    for (key, value in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorBothBindingsRethrow(flag: bool) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: e}\n"
        "    for (key, value in box) { if (flag) { throw key } else { throw value } }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorMixedKeyBothBindingsFallsBack(flag: bool) {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: e, MapErr.Other: e}\n"
        "    for (key, value in box) { if (flag) { throw key } else { throw value } }\n"
        "  }\n"
        "}\n"
        "fn mapKeysViewIteratorRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    const keys = box.keys()\n"
        "    for (key in keys) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDirectKeysViewIteratorRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for (key in box.keys()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDirectEntriesViewIteratorKeyRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for (entry in box.entries()) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDirectEntriesViewIteratorValueRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    for (entry in box.entries()) { const value: MapErr = entry[1]; throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesTupleDestructureKeyRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for ((key, value) in box.entries()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesTupleDestructureValueRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    for ((key, value) in box.entries()) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesTupleDestructureDoesNotAliasCaughtKeyToValue() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    for ((key, value) in box.entries()) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesTupleDestructureDoesNotAliasCaughtValueToKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{MapErr.Other: e}\n"
        "    for ((key, value) in box.entries()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesIteratorKeyRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesMixedIteratorKeyFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: MapErr.Other, MapErr.Other: MapErr.Boom}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapVarKeyIteratorRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyIteratorSetOtherKeyFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.set(MapErr.Other, MapErr.Boom)\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueIteratorSetOtherKeyFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.set(MapErr.Other, MapErr.Boom)\n"
        "    for (key, value in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeySetCaughtIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<MapErr, MapErr> = #{}\n"
        "    box.set(e, MapErr.Other)\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeySetCaughtOnMixedMapFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{MapErr.Other: MapErr.Boom}\n"
        "    box.set(e, MapErr.Other)\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueSetCaughtIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box.set(\"caught\", e)\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapKeyValueSetOtherIteratorFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.set(\"other\", MapErr.Other)\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapValuesViewSetCaughtOtherKeyPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.set(\"other\", e)\n"
        "    const values = box.values()\n"
        "    for (value in values) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapValueSetCaughtOnMixedMapFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"other\": MapErr.Other}\n"
        "    box.set(\"caught\", e)\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapClearValuesIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.clear()\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapClearKeysIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.clear()\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapClearValuesViewIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.clear()\n"
        "    for (value in box.values()) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapClearKeysViewIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.clear()\n"
        "    for (key in box.keys()) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapClearEntriesViewKeyIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.clear()\n"
        "    for (entry in box.entries()) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapClearEntriesViewValueIteratorPreserves() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.clear()\n"
        "    for (entry in box.entries()) { const value: MapErr = entry[1]; throw value }\n"
        "  }\n"
        "}\n"
        "fn mapClearThenSetCaughtValueIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"other\": MapErr.Other}\n"
        "    box.clear()\n"
        "    box.set(\"caught\", e)\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapClearThenSetCaughtKeyIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{MapErr.Other: MapErr.Other}\n"
        "    box.clear()\n"
        "    box.set(e, MapErr.Other)\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteSingletonThenSetCaughtValueIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box.delete(\"caught\")\n"
        "    box.set(\"again\", e)\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapDeleteSingletonCaughtKeyThenSetCaughtKeyIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box.delete(e)\n"
        "    box.set(e, MapErr.Other)\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapIndexSetCaughtValueIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<string, MapErr> = #{}\n"
        "    box[\"caught\"] = e\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapIndexSetOtherValueIteratorFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{\"caught\": e}\n"
        "    box[\"other\"] = MapErr.Other\n"
        "    for (key, value in box) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapIndexSetCaughtKeyIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box: Map<MapErr, MapErr> = #{}\n"
        "    box[e] = MapErr.Other\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapIndexSetOtherKeyIteratorFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    var box = #{e: MapErr.Other}\n"
        "    box[MapErr.Other] = MapErr.Boom\n"
        "    for (key in box) { throw key }\n"
        "  }\n"
        "}\n"
        "fn mapValuesIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    const values = box.values()\n"
        "    for (value in values) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapDirectValuesViewIteratorRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    for (value in box.values()) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapValuesMixedIteratorFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    const values = box.values()\n"
        "    for (value in values) { throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesIteratorValueRethrows() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const value: MapErr = entry[1]; throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesIteratorBothSlotsRethrowsCaughtKey() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: e}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const key: MapErr = entry[0]; throw key }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesIteratorBothSlotsRethrowsCaughtValue() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{e: e}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const value: MapErr = entry[1]; throw value }\n"
        "  }\n"
        "}\n"
        "fn mapEntriesMixedIteratorValueFallsBack() {\n"
        "  try { failMap() } catch (e: MapErr) {\n"
        "    const box = #{\"caught\": e, \"other\": MapErr.Other}\n"
        "    const entries = box.entries()\n"
        "    for (entry in entries) { const value: MapErr = entry[1]; throw value }\n"
        "  }\n"
        "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_map_catch_alias.xr", program);

    const XaEffectSummary *literal = analyzer_function_effect_summary(a, "mapLiteralRethrows");
    const XaEffectSummary *other_key =
        analyzer_function_effect_summary(a, "mapOtherKeyMutationPreserves");
    const XaEffectSummary *invalidated =
        analyzer_function_effect_summary(a, "mapCaughtKeyMutationInvalidates");
    const XaEffectSummary *dynamic = analyzer_function_effect_summary(a, "mapDynamicKeyRethrows");
    const XaEffectSummary *stable_local_key =
        analyzer_function_effect_summary(a, "mapStableLocalKeyRethrows");
    const XaEffectSummary *dynamic_invalidated =
        analyzer_function_effect_summary(a, "mapDynamicKeyMutationInvalidates");
    const XaEffectSummary *dynamic_method_set =
        analyzer_function_effect_summary(a, "mapDynamicMethodSetInvalidatesSlot");
    const XaEffectSummary *dynamic_delete_slot =
        analyzer_function_effect_summary(a, "mapDynamicDeleteInvalidatesSlot");
    const XaEffectSummary *dynamic_delete_values =
        analyzer_function_effect_summary(a, "mapDynamicDeleteValuesPreserve");
    const XaEffectSummary *mismatch = analyzer_function_effect_summary(a, "mapDynamicKeyMismatch");
    const XaEffectSummary *contains_key =
        analyzer_function_effect_summary(a, "mapContainsKeyPreserves");
    const XaEffectSummary *readonly_views =
        analyzer_function_effect_summary(a, "mapReadOnlyViewsPreserve");
    const XaEffectSummary *len_preserve = analyzer_function_effect_summary(a, "mapLenPreserves");
    const XaEffectSummary *set_caught =
        analyzer_function_effect_summary(a, "mapSetCaughtKeyPreserves");
    const XaEffectSummary *set_other =
        analyzer_function_effect_summary(a, "mapSetOtherKeyPreserves");
    const XaEffectSummary *set_invalidated =
        analyzer_function_effect_summary(a, "mapSetCaughtKeyInvalidates");
    const XaEffectSummary *delete_other =
        analyzer_function_effect_summary(a, "mapDeleteOtherKeyPreserves");
    const XaEffectSummary *delete_other_values_iter =
        analyzer_function_effect_summary(a, "mapDeleteOtherKeyValuesIteratorPreserves");
    const XaEffectSummary *delete_other_keys_iter =
        analyzer_function_effect_summary(a, "mapDeleteOtherKeyKeysIteratorPreserves");
    const XaEffectSummary *delete_other_entries_key_iter =
        analyzer_function_effect_summary(a, "mapDeleteOtherKeyEntriesKeyIteratorPreserves");
    const XaEffectSummary *delete_other_may_alias_key =
        analyzer_function_effect_summary(a, "mapDeleteOtherKeyMayAliasCaughtKeyFallsBack");
    const XaEffectSummary *delete_invalidated =
        analyzer_function_effect_summary(a, "mapDeleteCaughtKeyInvalidates");
    const XaEffectSummary *clear_invalidated =
        analyzer_function_effect_summary(a, "mapClearInvalidates");
    const XaEffectSummary *kv_iter =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorRethrows");
    const XaEffectSummary *kv_mixed =
        analyzer_function_effect_summary(a, "mapKeyValueMixedIteratorFallsBack");
    const XaEffectSummary *single_key =
        analyzer_function_effect_summary(a, "mapSingleKeyIteratorDoesNotAliasValue");
    const XaEffectSummary *single_caught_key =
        analyzer_function_effect_summary(a, "mapSingleKeyIteratorRethrowsCaughtKey");
    const XaEffectSummary *kv_caught_key =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorRethrowsCaughtKey");
    const XaEffectSummary *kv_no_key_to_value =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorDoesNotAliasCaughtKeyToValue");
    const XaEffectSummary *kv_no_value_to_key =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorDoesNotAliasCaughtValueToKey");
    const XaEffectSummary *kv_both =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorBothBindingsRethrow");
    const XaEffectSummary *kv_both_mixed_key =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorMixedKeyBothBindingsFallsBack");
    const XaEffectSummary *keys_view =
        analyzer_function_effect_summary(a, "mapKeysViewIteratorRethrowsCaughtKey");
    const XaEffectSummary *direct_keys_view =
        analyzer_function_effect_summary(a, "mapDirectKeysViewIteratorRethrowsCaughtKey");
    const XaEffectSummary *direct_entries_key =
        analyzer_function_effect_summary(a, "mapDirectEntriesViewIteratorKeyRethrows");
    const XaEffectSummary *direct_entries_value =
        analyzer_function_effect_summary(a, "mapDirectEntriesViewIteratorValueRethrows");
    const XaEffectSummary *tuple_entries_key =
        analyzer_function_effect_summary(a, "mapEntriesTupleDestructureKeyRethrows");
    const XaEffectSummary *tuple_entries_value =
        analyzer_function_effect_summary(a, "mapEntriesTupleDestructureValueRethrows");
    const XaEffectSummary *tuple_entries_no_key_to_value = analyzer_function_effect_summary(
        a, "mapEntriesTupleDestructureDoesNotAliasCaughtKeyToValue");
    const XaEffectSummary *tuple_entries_no_value_to_key = analyzer_function_effect_summary(
        a, "mapEntriesTupleDestructureDoesNotAliasCaughtValueToKey");
    const XaEffectSummary *entries_key =
        analyzer_function_effect_summary(a, "mapEntriesIteratorKeyRethrows");
    const XaEffectSummary *entries_key_mixed =
        analyzer_function_effect_summary(a, "mapEntriesMixedIteratorKeyFallsBack");
    const XaEffectSummary *var_key =
        analyzer_function_effect_summary(a, "mapVarKeyIteratorRethrowsCaughtKey");
    const XaEffectSummary *key_set_other =
        analyzer_function_effect_summary(a, "mapKeyIteratorSetOtherKeyFallsBack");
    const XaEffectSummary *kv_key_set_other =
        analyzer_function_effect_summary(a, "mapKeyValueIteratorSetOtherKeyFallsBack");
    const XaEffectSummary *key_set_caught =
        analyzer_function_effect_summary(a, "mapKeySetCaughtIteratorRethrows");
    const XaEffectSummary *key_set_caught_mixed =
        analyzer_function_effect_summary(a, "mapKeySetCaughtOnMixedMapFallsBack");
    const XaEffectSummary *kv_set_caught =
        analyzer_function_effect_summary(a, "mapKeyValueSetCaughtIteratorRethrows");
    const XaEffectSummary *kv_set_other =
        analyzer_function_effect_summary(a, "mapKeyValueSetOtherIteratorFallsBack");
    const XaEffectSummary *values_view_set_caught_other =
        analyzer_function_effect_summary(a, "mapValuesViewSetCaughtOtherKeyPreserves");
    const XaEffectSummary *value_set_caught_mixed =
        analyzer_function_effect_summary(a, "mapValueSetCaughtOnMixedMapFallsBack");
    const XaEffectSummary *clear_values =
        analyzer_function_effect_summary(a, "mapClearValuesIteratorPreserves");
    const XaEffectSummary *clear_keys =
        analyzer_function_effect_summary(a, "mapClearKeysIteratorPreserves");
    const XaEffectSummary *clear_values_view =
        analyzer_function_effect_summary(a, "mapClearValuesViewIteratorPreserves");
    const XaEffectSummary *clear_keys_view =
        analyzer_function_effect_summary(a, "mapClearKeysViewIteratorPreserves");
    const XaEffectSummary *clear_entries_key_view =
        analyzer_function_effect_summary(a, "mapClearEntriesViewKeyIteratorPreserves");
    const XaEffectSummary *clear_entries_value_view =
        analyzer_function_effect_summary(a, "mapClearEntriesViewValueIteratorPreserves");
    const XaEffectSummary *clear_then_set_value =
        analyzer_function_effect_summary(a, "mapClearThenSetCaughtValueIteratorRethrows");
    const XaEffectSummary *clear_then_set_key =
        analyzer_function_effect_summary(a, "mapClearThenSetCaughtKeyIteratorRethrows");
    const XaEffectSummary *delete_singleton_then_set_value =
        analyzer_function_effect_summary(a, "mapDeleteSingletonThenSetCaughtValueIteratorRethrows");
    const XaEffectSummary *delete_singleton_then_set_key = analyzer_function_effect_summary(
        a, "mapDeleteSingletonCaughtKeyThenSetCaughtKeyIteratorRethrows");
    const XaEffectSummary *index_set_caught_value =
        analyzer_function_effect_summary(a, "mapIndexSetCaughtValueIteratorRethrows");
    const XaEffectSummary *index_set_other_value =
        analyzer_function_effect_summary(a, "mapIndexSetOtherValueIteratorFallsBack");
    const XaEffectSummary *index_set_caught_key =
        analyzer_function_effect_summary(a, "mapIndexSetCaughtKeyIteratorRethrows");
    const XaEffectSummary *index_set_other_key =
        analyzer_function_effect_summary(a, "mapIndexSetOtherKeyIteratorFallsBack");
    const XaEffectSummary *values_iter =
        analyzer_function_effect_summary(a, "mapValuesIteratorRethrows");
    const XaEffectSummary *direct_values_iter =
        analyzer_function_effect_summary(a, "mapDirectValuesViewIteratorRethrows");
    const XaEffectSummary *values_mixed =
        analyzer_function_effect_summary(a, "mapValuesMixedIteratorFallsBack");
    const XaEffectSummary *entries_iter =
        analyzer_function_effect_summary(a, "mapEntriesIteratorValueRethrows");
    const XaEffectSummary *entries_both_key =
        analyzer_function_effect_summary(a, "mapEntriesIteratorBothSlotsRethrowsCaughtKey");
    const XaEffectSummary *entries_both_value =
        analyzer_function_effect_summary(a, "mapEntriesIteratorBothSlotsRethrowsCaughtValue");
    const XaEffectSummary *entries_mixed =
        analyzer_function_effect_summary(a, "mapEntriesMixedIteratorValueFallsBack");
    ASSERT(literal != NULL);
    ASSERT(other_key != NULL);
    ASSERT(invalidated != NULL);
    ASSERT(dynamic != NULL);
    ASSERT(stable_local_key != NULL);
    ASSERT(dynamic_invalidated != NULL);
    ASSERT(dynamic_method_set != NULL);
    ASSERT(dynamic_delete_slot != NULL);
    ASSERT(dynamic_delete_values != NULL);
    ASSERT(mismatch != NULL);
    ASSERT(contains_key != NULL);
    ASSERT(readonly_views != NULL);
    ASSERT(set_caught != NULL);
    ASSERT(set_other != NULL);
    ASSERT(set_invalidated != NULL);
    ASSERT(delete_other != NULL);
    ASSERT(delete_other_values_iter != NULL);
    ASSERT(delete_other_keys_iter != NULL);
    ASSERT(delete_other_entries_key_iter != NULL);
    ASSERT(delete_other_may_alias_key != NULL);
    ASSERT(delete_invalidated != NULL);
    ASSERT(clear_invalidated != NULL);
    ASSERT(kv_iter != NULL);
    ASSERT(kv_mixed != NULL);
    ASSERT(single_key != NULL);
    ASSERT(single_caught_key != NULL);
    ASSERT(kv_caught_key != NULL);
    ASSERT(kv_no_key_to_value != NULL);
    ASSERT(kv_no_value_to_key != NULL);
    ASSERT(kv_both != NULL);
    ASSERT(kv_both_mixed_key != NULL);
    ASSERT(keys_view != NULL);
    ASSERT(direct_keys_view != NULL);
    ASSERT(direct_entries_key != NULL);
    ASSERT(direct_entries_value != NULL);
    ASSERT(tuple_entries_key != NULL);
    ASSERT(tuple_entries_value != NULL);
    ASSERT(tuple_entries_no_key_to_value != NULL);
    ASSERT(tuple_entries_no_value_to_key != NULL);
    ASSERT(entries_key != NULL);
    ASSERT(entries_key_mixed != NULL);
    ASSERT(var_key != NULL);
    ASSERT(key_set_other != NULL);
    ASSERT(kv_key_set_other != NULL);
    ASSERT(key_set_caught != NULL);
    ASSERT(key_set_caught_mixed != NULL);
    ASSERT(kv_set_caught != NULL);
    ASSERT(kv_set_other != NULL);
    ASSERT(values_view_set_caught_other != NULL);
    ASSERT(value_set_caught_mixed != NULL);
    ASSERT(clear_values != NULL);
    ASSERT(clear_keys != NULL);
    ASSERT(clear_values_view != NULL);
    ASSERT(clear_keys_view != NULL);
    ASSERT(clear_entries_key_view != NULL);
    ASSERT(clear_entries_value_view != NULL);
    ASSERT(clear_then_set_value != NULL);
    ASSERT(clear_then_set_key != NULL);
    ASSERT(delete_singleton_then_set_value != NULL);
    ASSERT(delete_singleton_then_set_key != NULL);
    ASSERT(index_set_caught_value != NULL);
    ASSERT(index_set_other_value != NULL);
    ASSERT(index_set_caught_key != NULL);
    ASSERT(index_set_other_key != NULL);
    ASSERT(values_iter != NULL);
    ASSERT(direct_values_iter != NULL);
    ASSERT(values_mixed != NULL);
    ASSERT(entries_iter != NULL);
    ASSERT(entries_both_key != NULL);
    ASSERT(entries_both_value != NULL);
    ASSERT(entries_mixed != NULL);

    const XaErrorTypeSet *literal_set = effect_summary_enum_set_named(a, literal, "MapErr");
    const XaErrorTypeSet *other_key_set = effect_summary_enum_set_named(a, other_key, "MapErr");
    const XaErrorTypeSet *invalidated_set = effect_summary_enum_set_named(a, invalidated, "MapErr");
    const XaErrorTypeSet *dynamic_set = effect_summary_enum_set_named(a, dynamic, "MapErr");
    const XaErrorTypeSet *stable_local_key_set =
        effect_summary_enum_set_named(a, stable_local_key, "MapErr");
    const XaErrorTypeSet *dynamic_invalidated_set =
        effect_summary_enum_set_named(a, dynamic_invalidated, "MapErr");
    const XaErrorTypeSet *dynamic_method_set_set =
        effect_summary_enum_set_named(a, dynamic_method_set, "MapErr");
    const XaErrorTypeSet *dynamic_delete_slot_set =
        effect_summary_enum_set_named(a, dynamic_delete_slot, "MapErr");
    const XaErrorTypeSet *dynamic_delete_values_set =
        effect_summary_enum_set_named(a, dynamic_delete_values, "MapErr");
    const XaErrorTypeSet *mismatch_set = effect_summary_enum_set_named(a, mismatch, "MapErr");
    const XaErrorTypeSet *contains_key_set =
        effect_summary_enum_set_named(a, contains_key, "MapErr");
    const XaErrorTypeSet *readonly_views_set =
        effect_summary_enum_set_named(a, readonly_views, "MapErr");
    const XaErrorTypeSet *len_preserve_set =
        effect_summary_enum_set_named(a, len_preserve, "MapErr");
    const XaErrorTypeSet *set_caught_set = effect_summary_enum_set_named(a, set_caught, "MapErr");
    const XaErrorTypeSet *set_other_set = effect_summary_enum_set_named(a, set_other, "MapErr");
    const XaErrorTypeSet *set_invalidated_set =
        effect_summary_enum_set_named(a, set_invalidated, "MapErr");
    const XaErrorTypeSet *delete_other_set =
        effect_summary_enum_set_named(a, delete_other, "MapErr");
    const XaErrorTypeSet *delete_other_values_iter_set =
        effect_summary_enum_set_named(a, delete_other_values_iter, "MapErr");
    const XaErrorTypeSet *delete_other_keys_iter_set =
        effect_summary_enum_set_named(a, delete_other_keys_iter, "MapErr");
    const XaErrorTypeSet *delete_other_entries_key_iter_set =
        effect_summary_enum_set_named(a, delete_other_entries_key_iter, "MapErr");
    const XaErrorTypeSet *delete_other_may_alias_key_set =
        effect_summary_enum_set_named(a, delete_other_may_alias_key, "MapErr");
    const XaErrorTypeSet *delete_invalidated_set =
        effect_summary_enum_set_named(a, delete_invalidated, "MapErr");
    const XaErrorTypeSet *clear_invalidated_set =
        effect_summary_enum_set_named(a, clear_invalidated, "MapErr");
    const XaErrorTypeSet *kv_iter_set = effect_summary_enum_set_named(a, kv_iter, "MapErr");
    const XaErrorTypeSet *kv_mixed_set = effect_summary_enum_set_named(a, kv_mixed, "MapErr");
    const XaErrorTypeSet *single_key_set = effect_summary_enum_set_named(a, single_key, "MapErr");
    const XaErrorTypeSet *single_caught_key_set =
        effect_summary_enum_set_named(a, single_caught_key, "MapErr");
    const XaErrorTypeSet *kv_caught_key_set =
        effect_summary_enum_set_named(a, kv_caught_key, "MapErr");
    const XaErrorTypeSet *kv_no_key_to_value_set =
        effect_summary_enum_set_named(a, kv_no_key_to_value, "MapErr");
    const XaErrorTypeSet *kv_no_value_to_key_set =
        effect_summary_enum_set_named(a, kv_no_value_to_key, "MapErr");
    const XaErrorTypeSet *kv_both_set = effect_summary_enum_set_named(a, kv_both, "MapErr");
    const XaErrorTypeSet *kv_both_mixed_key_set =
        effect_summary_enum_set_named(a, kv_both_mixed_key, "MapErr");
    const XaErrorTypeSet *keys_view_set = effect_summary_enum_set_named(a, keys_view, "MapErr");
    const XaErrorTypeSet *direct_keys_view_set =
        effect_summary_enum_set_named(a, direct_keys_view, "MapErr");
    const XaErrorTypeSet *direct_entries_key_set =
        effect_summary_enum_set_named(a, direct_entries_key, "MapErr");
    const XaErrorTypeSet *direct_entries_value_set =
        effect_summary_enum_set_named(a, direct_entries_value, "MapErr");
    const XaErrorTypeSet *tuple_entries_key_set =
        effect_summary_enum_set_named(a, tuple_entries_key, "MapErr");
    const XaErrorTypeSet *tuple_entries_value_set =
        effect_summary_enum_set_named(a, tuple_entries_value, "MapErr");
    const XaErrorTypeSet *tuple_entries_no_key_to_value_set =
        effect_summary_enum_set_named(a, tuple_entries_no_key_to_value, "MapErr");
    const XaErrorTypeSet *tuple_entries_no_value_to_key_set =
        effect_summary_enum_set_named(a, tuple_entries_no_value_to_key, "MapErr");
    const XaErrorTypeSet *entries_key_set = effect_summary_enum_set_named(a, entries_key, "MapErr");
    const XaErrorTypeSet *entries_key_mixed_set =
        effect_summary_enum_set_named(a, entries_key_mixed, "MapErr");
    const XaErrorTypeSet *var_key_set = effect_summary_enum_set_named(a, var_key, "MapErr");
    const XaErrorTypeSet *key_set_other_set =
        effect_summary_enum_set_named(a, key_set_other, "MapErr");
    const XaErrorTypeSet *kv_key_set_other_set =
        effect_summary_enum_set_named(a, kv_key_set_other, "MapErr");
    const XaErrorTypeSet *key_set_caught_set =
        effect_summary_enum_set_named(a, key_set_caught, "MapErr");
    const XaErrorTypeSet *key_set_caught_mixed_set =
        effect_summary_enum_set_named(a, key_set_caught_mixed, "MapErr");
    const XaErrorTypeSet *kv_set_caught_set =
        effect_summary_enum_set_named(a, kv_set_caught, "MapErr");
    const XaErrorTypeSet *kv_set_other_set =
        effect_summary_enum_set_named(a, kv_set_other, "MapErr");
    const XaErrorTypeSet *values_view_set_caught_other_set =
        effect_summary_enum_set_named(a, values_view_set_caught_other, "MapErr");
    const XaErrorTypeSet *value_set_caught_mixed_set =
        effect_summary_enum_set_named(a, value_set_caught_mixed, "MapErr");
    const XaErrorTypeSet *clear_values_set =
        effect_summary_enum_set_named(a, clear_values, "MapErr");
    const XaErrorTypeSet *clear_keys_set = effect_summary_enum_set_named(a, clear_keys, "MapErr");
    const XaErrorTypeSet *clear_values_view_set =
        effect_summary_enum_set_named(a, clear_values_view, "MapErr");
    const XaErrorTypeSet *clear_keys_view_set =
        effect_summary_enum_set_named(a, clear_keys_view, "MapErr");
    const XaErrorTypeSet *clear_entries_key_view_set =
        effect_summary_enum_set_named(a, clear_entries_key_view, "MapErr");
    const XaErrorTypeSet *clear_entries_value_view_set =
        effect_summary_enum_set_named(a, clear_entries_value_view, "MapErr");
    const XaErrorTypeSet *clear_then_set_value_set =
        effect_summary_enum_set_named(a, clear_then_set_value, "MapErr");
    const XaErrorTypeSet *clear_then_set_key_set =
        effect_summary_enum_set_named(a, clear_then_set_key, "MapErr");
    const XaErrorTypeSet *delete_singleton_then_set_value_set =
        effect_summary_enum_set_named(a, delete_singleton_then_set_value, "MapErr");
    const XaErrorTypeSet *delete_singleton_then_set_key_set =
        effect_summary_enum_set_named(a, delete_singleton_then_set_key, "MapErr");
    const XaErrorTypeSet *index_set_caught_value_set =
        effect_summary_enum_set_named(a, index_set_caught_value, "MapErr");
    const XaErrorTypeSet *index_set_other_value_set =
        effect_summary_enum_set_named(a, index_set_other_value, "MapErr");
    const XaErrorTypeSet *index_set_caught_key_set =
        effect_summary_enum_set_named(a, index_set_caught_key, "MapErr");
    const XaErrorTypeSet *index_set_other_key_set =
        effect_summary_enum_set_named(a, index_set_other_key, "MapErr");
    const XaErrorTypeSet *values_iter_set = effect_summary_enum_set_named(a, values_iter, "MapErr");
    const XaErrorTypeSet *direct_values_iter_set =
        effect_summary_enum_set_named(a, direct_values_iter, "MapErr");
    const XaErrorTypeSet *values_mixed_set =
        effect_summary_enum_set_named(a, values_mixed, "MapErr");
    const XaErrorTypeSet *entries_iter_set =
        effect_summary_enum_set_named(a, entries_iter, "MapErr");
    const XaErrorTypeSet *entries_both_key_set =
        effect_summary_enum_set_named(a, entries_both_key, "MapErr");
    const XaErrorTypeSet *entries_both_value_set =
        effect_summary_enum_set_named(a, entries_both_value, "MapErr");
    const XaErrorTypeSet *entries_mixed_set =
        effect_summary_enum_set_named(a, entries_mixed, "MapErr");
    ASSERT(literal_set != NULL);
    ASSERT(other_key_set != NULL);
    ASSERT(invalidated_set != NULL);
    ASSERT(dynamic_set != NULL);
    ASSERT(stable_local_key_set != NULL);
    ASSERT(dynamic_invalidated_set != NULL);
    ASSERT(dynamic_method_set_set != NULL);
    ASSERT(dynamic_delete_slot_set != NULL);
    ASSERT(dynamic_delete_values_set != NULL);
    ASSERT(mismatch_set != NULL);
    ASSERT(contains_key_set != NULL);
    ASSERT(readonly_views_set != NULL);
    ASSERT(len_preserve_set != NULL);
    ASSERT(set_caught_set != NULL);
    ASSERT(set_other_set != NULL);
    ASSERT(set_invalidated_set != NULL);
    ASSERT(delete_other_set != NULL);
    ASSERT(delete_other_values_iter_set != NULL);
    ASSERT(delete_other_keys_iter_set != NULL);
    ASSERT(delete_other_entries_key_iter_set != NULL);
    ASSERT(delete_other_may_alias_key_set != NULL);
    ASSERT(delete_invalidated_set != NULL);
    ASSERT(clear_invalidated_set != NULL);
    ASSERT(kv_iter_set != NULL);
    ASSERT(kv_mixed_set != NULL);
    ASSERT(single_key_set != NULL);
    ASSERT(single_caught_key_set != NULL);
    ASSERT(kv_caught_key_set != NULL);
    ASSERT(kv_no_key_to_value_set != NULL);
    ASSERT(kv_no_value_to_key_set != NULL);
    ASSERT(kv_both_set != NULL);
    ASSERT(kv_both_mixed_key_set != NULL);
    ASSERT(keys_view_set != NULL);
    ASSERT(direct_keys_view_set != NULL);
    ASSERT(direct_entries_key_set != NULL);
    ASSERT(direct_entries_value_set != NULL);
    ASSERT(tuple_entries_key_set != NULL);
    ASSERT(tuple_entries_value_set != NULL);
    ASSERT(tuple_entries_no_key_to_value_set != NULL);
    ASSERT(tuple_entries_no_value_to_key_set != NULL);
    ASSERT(entries_key_set != NULL);
    ASSERT(entries_key_mixed_set != NULL);
    ASSERT(var_key_set != NULL);
    ASSERT(key_set_other_set != NULL);
    ASSERT(kv_key_set_other_set != NULL);
    ASSERT(key_set_caught_set != NULL);
    ASSERT(key_set_caught_mixed_set != NULL);
    ASSERT(kv_set_caught_set != NULL);
    ASSERT(kv_set_other_set != NULL);
    ASSERT(values_view_set_caught_other_set != NULL);
    ASSERT(value_set_caught_mixed_set != NULL);
    ASSERT(clear_values_set != NULL);
    ASSERT(clear_keys_set != NULL);
    ASSERT(clear_values_view_set != NULL);
    ASSERT(clear_keys_view_set != NULL);
    ASSERT(clear_entries_key_view_set != NULL);
    ASSERT(clear_entries_value_view_set != NULL);
    ASSERT(clear_then_set_value_set != NULL);
    ASSERT(clear_then_set_key_set != NULL);
    ASSERT(delete_singleton_then_set_value_set != NULL);
    ASSERT(delete_singleton_then_set_key_set != NULL);
    ASSERT(index_set_caught_value_set != NULL);
    ASSERT(index_set_other_value_set != NULL);
    ASSERT(index_set_caught_key_set != NULL);
    ASSERT(index_set_other_key_set != NULL);
    ASSERT(values_iter_set != NULL);
    ASSERT(direct_values_iter_set != NULL);
    ASSERT(values_mixed_set != NULL);
    ASSERT(entries_iter_set != NULL);
    ASSERT(entries_both_key_set != NULL);
    ASSERT(entries_both_value_set != NULL);
    ASSERT(entries_mixed_set != NULL);
    ASSERT(!literal_set->all_variants);
    ASSERT(xa_bitset_test(&literal_set->variants, 0));
    ASSERT(!xa_bitset_test(&literal_set->variants, 1));
    ASSERT(!other_key_set->all_variants);
    ASSERT(xa_bitset_test(&other_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&other_key_set->variants, 1));
    ASSERT(invalidated_set->all_variants);
    ASSERT(!dynamic_set->all_variants);
    ASSERT(xa_bitset_test(&dynamic_set->variants, 0));
    ASSERT(!xa_bitset_test(&dynamic_set->variants, 1));
    ASSERT(!stable_local_key_set->all_variants);
    ASSERT(xa_bitset_test(&stable_local_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&stable_local_key_set->variants, 1));
    ASSERT(dynamic_invalidated_set->all_variants);
    ASSERT(dynamic_method_set_set->all_variants);
    ASSERT(dynamic_delete_slot_set->all_variants);
    ASSERT(!dynamic_delete_values_set->all_variants);
    ASSERT(xa_bitset_test(&dynamic_delete_values_set->variants, 0));
    ASSERT(!xa_bitset_test(&dynamic_delete_values_set->variants, 1));
    ASSERT(mismatch_set->all_variants);
    ASSERT(!contains_key_set->all_variants);
    ASSERT(xa_bitset_test(&contains_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&contains_key_set->variants, 1));
    ASSERT(!readonly_views_set->all_variants);
    ASSERT(xa_bitset_test(&readonly_views_set->variants, 0));
    ASSERT(!xa_bitset_test(&readonly_views_set->variants, 1));
    ASSERT(!len_preserve_set->all_variants);
    ASSERT(xa_bitset_test(&len_preserve_set->variants, 0));
    ASSERT(!xa_bitset_test(&len_preserve_set->variants, 1));
    ASSERT(!set_caught_set->all_variants);
    ASSERT(xa_bitset_test(&set_caught_set->variants, 0));
    ASSERT(!xa_bitset_test(&set_caught_set->variants, 1));
    ASSERT(!set_other_set->all_variants);
    ASSERT(xa_bitset_test(&set_other_set->variants, 0));
    ASSERT(!xa_bitset_test(&set_other_set->variants, 1));
    ASSERT(set_invalidated_set->all_variants);
    ASSERT(!delete_other_set->all_variants);
    ASSERT(xa_bitset_test(&delete_other_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_other_set->variants, 1));
    ASSERT(!delete_other_values_iter_set->all_variants);
    ASSERT(xa_bitset_test(&delete_other_values_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_other_values_iter_set->variants, 1));
    ASSERT(!delete_other_keys_iter_set->all_variants);
    ASSERT(xa_bitset_test(&delete_other_keys_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_other_keys_iter_set->variants, 1));
    ASSERT(!delete_other_entries_key_iter_set->all_variants);
    ASSERT(xa_bitset_test(&delete_other_entries_key_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_other_entries_key_iter_set->variants, 1));
    ASSERT(delete_other_may_alias_key_set->all_variants);
    ASSERT(delete_invalidated_set->all_variants);
    ASSERT(clear_invalidated_set->all_variants);
    ASSERT(!kv_iter_set->all_variants);
    ASSERT(xa_bitset_test(&kv_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&kv_iter_set->variants, 1));
    ASSERT(kv_mixed_set->all_variants);
    ASSERT(single_key_set->all_variants);
    ASSERT(!single_caught_key_set->all_variants);
    ASSERT(xa_bitset_test(&single_caught_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&single_caught_key_set->variants, 1));
    ASSERT(!kv_caught_key_set->all_variants);
    ASSERT(xa_bitset_test(&kv_caught_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&kv_caught_key_set->variants, 1));
    ASSERT(kv_no_key_to_value_set->all_variants);
    ASSERT(kv_no_value_to_key_set->all_variants);
    ASSERT(!kv_both_set->all_variants);
    ASSERT(xa_bitset_test(&kv_both_set->variants, 0));
    ASSERT(!xa_bitset_test(&kv_both_set->variants, 1));
    ASSERT(kv_both_mixed_key_set->all_variants);
    ASSERT(!keys_view_set->all_variants);
    ASSERT(xa_bitset_test(&keys_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&keys_view_set->variants, 1));
    ASSERT(!direct_keys_view_set->all_variants);
    ASSERT(xa_bitset_test(&direct_keys_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&direct_keys_view_set->variants, 1));
    ASSERT(!direct_entries_key_set->all_variants);
    ASSERT(xa_bitset_test(&direct_entries_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&direct_entries_key_set->variants, 1));
    ASSERT(!direct_entries_value_set->all_variants);
    ASSERT(xa_bitset_test(&direct_entries_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&direct_entries_value_set->variants, 1));
    ASSERT(!tuple_entries_key_set->all_variants);
    ASSERT(xa_bitset_test(&tuple_entries_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&tuple_entries_key_set->variants, 1));
    ASSERT(!tuple_entries_value_set->all_variants);
    ASSERT(xa_bitset_test(&tuple_entries_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&tuple_entries_value_set->variants, 1));
    ASSERT(tuple_entries_no_key_to_value_set->all_variants);
    ASSERT(tuple_entries_no_value_to_key_set->all_variants);
    ASSERT(!entries_key_set->all_variants);
    ASSERT(xa_bitset_test(&entries_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&entries_key_set->variants, 1));
    ASSERT(entries_key_mixed_set->all_variants);
    ASSERT(!var_key_set->all_variants);
    ASSERT(xa_bitset_test(&var_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&var_key_set->variants, 1));
    ASSERT(key_set_other_set->all_variants);
    ASSERT(kv_key_set_other_set->all_variants);
    ASSERT(!key_set_caught_set->all_variants);
    ASSERT(xa_bitset_test(&key_set_caught_set->variants, 0));
    ASSERT(!xa_bitset_test(&key_set_caught_set->variants, 1));
    ASSERT(key_set_caught_mixed_set->all_variants);
    ASSERT(!kv_set_caught_set->all_variants);
    ASSERT(xa_bitset_test(&kv_set_caught_set->variants, 0));
    ASSERT(!xa_bitset_test(&kv_set_caught_set->variants, 1));
    ASSERT(kv_set_other_set->all_variants);
    ASSERT(!values_view_set_caught_other_set->all_variants);
    ASSERT(xa_bitset_test(&values_view_set_caught_other_set->variants, 0));
    ASSERT(!xa_bitset_test(&values_view_set_caught_other_set->variants, 1));
    ASSERT(value_set_caught_mixed_set->all_variants);
    ASSERT(!clear_values_set->all_variants);
    ASSERT(xa_bitset_test(&clear_values_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_values_set->variants, 1));
    ASSERT(!clear_keys_set->all_variants);
    ASSERT(xa_bitset_test(&clear_keys_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_keys_set->variants, 1));
    ASSERT(!clear_values_view_set->all_variants);
    ASSERT(xa_bitset_test(&clear_values_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_values_view_set->variants, 1));
    ASSERT(!clear_keys_view_set->all_variants);
    ASSERT(xa_bitset_test(&clear_keys_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_keys_view_set->variants, 1));
    ASSERT(!clear_entries_key_view_set->all_variants);
    ASSERT(xa_bitset_test(&clear_entries_key_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_entries_key_view_set->variants, 1));
    ASSERT(!clear_entries_value_view_set->all_variants);
    ASSERT(xa_bitset_test(&clear_entries_value_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_entries_value_view_set->variants, 1));
    ASSERT(!clear_then_set_value_set->all_variants);
    ASSERT(xa_bitset_test(&clear_then_set_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_then_set_value_set->variants, 1));
    ASSERT(!clear_then_set_key_set->all_variants);
    ASSERT(xa_bitset_test(&clear_then_set_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_then_set_key_set->variants, 1));
    ASSERT(!delete_singleton_then_set_value_set->all_variants);
    ASSERT(xa_bitset_test(&delete_singleton_then_set_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_singleton_then_set_value_set->variants, 1));
    ASSERT(!delete_singleton_then_set_key_set->all_variants);
    ASSERT(xa_bitset_test(&delete_singleton_then_set_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_singleton_then_set_key_set->variants, 1));
    ASSERT(!index_set_caught_value_set->all_variants);
    ASSERT(xa_bitset_test(&index_set_caught_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&index_set_caught_value_set->variants, 1));
    ASSERT(index_set_other_value_set->all_variants);
    ASSERT(!index_set_caught_key_set->all_variants);
    ASSERT(xa_bitset_test(&index_set_caught_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&index_set_caught_key_set->variants, 1));
    ASSERT(index_set_other_key_set->all_variants);
    ASSERT(!values_iter_set->all_variants);
    ASSERT(xa_bitset_test(&values_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&values_iter_set->variants, 1));
    ASSERT(!direct_values_iter_set->all_variants);
    ASSERT(xa_bitset_test(&direct_values_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&direct_values_iter_set->variants, 1));
    ASSERT(values_mixed_set->all_variants);
    ASSERT(!entries_iter_set->all_variants);
    ASSERT(xa_bitset_test(&entries_iter_set->variants, 0));
    ASSERT(!xa_bitset_test(&entries_iter_set->variants, 1));
    ASSERT(!entries_both_key_set->all_variants);
    ASSERT(xa_bitset_test(&entries_both_key_set->variants, 0));
    ASSERT(!xa_bitset_test(&entries_both_key_set->variants, 1));
    ASSERT(!entries_both_value_set->all_variants);
    ASSERT(xa_bitset_test(&entries_both_value_set->variants, 0));
    ASSERT(!xa_bitset_test(&entries_both_value_set->variants, 1));
    ASSERT(entries_mixed_set->all_variants);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_len_shadow_does_not_preserve_map_provenance) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum MapErr { Boom, Other }\n"
                         "fn failMap() { throw MapErr.Boom }\n"
                         "fn len(box: Map<string, MapErr>) -> int {\n"
                         "  box.clear()\n"
                         "  return 0\n"
                         "}\n"
                         "fn mapUserLenInvalidates() {\n"
                         "  try { failMap() } catch (e: MapErr) {\n"
                         "    var box = #{\"caught\": e}\n"
                         "    len(box)\n"
                         "    throw box[\"caught\"]\n"
                         "  }\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_map_len_shadow_invalidates.xr", program);

    const XaEffectSummary *summary = analyzer_function_effect_summary(a, "mapUserLenInvalidates");
    ASSERT(summary != NULL);
    const XaErrorTypeSet *set = effect_summary_enum_set_named(a, summary, "MapErr");
    ASSERT(set != NULL);
    ASSERT(set->all_variants);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_tracks_set_iterator_catch_aliases) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum SetErr { Boom, Other }\n"
                         "fn failSet() { throw SetErr.Boom }\n"
                         "fn setSingletonIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const box = #[e]\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDedupIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const alias = e\n"
                         "    const box = #[e, alias, e]\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setMixedIteratorFallsBack() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const box: Set<SetErr> = #[e, SetErr.Other]\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setMutatedIteratorFallsBack() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.add(SetErr.Other)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setAddCaughtIteratorPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const alias = e\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.add(alias)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setAddCaughtEmptyIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[]\n"
                         "    box.add(e)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setAddCaughtMixedIteratorFallsBack() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[SetErr.Other]\n"
                         "    box.add(e)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setContainsIteratorPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.contains(SetErr.Other)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setValuesIteratorPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.values()\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setLenIteratorPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    len(box)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setValuesViewIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const box: Set<SetErr> = #[e]\n"
                         "    for (item in box.values()) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setBoundValuesViewIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    const box: Set<SetErr> = #[e]\n"
                         "    const values = box.values()\n"
                         "    for (item in values) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDeleteIteratorPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.delete(SetErr.Other)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDeleteMatchingEnumMemberFallsBack() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.delete(SetErr.Boom)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDeleteOtherValuesViewPreserves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.delete(SetErr.Other)\n"
                         "    for (item in box.values()) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDeleteCaughtIteratorRemoves() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.delete(e)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setDeleteCaughtThenAddCaughtIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.delete(e)\n"
                         "    box.add(e)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setClearIteratorNoRethrow() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.clear()\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setClearValuesViewIteratorNoRethrow() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.clear()\n"
                         "    for (item in box.values()) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setClearThenAddCaughtIteratorRethrows() {\n"
                         "  try { failSet() } catch (e: SetErr) {\n"
                         "    var box: Set<SetErr> = #[e]\n"
                         "    box.clear()\n"
                         "    box.add(e)\n"
                         "    for (item in box) { throw item }\n"
                         "  }\n"
                         "}\n"
                         "fn setOrdinaryIteratorFallsBack() {\n"
                         "  const box: Set<SetErr> = #[SetErr.Boom, SetErr.Other]\n"
                         "  for (item in box) { throw item }\n"
                         "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_set_iterator_catch_alias.xr", program);

    const XaEffectSummary *singleton =
        analyzer_function_effect_summary(a, "setSingletonIteratorRethrows");
    const XaEffectSummary *dedup = analyzer_function_effect_summary(a, "setDedupIteratorRethrows");
    const XaEffectSummary *mixed = analyzer_function_effect_summary(a, "setMixedIteratorFallsBack");
    const XaEffectSummary *mutated =
        analyzer_function_effect_summary(a, "setMutatedIteratorFallsBack");
    const XaEffectSummary *add_caught =
        analyzer_function_effect_summary(a, "setAddCaughtIteratorPreserves");
    const XaEffectSummary *add_caught_empty =
        analyzer_function_effect_summary(a, "setAddCaughtEmptyIteratorRethrows");
    const XaEffectSummary *add_caught_mixed =
        analyzer_function_effect_summary(a, "setAddCaughtMixedIteratorFallsBack");
    const XaEffectSummary *contains =
        analyzer_function_effect_summary(a, "setContainsIteratorPreserves");
    const XaEffectSummary *values =
        analyzer_function_effect_summary(a, "setValuesIteratorPreserves");
    const XaEffectSummary *len_preserve =
        analyzer_function_effect_summary(a, "setLenIteratorPreserves");
    const XaEffectSummary *values_view =
        analyzer_function_effect_summary(a, "setValuesViewIteratorRethrows");
    const XaEffectSummary *bound_values_view =
        analyzer_function_effect_summary(a, "setBoundValuesViewIteratorRethrows");
    const XaEffectSummary *delete_preserves =
        analyzer_function_effect_summary(a, "setDeleteIteratorPreserves");
    const XaEffectSummary *delete_matching_enum =
        analyzer_function_effect_summary(a, "setDeleteMatchingEnumMemberFallsBack");
    const XaEffectSummary *delete_values_preserves =
        analyzer_function_effect_summary(a, "setDeleteOtherValuesViewPreserves");
    const XaEffectSummary *delete_caught =
        analyzer_function_effect_summary(a, "setDeleteCaughtIteratorRemoves");
    const XaEffectSummary *delete_caught_then_add =
        analyzer_function_effect_summary(a, "setDeleteCaughtThenAddCaughtIteratorRethrows");
    const XaEffectSummary *clear_no_rethrow =
        analyzer_function_effect_summary(a, "setClearIteratorNoRethrow");
    const XaEffectSummary *clear_values_view_no_rethrow =
        analyzer_function_effect_summary(a, "setClearValuesViewIteratorNoRethrow");
    const XaEffectSummary *clear_then_add =
        analyzer_function_effect_summary(a, "setClearThenAddCaughtIteratorRethrows");
    const XaEffectSummary *ordinary =
        analyzer_function_effect_summary(a, "setOrdinaryIteratorFallsBack");
    ASSERT(singleton != NULL);
    ASSERT(dedup != NULL);
    ASSERT(mixed != NULL);
    ASSERT(mutated != NULL);
    ASSERT(add_caught != NULL);
    ASSERT(add_caught_empty != NULL);
    ASSERT(add_caught_mixed != NULL);
    ASSERT(contains != NULL);
    ASSERT(values != NULL);
    ASSERT(values_view != NULL);
    ASSERT(bound_values_view != NULL);
    ASSERT(delete_preserves != NULL);
    ASSERT(delete_matching_enum != NULL);
    ASSERT(delete_values_preserves != NULL);
    ASSERT(delete_caught != NULL);
    ASSERT(delete_caught_then_add != NULL);
    ASSERT(clear_no_rethrow != NULL);
    ASSERT(clear_values_view_no_rethrow != NULL);
    ASSERT(clear_then_add != NULL);
    ASSERT(ordinary != NULL);

    const XaErrorTypeSet *singleton_set = effect_summary_enum_set_named(a, singleton, "SetErr");
    const XaErrorTypeSet *dedup_set = effect_summary_enum_set_named(a, dedup, "SetErr");
    const XaErrorTypeSet *mixed_set = effect_summary_enum_set_named(a, mixed, "SetErr");
    const XaErrorTypeSet *mutated_set = effect_summary_enum_set_named(a, mutated, "SetErr");
    const XaErrorTypeSet *add_caught_set = effect_summary_enum_set_named(a, add_caught, "SetErr");
    const XaErrorTypeSet *add_caught_empty_set =
        effect_summary_enum_set_named(a, add_caught_empty, "SetErr");
    const XaErrorTypeSet *add_caught_mixed_set =
        effect_summary_enum_set_named(a, add_caught_mixed, "SetErr");
    const XaErrorTypeSet *contains_set = effect_summary_enum_set_named(a, contains, "SetErr");
    const XaErrorTypeSet *values_set = effect_summary_enum_set_named(a, values, "SetErr");
    const XaErrorTypeSet *len_preserve_set =
        effect_summary_enum_set_named(a, len_preserve, "SetErr");
    const XaErrorTypeSet *values_view_set = effect_summary_enum_set_named(a, values_view, "SetErr");
    const XaErrorTypeSet *bound_values_view_set =
        effect_summary_enum_set_named(a, bound_values_view, "SetErr");
    const XaErrorTypeSet *delete_preserves_set =
        effect_summary_enum_set_named(a, delete_preserves, "SetErr");
    const XaErrorTypeSet *delete_matching_enum_set =
        effect_summary_enum_set_named(a, delete_matching_enum, "SetErr");
    const XaErrorTypeSet *delete_values_preserves_set =
        effect_summary_enum_set_named(a, delete_values_preserves, "SetErr");
    const XaErrorTypeSet *delete_caught_set =
        effect_summary_enum_set_named(a, delete_caught, "SetErr");
    const XaErrorTypeSet *delete_caught_then_add_set =
        effect_summary_enum_set_named(a, delete_caught_then_add, "SetErr");
    const XaErrorTypeSet *clear_no_rethrow_set =
        effect_summary_enum_set_named(a, clear_no_rethrow, "SetErr");
    const XaErrorTypeSet *clear_values_view_no_rethrow_set =
        effect_summary_enum_set_named(a, clear_values_view_no_rethrow, "SetErr");
    const XaErrorTypeSet *clear_then_add_set =
        effect_summary_enum_set_named(a, clear_then_add, "SetErr");
    const XaErrorTypeSet *ordinary_set = effect_summary_enum_set_named(a, ordinary, "SetErr");
    ASSERT(singleton_set != NULL);
    ASSERT(dedup_set != NULL);
    ASSERT(mixed_set != NULL);
    ASSERT(mutated_set != NULL);
    ASSERT(add_caught_set != NULL);
    ASSERT(add_caught_empty_set != NULL);
    ASSERT(add_caught_mixed_set != NULL);
    ASSERT(contains_set != NULL);
    ASSERT(values_set != NULL);
    ASSERT(len_preserve_set != NULL);
    ASSERT(values_view_set != NULL);
    ASSERT(bound_values_view_set != NULL);
    ASSERT(delete_preserves_set != NULL);
    ASSERT(delete_matching_enum_set != NULL);
    ASSERT(delete_values_preserves_set != NULL);
    ASSERT(delete_caught_then_add_set != NULL);
    ASSERT(clear_then_add_set != NULL);
    ASSERT(ordinary_set != NULL);
    ASSERT(!singleton_set->all_variants);
    ASSERT(xa_bitset_test(&singleton_set->variants, 0));
    ASSERT(!xa_bitset_test(&singleton_set->variants, 1));
    ASSERT(!dedup_set->all_variants);
    ASSERT(xa_bitset_test(&dedup_set->variants, 0));
    ASSERT(!xa_bitset_test(&dedup_set->variants, 1));
    ASSERT(mixed_set->all_variants);
    ASSERT(mutated_set->all_variants);
    ASSERT(!add_caught_set->all_variants);
    ASSERT(xa_bitset_test(&add_caught_set->variants, 0));
    ASSERT(!xa_bitset_test(&add_caught_set->variants, 1));
    ASSERT(!add_caught_empty_set->all_variants);
    ASSERT(xa_bitset_test(&add_caught_empty_set->variants, 0));
    ASSERT(!xa_bitset_test(&add_caught_empty_set->variants, 1));
    ASSERT(add_caught_mixed_set->all_variants);
    ASSERT(!contains_set->all_variants);
    ASSERT(xa_bitset_test(&contains_set->variants, 0));
    ASSERT(!xa_bitset_test(&contains_set->variants, 1));
    ASSERT(!values_set->all_variants);
    ASSERT(xa_bitset_test(&values_set->variants, 0));
    ASSERT(!xa_bitset_test(&values_set->variants, 1));
    ASSERT(!len_preserve_set->all_variants);
    ASSERT(xa_bitset_test(&len_preserve_set->variants, 0));
    ASSERT(!xa_bitset_test(&len_preserve_set->variants, 1));
    ASSERT(!values_view_set->all_variants);
    ASSERT(xa_bitset_test(&values_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&values_view_set->variants, 1));
    ASSERT(!bound_values_view_set->all_variants);
    ASSERT(xa_bitset_test(&bound_values_view_set->variants, 0));
    ASSERT(!xa_bitset_test(&bound_values_view_set->variants, 1));
    ASSERT(!delete_preserves_set->all_variants);
    ASSERT(xa_bitset_test(&delete_preserves_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_preserves_set->variants, 1));
    ASSERT(delete_matching_enum_set->all_variants);
    ASSERT(!delete_values_preserves_set->all_variants);
    ASSERT(xa_bitset_test(&delete_values_preserves_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_values_preserves_set->variants, 1));
    ASSERT(delete_caught_set == NULL);
    ASSERT(!delete_caught_then_add_set->all_variants);
    ASSERT(xa_bitset_test(&delete_caught_then_add_set->variants, 0));
    ASSERT(!xa_bitset_test(&delete_caught_then_add_set->variants, 1));
    ASSERT(clear_no_rethrow_set == NULL);
    ASSERT(clear_values_view_no_rethrow_set == NULL);
    ASSERT(!clear_then_add_set->all_variants);
    ASSERT(xa_bitset_test(&clear_then_add_set->variants, 0));
    ASSERT(!xa_bitset_test(&clear_then_add_set->variants, 1));
    ASSERT(ordinary_set->all_variants);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_error_effect_converges_recursive_components) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "enum RecursiveErr { Boom, Other }\n"
                         "fn directRecursive(flag: bool) {\n"
                         "  if (flag) { throw RecursiveErr.Boom }\n"
                         "  directRecursive(false)\n"
                         "}\n"
                         "fn cycleA() { cycleB() }\n"
                         "fn cycleB() { cycleC() }\n"
                         "fn cycleC() { cycleA(); throw RecursiveErr.Boom }\n"
                         "fn caughtCycleA() { try { caughtCycleB() } catch RecursiveErr { } }\n"
                         "fn caughtCycleB() { caughtCycleA(); throw RecursiveErr.Boom }\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "effect_recursive_fixpoint.xr", program);

    const XaEffectSummary *direct = analyzer_function_effect_summary(a, "directRecursive");
    const XaEffectSummary *cycle_a = analyzer_function_effect_summary(a, "cycleA");
    const XaEffectSummary *cycle_b = analyzer_function_effect_summary(a, "cycleB");
    const XaEffectSummary *cycle_c = analyzer_function_effect_summary(a, "cycleC");
    const XaEffectSummary *caught_a = analyzer_function_effect_summary(a, "caughtCycleA");
    const XaEffectSummary *caught_b = analyzer_function_effect_summary(a, "caughtCycleB");
    ASSERT(direct != NULL);
    ASSERT(cycle_a != NULL);
    ASSERT(cycle_b != NULL);
    ASSERT(cycle_c != NULL);
    ASSERT(caught_a != NULL);
    ASSERT(caught_b != NULL);
    ASSERT(direct->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(cycle_a->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(cycle_b->error_set_completeness == XA_EFFECT_COMPLETE);
    ASSERT(cycle_c->error_set_completeness == XA_EFFECT_COMPLETE);
    const XaErrorTypeSet *direct_set = effect_summary_enum_set_named(a, direct, "RecursiveErr");
    const XaErrorTypeSet *cycle_a_set = effect_summary_enum_set_named(a, cycle_a, "RecursiveErr");
    const XaErrorTypeSet *cycle_b_set = effect_summary_enum_set_named(a, cycle_b, "RecursiveErr");
    const XaErrorTypeSet *cycle_c_set = effect_summary_enum_set_named(a, cycle_c, "RecursiveErr");
    ASSERT(direct_set != NULL);
    ASSERT(cycle_a_set != NULL);
    ASSERT(cycle_b_set != NULL);
    ASSERT(cycle_c_set != NULL);
    ASSERT(!direct_set->all_variants);
    ASSERT(!cycle_a_set->all_variants);
    ASSERT(!cycle_b_set->all_variants);
    ASSERT(!cycle_c_set->all_variants);
    ASSERT(xa_bitset_test(&direct_set->variants, 0));
    ASSERT(xa_bitset_test(&cycle_a_set->variants, 0));
    ASSERT(xa_bitset_test(&cycle_b_set->variants, 0));
    ASSERT(xa_bitset_test(&cycle_c_set->variants, 0));
    ASSERT(xa_effect_summary_is_nothrow(caught_a));
    const XaErrorTypeSet *caught_b_set = effect_summary_enum_set_named(a, caught_b, "RecursiveErr");
    ASSERT(caught_b_set != NULL);
    ASSERT(!caught_b_set->all_variants);
    ASSERT(xa_bitset_test(&caught_b_set->variants, 0));

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

TEST(analyzer_struct_literal_unknown_fields_offer_lambda_return_hint) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source = "struct Point { x: int; y: int }\n"
                         "var f: (int) -> Point = value -> Point{result: value}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "lambda_struct_literal_hint.xr", program);

    ASSERT(analyzer_diag_contains(a, "type 'Point' has no field 'result'"));
    ASSERT(analyzer_diag_contains(
        a, "if you meant a return-type annotation: arrow lambdas have none"));
    ASSERT(analyzer_diag_contains(a, "use `fn(...) -> Point { ... }`"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_unconstrained_expression_lambda_reports_e0365_with_all_routes) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var f = value -> value\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "lambda_inference_routes.xr", program);

    int count = 0;
    XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(a, &count);
    bool found_e0365 = false;
    for (XaDiagnostic *diag = diagnostics; diag; diag = diag->next) {
        if (diag->code == XR_ERR_ANALYZE_MISSING_TYPE) {
            found_e0365 = true;
            break;
        }
    }
    ASSERT(found_e0365);
    ASSERT(analyzer_diag_contains(a, "annotate this lambda parameter"));
    ASSERT(analyzer_diag_contains(a, "annotate the binding"));
    ASSERT(analyzer_diag_contains(a, "rely on the call-site signature"));
    ASSERT(analyzer_diag_contains(a, "use `fn(x: T) -> R { ... }`"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_ref_arrow_hint_is_advisory_and_fail_closed) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    const char *source =
        "fn mutate(value: ref int) { value *= 2 }\n"
        "var readOnly = (x: ref int) -> x * 2\n"
        "var direct = (x: ref int) -> { x = x * 2 }\n"
        "var compound = (x: ref int) -> { x *= 2 }\n"
        "var increment = (x: ref int) -> { x++ }\n"
        "var delegated = (x: ref int) -> { mutate(ref x) }\n"
        "var indexed = (xs: ref Array<int>) -> { xs[0] = 1 }\n"
        "var receiver = (xs: ref Array<int>) -> { xs.push(1) }\n"
        "var required: (ref int) -> int = (x: ref int) -> x * 2\n"
        "var inferred: (ref int) -> int = x -> x * 2\n"
        "var uncertain = (x: ref int, callback: (ref int) -> ()) -> {\n"
        "    callback(ref x)\n"
        "}\n";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "lambda_ref_hint.xr", program);

    int diagnostic_count = 0;
    int hint_count = 0;
    int error_count = 0;
    XaDiagnostic *diagnostics = xa_analyzer_get_diagnostics(a, &diagnostic_count);
    for (XaDiagnostic *diag = diagnostics; diag; diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            error_count++;
        if (diag->severity == XR_DIAG_SEV_HINT && diag->message &&
            strstr(diag->message, "ref parameter 'x' is never mutated"))
            hint_count++;
    }
    ASSERT(error_count == 0);
    ASSERT(hint_count == 1);
    ASSERT(analyzer_diag_contains(a, "use a read parameter"));
    ASSERT(!analyzer_diag_contains(a, "ref parameter 'xs' is never mutated"));

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

TEST(analyzer_rejects_generator_yield_value_mismatch) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "generator_yield_value_mismatch.xr",
                                          &scope));

    XrTypeRef *iter_args[] = {xr_tref_int(g_session)};
    XrType *iter_type =
        xr_tref_resolve_in_analyzer(a, xr_tref_generic(g_session, "Iterator", iter_args, 1));
    ASSERT(iter_type != NULL);
    ASSERT(iter_type->kind == XR_KIND_INTERFACE || iter_type->kind == XR_KIND_INSTANCE);
    ASSERT(iter_type->instance.type_arg_count == 1);
    ASSERT(iter_type->instance.type_args != NULL);
    ASSERT(iter_type->instance.type_args[0] != NULL);
    ASSERT(XR_TYPE_IS_INT(iter_type->instance.type_args[0]));
    XrType *iter_elem = NULL;
    ASSERT(xa_analyzer_is_iterable(a, iter_type, &iter_elem));
    ASSERT(iter_elem != NULL);
    ASSERT(XR_TYPE_IS_INT(iter_elem));
    ASSERT(!xa_typecheck_assignable(iter_elem, xr_type_new_string(NULL)));

    const char *source = "fn bad() -> Iterator<int> { yield \"x\" }";
    AstNode *program = xr_parse(g_session, source);
    ASSERT(program != NULL);

    xa_analyzer_analyze(a, "generator_yield_value_mismatch.xr", program);

    ASSERT(analyzer_diag_contains(
        a, "yielded value of type 'string' is not assignable to generator element type 'int'"));

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

TEST(analyzer_type_ref_failures_use_error_recovery) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    XrArena arena;
    xr_arena_init(&arena, XR_ARENA_SEGMENT_SIZE);
    XrCompilerSessionScope scope;
    ASSERT(xr_compiler_session_push_arena(g_session, &arena, "type_ref_error_recovery.xr", &scope));

    XrType *missing =
        xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "DefinitelyMissingType"));
    ASSERT(missing != NULL);
    ASSERT(XR_TYPE_IS_ERROR(missing));

    XrType *removed = xr_tref_resolve_in_analyzer(a, xr_tref_named(g_session, "EnumValue"));
    ASSERT(removed != NULL);
    ASSERT(XR_TYPE_IS_ERROR(removed));

    XrTypeRef *missing_args[] = {xr_tref_int(g_session)};
    XrType *missing_generic = xr_tref_resolve_in_analyzer(
        a, xr_tref_generic(g_session, "DefinitelyMissingGeneric", missing_args, 1));
    ASSERT(missing_generic != NULL);
    ASSERT(XR_TYPE_IS_ERROR(missing_generic));

    XrTypeRef *invalid_fixed = xr_tref_fixed_array_expr(g_session, xr_tref_int(g_session), NULL, 0);
    XrType *fixed = xr_tref_resolve_in_analyzer(a, invalid_fixed);
    ASSERT(fixed != NULL);
    ASSERT(XR_TYPE_IS_ERROR(fixed));

    ASSERT(analyzer_diag_contains(a, "undefined type 'DefinitelyMissingType'"));
    ASSERT(analyzer_diag_contains(a, "runtime enum wrapper type 'EnumValue' has been removed"));
    ASSERT(analyzer_diag_contains(a, "undefined type 'DefinitelyMissingGeneric'"));
    ASSERT(analyzer_diag_contains(a, "fixed array length must be greater than zero"));

    xr_compiler_session_pop_arena(&scope);
    xr_arena_destroy(&arena);
    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_cast_error_recovery_and_union_overlap) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var value = 1 as MissingCastType\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "cast_error_recovery.xr", program);

    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 1);
    ASSERT(analyzer_diag_contains(a, "undefined type 'MissingCastType'"));
    ASSERT(!analyzer_diag_contains(a, "Cannot cast type"));

    xa_analyzer_free(a);
    setup_pool();

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "var value: string | bool = \"text\"\n"
                                  "var result = value as int\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "cast_disjoint_union.xr", program);

    count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 1);
    ASSERT(analyzer_diag_contains(a, "Cannot cast type"));
    ASSERT(analyzer_diag_contains(a, "to unrelated type 'int'"));

    xa_analyzer_free(a);
    setup_pool();

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "var value: int | string = 1\n"
                                  "var result = value as string\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "cast_overlapping_union.xr", program);

    count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_enum_identity_is_nominal) {
    // Two distinct enums are separate nominal types. Neither an `as` cast nor an
    // assignment may bridge unrelated enums; only same-name enums are equal.
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "enum Color { Red, Green, Blue }\n"
                                           "enum Suit { Hearts, Spades }\n"
                                           "var value = Color.Red as Suit\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "enum_disjoint_cast.xr", program);
    ASSERT(analyzer_diag_contains(a, "Cannot cast type 'Color' to unrelated type 'Suit'"));

    xa_analyzer_free(a);
    setup_pool();

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "enum Color { Red, Green, Blue }\n"
                                  "enum Suit { Hearts, Spades }\n"
                                  "var value: Suit = Color.Red\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "enum_disjoint_assign.xr", program);
    ASSERT(analyzer_diag_contains(a, "is not assignable to type 'Suit'"));

    xa_analyzer_free(a);
    setup_pool();

    // Same enum stays assignable, and enum -> int conversion remains valid.
    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "enum Color { Red, Green, Blue }\n"
                                  "var c: Color = Color.Red\n"
                                  "var same: Color = c\n"
                                  "var n = c as int\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "enum_same_ok.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_assignment_error_recovery_suppresses_cascade) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var value: MissingAssignmentType = 1\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "assignment_error_recovery.xr", program);

    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 1);
    ASSERT(analyzer_diag_contains(a, "undefined type 'MissingAssignmentType'"));
    ASSERT(!analyzer_diag_contains(a, "not assignable"));

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_member_error_recovery_suppresses_call_cascade) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "\"hello\".missing();\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "member_error_recovery.xr", program);

    ASSERT(analyzer_diag_contains(a, "string has no member 'missing'"));
    ASSERT(!analyzer_diag_contains(a, "Value is not callable"));
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_operator_and_index_failures_use_error_recovery) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var op = \"text\" - 1\n"
                                           "op.missing()\n"
                                           "var indexed = \"text\"[0]\n"
                                           "indexed.missing()\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "operator_index_error_recovery.xr", program);

    ASSERT(analyzer_diag_contains(a, "operator '-' is not defined"));
    ASSERT(analyzer_diag_contains(a, "string does not support integer indexing"));
    ASSERT(!analyzer_diag_contains(a, "has no member 'missing'"));
    ASSERT(!analyzer_diag_contains(a, "Value is not callable"));
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count >= 2);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_nullable_numeric_equality_uses_nonnull_literal_context) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "fn value() -> int? { return 7 }\n"
                                           "var a = value() == 7\n"
                                           "var b = -7 == value()\n"
                                           "var c = value() != null\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "nullable_numeric_equality.xr", program);

    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_non_callable_failure_uses_error_recovery) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var called = (1)()\ncalled.missing()\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "non_callable_error_recovery.xr", program);

    ASSERT(analyzer_diag_contains(a, "Value is not callable"));
    ASSERT(!analyzer_diag_contains(a, "has no member 'missing'"));
    ASSERT(a->unresolved_inference_count == 0);
    ASSERT(a->recovery_poison_type_count >= 1);

    xa_analyzer_free(a);
    setup_pool();
}

TEST(analyzer_container_recovery_rejects_poisoned_success_types) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var xs = [missingElement]\n"
                                           "xs.missing()\n"
                                           "var raw = Array()\n"
                                           "raw.missing()\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "container_error_recovery.xr", program);

    ASSERT(analyzer_diag_contains(a, "Undeclared variable 'missingElement'"));
    ASSERT(
        analyzer_diag_contains(a, "cannot infer type arguments for generic constructor 'Array'"));
    ASSERT(!analyzer_diag_contains(a, "Array has no member 'missing'"));
    ASSERT(!analyzer_diag_contains(a, "Value is not callable"));
    ASSERT(a->unresolved_inference_count == 1);
    ASSERT(a->recovery_poison_type_count >= 2);

    xa_analyzer_free(a);
    setup_pool();

    a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);
    program = xr_parse(g_session, "var typed = Array<int>()\n"
                                  "var value: int = typed[0]\n"
                                  "var contextualArray: Array<int> = Array()\n"
                                  "var contextualMap: Map<string, int> = Map()\n"
                                  "var contextualSet: Set<string> = Set()\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "typed_container_constructor.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count == 0);
    ASSERT(a->unresolved_inference_count == 0);

    xa_analyzer_free(a);
    setup_pool();
}

/* WeakMap and WeakSet were removed. The names must now be unknown
 * types, not silently-accepted ones — a deleted surface that still parses is
 * worse than one that never existed. */
TEST(analyzer_weak_containers_are_unknown_types) {
    XaAnalyzer *a = xa_analyzer_new(g_session);
    ASSERT(a != NULL);

    AstNode *program = xr_parse(g_session, "var wm = WeakMap<string, int>()\n"
                                           "var ws = WeakSet<string>()\n");
    ASSERT(program != NULL);
    xa_analyzer_analyze(a, "weak_containers_removed.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(a, &count);
    ASSERT(count > 0);

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
    box->links.owns_class_info = true;
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

TEST(type_substitute_preserves_function_param_modes) {
    XrType *params[] = {xr_type_new_type_param(g_isolate, "T", 0),
                        xr_type_new_type_param(g_isolate, "U", 1),
                        xr_type_new_type_param(g_isolate, "V", 2)};
    XrType *fn = xr_type_new_function(g_isolate, params, 3, xr_type_new_unit(NULL), false);
    ASSERT(fn != NULL);
    ASSERT(xr_type_function_set_param_mode(fn, 0, XR_PARAM_READ));
    ASSERT(xr_type_function_set_param_mode(fn, 1, XR_PARAM_REF));
    ASSERT(xr_type_function_set_param_mode(fn, 2, XR_PARAM_MOVE));

    const char *names[] = {"T", "U", "V"};
    XrType *actuals[] = {xr_type_new_int(NULL), xr_type_new_string(NULL), xr_type_new_bool(NULL)};
    XrType *subst = xr_type_substitute(g_isolate, fn, names, actuals, 3);

    ASSERT(subst != NULL);
    ASSERT(subst != fn);
    ASSERT(XR_TYPE_IS_FUNCTION(subst));
    ASSERT(XR_TYPE_IS_INT(xr_type_function_param_type(subst, 0)));
    ASSERT(XR_TYPE_IS_STRING(xr_type_function_param_type(subst, 1)));
    ASSERT(XR_TYPE_IS_BOOL(xr_type_function_param_type(subst, 2)));
    ASSERT(xr_type_function_param_mode(subst, 0) == XR_PARAM_READ);
    ASSERT(xr_type_function_param_mode(subst, 1) == XR_PARAM_REF);
    ASSERT(xr_type_function_param_mode(subst, 2) == XR_PARAM_MOVE);
}

// ============================================================================
// Edge case tests
// ============================================================================

TEST(type_null_handling) {
    // Operations on NULL should not crash
    ASSERT(strcmp(xr_type_to_string(NULL), "<error>") == 0);
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
    ASSERT(mod == NULL);

    ASSERT(xa_builtin_get_module_func_signature("http", "parseRequest") == NULL);
    ASSERT(xa_builtin_get_module_func_signature("http", "sendResponse") == NULL);
    ASSERT(xa_builtin_get_module_func_signature("http", "setConnHandler") == NULL);
    ASSERT(xa_builtin_get_module_func_signature("http", "__getConnHandler") == NULL);
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
    RUN_TEST(type_scalar_alias_identity);
    RUN_TEST(type_const_capability_is_part_of_identity_and_format);
    RUN_TEST(type_nullable_qualifier_does_not_mutate_declaration_identity);
    RUN_TEST(type_containers);
    RUN_TEST(type_union);
    RUN_TEST(type_error_recovery);
    RUN_TEST(type_assignable);
    RUN_TEST(record_nullable_field_accepts_explicit_null);
    RUN_TEST(typecheck_assignable_rejects_unknown_source);
    RUN_TEST(typecheck_assignable_rejects_unknown_container_member);
    RUN_TEST(analyzer_check_assignment_rejects_unknown_source);
    RUN_TEST(type_to_string);
    RUN_TEST(type_narrowing);

    printf("\nSymbol tests:\n");
    RUN_TEST(symbol_create);
    RUN_TEST(scope_basic);
    RUN_TEST(scope_lookup);
    RUN_TEST(scope_owns_replaced_symbols_until_teardown);

    printf("\nAnalyzer tests:\n");
    RUN_TEST(analyzer_create);
    RUN_TEST(analyzer_diagnostics);
    RUN_TEST(analyzer_type_telemetry_splits_unknown_and_error);
    RUN_TEST(analyzer_scope_management);
    RUN_TEST(analyzer_inferred_unique_alias_nll_guards_move);
    RUN_TEST(analyzer_parameter_effect_is_canonical_product);
    RUN_TEST(analyzer_memory_effect_infers_and_instantiates_root_relative_facts);
    RUN_TEST(analyzer_mem_scalar_access_is_stable_for_pointer_owner_borrows);
    RUN_TEST(analyzer_codegen_controls_are_semantic_neutral_and_type_closed);
    RUN_TEST(symbol_export_metadata_reinterns_analyzer_local_sidecars);
    RUN_TEST(analyzer_slice_mutator_effect_is_independent_of_discarded_result);
    RUN_TEST(analyzer_canonical_effect_product_publishes_suspend_fixpoint);
    RUN_TEST(analyzer_generator_suspend_is_separate_from_scheduler_suspend);
    RUN_TEST(analyzer_allocation_effect_propagates_and_validates_contracts);
    RUN_TEST(analyzer_throw_effect_bit_matches_effect_summary);
    RUN_TEST(analyzer_inferred_effects_accept_function_values);
    RUN_TEST(analyzer_call_context_accepts_u64_only_literals);
    RUN_TEST(analyzer_effect_inference_handles_redundant_try_catch);
    RUN_TEST(analyzer_deprecated_message_reaches_use_diagnostic);
    RUN_TEST(analyzer_stored_function_value_defaults_may_throw);
    RUN_TEST(analyzer_generic_hof_splits_throw_effect_dimension);
    RUN_TEST(analyzer_error_effect_records_direct_throw_variant);
    RUN_TEST(analyzer_error_effect_propagates_const_function_value_aliases);
    RUN_TEST(analyzer_error_effect_propagates_stable_var_function_values);
    RUN_TEST(analyzer_error_effect_propagates_generic_specialization_target_sets);
    RUN_TEST(analyzer_error_effect_propagates_immediate_function_expr_calls);
    RUN_TEST(cycle_candidate_marks_every_field_shape);
    RUN_TEST(cycle_candidate_weak_field_breaks_the_edge);
    RUN_TEST(cycle_candidate_weak_self_reference_breaks_the_edge);
    RUN_TEST(cycle_candidate_follows_inherited_fields);
    RUN_TEST(cycle_candidate_marks_recursive_tree_types);
    RUN_TEST(analyzer_error_effect_handles_recursive_function_expr_cycles);
    RUN_TEST(analyzer_error_effect_propagates_direct_method_calls);
    RUN_TEST(analyzer_error_effect_propagates_module_export_calls);
    RUN_TEST(analyzer_xrd_signatures_fail_closed_without_typed_contracts);
    RUN_TEST(analyzer_xrd_native_typed_byte_contracts_reject_legacy_aliases);
    RUN_TEST(analyzer_xrd_handle_fields_reject_legacy_byte_aliases);
    RUN_TEST(analyzer_xrd_namespace_reports_invalid_descriptor);
    RUN_TEST(analyzer_error_effect_consumes_builtin_type_member_contracts);
    RUN_TEST(analyzer_error_effect_subtracts_typed_catches);
    RUN_TEST(analyzer_error_effect_marks_invalid_program_partial_facts);
    RUN_TEST(analyzer_error_effect_tracks_map_catch_aliases);
    RUN_TEST(analyzer_error_effect_len_shadow_does_not_preserve_map_provenance);
    RUN_TEST(analyzer_error_effect_tracks_set_iterator_catch_aliases);
    RUN_TEST(analyzer_error_effect_converges_recursive_components);

    printf("\nFlow analysis tests:\n");
    RUN_TEST(flow_builder_create);
    RUN_TEST(flow_basic_graph);
    RUN_TEST(flow_condition_branches);
    RUN_TEST(flow_binding_use_join_and_reassignment);
    RUN_TEST(flow_cache);
    RUN_TEST(narrow_by_typeof);
    RUN_TEST(narrow_by_null);

    printf("\nAdditional type tests:\n");
    RUN_TEST(type_class_instance);
    RUN_TEST(type_function_complex);
    RUN_TEST(type_function_throw_effect_covariance);
    RUN_TEST(type_void_never);
    RUN_TEST(type_rejects_invalid_counts);
    RUN_TEST(type_function_copy_preserves_metadata);
    RUN_TEST(type_string_parser_uses_error_recovery_for_invalid_types);

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
    RUN_TEST(analyzer_struct_literal_unknown_fields_offer_lambda_return_hint);
    RUN_TEST(analyzer_unconstrained_expression_lambda_reports_e0365_with_all_routes);
    RUN_TEST(analyzer_ref_arrow_hint_is_advisory_and_fail_closed);
    RUN_TEST(analyzer_rejects_builtin_generic_arity);
    RUN_TEST(analyzer_rejects_generator_yield_value_mismatch);
    RUN_TEST(analyzer_rejects_error_type_container_success_types);
    RUN_TEST(analyzer_type_ref_failures_use_error_recovery);
    RUN_TEST(analyzer_cast_error_recovery_and_union_overlap);
    RUN_TEST(analyzer_enum_identity_is_nominal);
    RUN_TEST(analyzer_assignment_error_recovery_suppresses_cascade);
    RUN_TEST(analyzer_member_error_recovery_suppresses_call_cascade);
    RUN_TEST(analyzer_operator_and_index_failures_use_error_recovery);
    RUN_TEST(analyzer_nullable_numeric_equality_uses_nonnull_literal_context);
    RUN_TEST(analyzer_non_callable_failure_uses_error_recovery);
    RUN_TEST(analyzer_container_recovery_rejects_poisoned_success_types);
    RUN_TEST(analyzer_weak_containers_are_unknown_types);
    RUN_TEST(analyzer_rejects_error_type_generic_argument_and_constraint);
    RUN_TEST(export_symbols_invalidate_table_on_nested_error_type);
    RUN_TEST(compile_type_class);
    RUN_TEST(compile_type_optional);
    RUN_TEST(type_substitute_preserves_nullable_type_param);
    RUN_TEST(type_substitute_preserves_function_param_modes);

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
