/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mono.c - Unit tests for monomorphization infrastructure
 */

#include "../test_framework.h"
#include "../../../src/frontend/analyzer/xanalyzer_mono.h"
#include "../../../src/frontend/analyzer/xanalyzer_capability.h"
#include "../../../src/frontend/parser/xtype_ref.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

/* ========== Name Mangling Tests ========== */

TEST(mono_type_tag_basic) {
    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef float_t = {.kind = XR_TREF_FLOAT};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef bool_t = {.kind = XR_TREF_BOOL};
    XrTypeRef error_t = {.kind = XR_TREF_ERROR};

    ASSERT_STR_EQ(xr_mono_type_tag(&int_t), "i64");
    ASSERT_STR_EQ(xr_mono_type_tag(&float_t), "f64");
    ASSERT_STR_EQ(xr_mono_type_tag(&str_t), "str");
    ASSERT_STR_EQ(xr_mono_type_tag(&bool_t), "bool");
    ASSERT_STR_EQ(xr_mono_type_tag(&error_t), "err");
    ASSERT_STR_EQ(xr_mono_type_tag(NULL), "unknown");
}

TEST(mono_scalar_tags_are_semantic_and_unique) {
    struct {
        uint8_t kind;
        uint8_t scalar_rep;
        const char *tag;
    } cases[] = {
        {XR_TREF_INT_WIDTH, XR_NATIVE_I8, "i8"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_U8, "u8"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_I16, "i16"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_U16, "u16"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_I32, "i32"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_U32, "u32"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_I64, "i64"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_U64, "u64"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_ISIZE, "isize"},
        {XR_TREF_INT_WIDTH, XR_NATIVE_USIZE, "usize"},
        {XR_TREF_FLOAT_WIDTH, XR_NATIVE_F32, "f32"},
        {XR_TREF_FLOAT_WIDTH, XR_NATIVE_F64, "f64"},
    };
    XrTypeRef refs[sizeof(cases) / sizeof(cases[0])];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        refs[i] = (XrTypeRef) {.kind = cases[i].kind, .scalar_rep = cases[i].scalar_rep};
        ASSERT_STR_EQ(xr_mono_type_tag(&refs[i]), cases[i].tag);
        for (size_t j = 0; j < i; j++)
            ASSERT(strcmp(xr_mono_type_tag(&refs[i]), xr_mono_type_tag(&refs[j])) != 0);
    }

    XrTypeRef int_default = {.kind = XR_TREF_INT, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef float_default = {.kind = XR_TREF_FLOAT, .scalar_rep = XR_NATIVE_F64};
    ASSERT_STR_EQ(xr_mono_type_tag(&int_default), xr_mono_type_tag(&refs[6]));
    ASSERT_STR_EQ(xr_mono_type_tag(&float_default), xr_mono_type_tag(&refs[11]));
}

TEST(mono_mangle_single) {
    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef *args[] = {&int_t};
    char *result = xr_mono_mangle("identity", args, 1);
    ASSERT_STR_EQ(result, "identity$i64");
    free(result);
}

TEST(mono_mangle_multi) {
    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *args[] = {&int_t, &str_t};
    char *result = xr_mono_mangle("map", args, 2);
    ASSERT_STR_EQ(result, "map$i64_str");
    free(result);
}

TEST(mono_mangle_preserves_const_capability_identity) {
    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *array_children[] = {&int_t};
    XrTypeRef array_t = {
        .kind = XR_TREF_GENERIC, .name = "Array", .nchildren = 1, .children = array_children};
    XrTypeRef *map_children[] = {&str_t, &int_t};
    XrTypeRef map_t = {
        .kind = XR_TREF_GENERIC, .name = "Map", .nchildren = 2, .children = map_children};
    XrTypeRef *const_array_children[] = {&array_t};
    XrTypeRef const_array = {
        .kind = XR_TREF_CONST, .nchildren = 1, .children = const_array_children};
    XrTypeRef *const_map_children[] = {&map_t};
    XrTypeRef const_map = {.kind = XR_TREF_CONST, .nchildren = 1, .children = const_map_children};

    XrTypeRef *array_args[] = {&const_array};
    XrTypeRef *map_args[] = {&const_map};
    char *array_name = xr_mono_mangle("hold", array_args, 1);
    char *map_name = xr_mono_mangle("hold", map_args, 1);
    ASSERT_STR_EQ(array_name, "hold$const_Array_i64");
    ASSERT_STR_EQ(map_name, "hold$const_Map_str_i64");
    ASSERT(strcmp(array_name, map_name) != 0);
    free(array_name);
    free(map_name);
}

TEST(monomorphized_stdlib_type_preserves_sealed_capabilities) {
    const uint32_t expected = XA_TYPE_CAP_INTERIOR_MUTABLE | XA_TYPE_CAP_SYNC_SHAREABLE;
    ASSERT_EQ(xa_stdlib_type_capability_flags("sync", "Mutex"), expected);
    ASSERT_EQ(xa_stdlib_type_capability_flags("sync", "Mutex$i64"), expected);
    ASSERT_EQ(xa_stdlib_type_capability_flags("sync", "RwLock$str"), expected);
    ASSERT_EQ(xa_stdlib_type_capability_flags("user_sync", "Mutex$i64"), XA_TYPE_CAP_NONE);
    ASSERT_EQ(xa_stdlib_type_capability_flags("sync", "MutexImpostor$i64"), XA_TYPE_CAP_NONE);
}

TEST(mono_mangle_null_name) {
    char *result = xr_mono_mangle(NULL, NULL, 0);
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "");
    free(result);
}

TEST(mono_mangle_zero_args) {
    char *result = xr_mono_mangle("foo", NULL, 0);
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "foo");
    free(result);
}

/* ========== Type Substitution Tests ========== */

TEST(type_substitute_type_param) {
    XrTypeRef param_t = {.kind = XR_TREF_TYPE_PARAM};
    param_t.name = "T";

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&param_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_TREF_INT);
}

TEST(type_substitute_no_match) {
    XrTypeRef param_t = {.kind = XR_TREF_TYPE_PARAM};
    param_t.name = "U";

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&param_t, map, 1);
    // No match, returns original
    ASSERT(result == &param_t);
}

TEST(type_substitute_non_param) {
    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef concrete = {.kind = XR_TREF_FLOAT};
    XrMonoTypeMap map[] = {{"T", &concrete}};

    XrTypeRef *result = xr_mono_type_substitute(&int_t, map, 1);
    // Non-param type is unchanged
    ASSERT(result == &int_t);
}

TEST(type_substitute_array_element) {
    // Array<T> where T=int ?Array<int>
    XrTypeRef param_t = {.kind = XR_TREF_TYPE_PARAM};
    param_t.name = "T";

    XrTypeRef *elem_child = &param_t;
    XrTypeRef array_t = {
        .kind = XR_TREF_NAMED, .name = "Array", .nchildren = 1, .children = &elem_child};

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&array_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_TREF_NAMED);
    ASSERT(result->nchildren == 1 && result->children != NULL);
    ASSERT_EQ(result->children[0]->kind, XR_TREF_INT);
    // Should be a new type (not the original)
    ASSERT(result != &array_t);
    free(result);
}

TEST(type_substitute_null_safe) {
    XrTypeRef *result = xr_mono_type_substitute(NULL, NULL, 0);
    ASSERT(result == NULL);
}

/* ========== AST Clone Tests ========== */

TEST(ast_clone_null) {
    AstNode *result = xr_ast_clone(NULL, NULL, 0);
    ASSERT(result == NULL);
}

TEST(ast_clone_literal_int) {
    AstNode node = {.type = AST_LITERAL_INT, .line = 42, .column = 5};
    node.as.literal.kind = LITERAL_KIND_INT;
    node.as.literal.raw_value.int_val = 123;

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT(clone != &node);  // Must be a different allocation
    ASSERT_EQ(clone->type, AST_LITERAL_INT);
    ASSERT_EQ(clone->line, 42);
    ASSERT_EQ(clone->column, 5);
    ASSERT_EQ(clone->as.literal.raw_value.int_val, 123);
    free(clone);
}

TEST(ast_clone_literal_string) {
    AstNode node = {.type = AST_LITERAL_STRING, .line = 1};
    node.as.literal.kind = LITERAL_KIND_STRING;
    node.as.literal.raw_value.string_val = "hello";

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT_STR_EQ(clone->as.literal.raw_value.string_val, "hello");
    // String must be a separate copy
    ASSERT(clone->as.literal.raw_value.string_val != node.as.literal.raw_value.string_val);
    free((void *) clone->as.literal.raw_value.string_val);
    free(clone);
}

TEST(ast_clone_binary) {
    AstNode left = {.type = AST_LITERAL_INT, .line = 1};
    left.as.literal.raw_value.int_val = 10;
    AstNode right = {.type = AST_LITERAL_INT, .line = 1};
    right.as.literal.raw_value.int_val = 20;

    AstNode add = {.type = AST_BINARY_ADD, .line = 1};
    add.as.binary.left = &left;
    add.as.binary.right = &right;

    AstNode *clone = xr_ast_clone(&add, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT_EQ(clone->type, AST_BINARY_ADD);
    ASSERT(clone->as.binary.left != NULL);
    ASSERT(clone->as.binary.right != NULL);
    ASSERT(clone->as.binary.left != &left);  // Deep copy
    ASSERT(clone->as.binary.right != &right);
    ASSERT_EQ(clone->as.binary.left->as.literal.raw_value.int_val, 10);
    ASSERT_EQ(clone->as.binary.right->as.literal.raw_value.int_val, 20);
    free(clone->as.binary.left);
    free(clone->as.binary.right);
    free(clone);
}

TEST(ast_clone_variable) {
    AstNode node = {.type = AST_VARIABLE, .line = 5};
    node.as.variable.name = "x";

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT_STR_EQ(clone->as.variable.name, "x");
    ASSERT(clone->as.variable.name != node.as.variable.name);  // Deep copy
    free(clone->as.variable.name);
    free(clone);
}

TEST(ast_clone_with_type_substitution) {
    // AstNode no longer carries an inline compile_type
    // field; mono substitutes types only through legitimate per-node
    // type fields (param types, var-decl annotations, return types, ...).
    // This test now exercises that path via VarDeclNode::type_annotation.
    // type_annotation is now XrTypeRef*; a NAMED ref matching the type param
    // name will be substituted by the mono clone.
    XrTypeRef param_tref = {.kind = XR_TREF_NAMED, .name = "T"};

    AstNode node = {.type = AST_VAR_DECL, .line = 1};
    node.as.var_decl.name = "result";
    node.as.var_decl.initializer = NULL;
    node.as.var_decl.is_const = false;
    node.as.var_decl.type_annotation = &param_tref;

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    AstNode *clone = xr_ast_clone(&node, map, 1);
    ASSERT(clone != NULL);
    ASSERT(clone->as.var_decl.type_annotation != NULL);
    ASSERT_EQ(clone->as.var_decl.type_annotation->kind, XR_TREF_INT);
    free(clone->as.var_decl.name);
    free(clone);
}

/* ========== Mono Collector Tests ========== */

TEST(mono_collector_basic) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);
    ASSERT_EQ(c.count, 0);

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef *args[] = {&int_t};
    const char *name = xa_mono_collector_add(&c, "identity", args, 1, false);
    ASSERT(name != NULL);
    ASSERT_STR_EQ(name, "identity$i64");
    ASSERT_EQ(c.count, 1);

    xa_mono_collector_free(&c);
}

TEST(mono_collector_dedup) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);

    XrTypeRef int_t = {.kind = XR_TREF_INT};
    XrTypeRef *args1[] = {&int_t};
    xa_mono_collector_add(&c, "identity", args1, 1, false);

    // bool has different slot type from int (BOOL=11 vs I64=7) ?separate instance
    XrTypeRef bool_t = {.kind = XR_TREF_BOOL};
    XrTypeRef *args2[] = {&bool_t};
    xa_mono_collector_add(&c, "identity", args2, 1, false);
    ASSERT_EQ(c.count, 2);

    // Same int type again ?should deduplicate
    XrTypeRef int_t2 = {.kind = XR_TREF_INT};
    XrTypeRef *args2b[] = {&int_t2};
    xa_mono_collector_add(&c, "identity", args2b, 1, false);
    ASSERT_EQ(c.count, 2);

    // float has different rep ?separate instance
    XrTypeRef float_t = {.kind = XR_TREF_FLOAT};
    XrTypeRef *args3[] = {&float_t};
    xa_mono_collector_add(&c, "identity", args3, 1, false);
    ASSERT_EQ(c.count, 3);

    xa_mono_collector_free(&c);
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("Name Mangling");
    RUN_TEST(mono_type_tag_basic);
    RUN_TEST(mono_scalar_tags_are_semantic_and_unique);
    RUN_TEST(mono_mangle_single);
    RUN_TEST(mono_mangle_multi);
    RUN_TEST(mono_mangle_preserves_const_capability_identity);
    RUN_TEST(monomorphized_stdlib_type_preserves_sealed_capabilities);
    RUN_TEST(mono_mangle_null_name);
    RUN_TEST(mono_mangle_zero_args);

    RUN_TEST_SUITE("Type Substitution");
    RUN_TEST(type_substitute_type_param);
    RUN_TEST(type_substitute_no_match);
    RUN_TEST(type_substitute_non_param);
    RUN_TEST(type_substitute_array_element);
    RUN_TEST(type_substitute_null_safe);

    RUN_TEST_SUITE("AST Clone");
    RUN_TEST(ast_clone_null);
    RUN_TEST(ast_clone_literal_int);
    RUN_TEST(ast_clone_literal_string);
    RUN_TEST(ast_clone_binary);
    RUN_TEST(ast_clone_variable);
    RUN_TEST(ast_clone_with_type_substitution);

    RUN_TEST_SUITE("Mono Collector");
    RUN_TEST(mono_collector_basic);
    RUN_TEST(mono_collector_dedup);

    TEST_REPORT();
    return xr_tests_failed > 0 ? 1 : 0;
}
