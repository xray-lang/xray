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
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

/* ========== Name Mangling Tests ========== */

TEST(mono_type_tag_basic) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType float_t = { .kind = XR_KIND_FLOAT };
    XrType str_t = { .kind = XR_KIND_STRING };
    XrType bool_t = { .kind = XR_KIND_BOOL };
    XrType unknown_t = { .kind = XR_KIND_UNKNOWN };

    ASSERT_STR_EQ(xr_mono_type_tag(&int_t), "i64");
    ASSERT_STR_EQ(xr_mono_type_tag(&float_t), "f64");
    ASSERT_STR_EQ(xr_mono_type_tag(&str_t), "str");
    ASSERT_STR_EQ(xr_mono_type_tag(&bool_t), "bool");
    ASSERT_STR_EQ(xr_mono_type_tag(&unknown_t), "any");
    ASSERT_STR_EQ(xr_mono_type_tag(NULL), "unknown");
}

TEST(mono_mangle_single) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType *args[] = { &int_t };
    char *result = xr_mono_mangle("identity", args, 1);
    ASSERT_STR_EQ(result, "identity$i64");
    free(result);
}

TEST(mono_mangle_multi) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType str_t = { .kind = XR_KIND_STRING };
    XrType *args[] = { &int_t, &str_t };
    char *result = xr_mono_mangle("map", args, 2);
    ASSERT_STR_EQ(result, "map$i64_str");
    free(result);
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
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "T";
    param_t.type_param.id = 0;

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    XrType *result = xr_mono_type_substitute(&param_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_KIND_INT);
}

TEST(type_substitute_no_match) {
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "U";

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    XrType *result = xr_mono_type_substitute(&param_t, map, 1);
    // No match, returns original
    ASSERT(result == &param_t);
}

TEST(type_substitute_non_param) {
    XrType int_t = { .kind = XR_KIND_INT };
    XrType concrete = { .kind = XR_KIND_FLOAT };
    XrMonoTypeMap map[] = { { "T", &concrete } };

    XrType *result = xr_mono_type_substitute(&int_t, map, 1);
    // Non-param type is unchanged
    ASSERT(result == &int_t);
}

TEST(type_substitute_array_element) {
    // Array<T> where T=int → Array<int>
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "T";

    XrType array_t = { .kind = XR_KIND_ARRAY };
    array_t.container.element_type = &param_t;

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    XrType *result = xr_mono_type_substitute(&array_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_KIND_ARRAY);
    ASSERT(result->container.element_type != NULL);
    ASSERT_EQ(result->container.element_type->kind, XR_KIND_INT);
    // Should be a new type (not the original)
    ASSERT(result != &array_t);
    free(result);
}

TEST(type_substitute_null_safe) {
    XrType *result = xr_mono_type_substitute(NULL, NULL, 0);
    ASSERT(result == NULL);
}

/* ========== AST Clone Tests ========== */

TEST(ast_clone_null) {
    AstNode *result = xr_ast_clone(NULL, NULL, 0);
    ASSERT(result == NULL);
}

TEST(ast_clone_literal_int) {
    AstNode node = { .type = AST_LITERAL_INT, .line = 42, .column = 5 };
    node.as.literal.kind = LITERAL_KIND_INT;
    node.as.literal.raw_value.int_val = 123;

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT(clone != &node); // Must be a different allocation
    ASSERT_EQ(clone->type, AST_LITERAL_INT);
    ASSERT_EQ(clone->line, 42);
    ASSERT_EQ(clone->column, 5);
    ASSERT_EQ(clone->as.literal.raw_value.int_val, 123);
    free(clone);
}

TEST(ast_clone_literal_string) {
    AstNode node = { .type = AST_LITERAL_STRING, .line = 1 };
    node.as.literal.kind = LITERAL_KIND_STRING;
    node.as.literal.raw_value.string_val = "hello";

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT_STR_EQ(clone->as.literal.raw_value.string_val, "hello");
    // String must be a separate copy
    ASSERT(clone->as.literal.raw_value.string_val != node.as.literal.raw_value.string_val);
    free((void *)clone->as.literal.raw_value.string_val);
    free(clone);
}

TEST(ast_clone_binary) {
    AstNode left = { .type = AST_LITERAL_INT, .line = 1 };
    left.as.literal.raw_value.int_val = 10;
    AstNode right = { .type = AST_LITERAL_INT, .line = 1 };
    right.as.literal.raw_value.int_val = 20;

    AstNode add = { .type = AST_BINARY_ADD, .line = 1 };
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
    AstNode node = { .type = AST_VARIABLE, .line = 5 };
    node.as.variable.name = "x";

    AstNode *clone = xr_ast_clone(&node, NULL, 0);
    ASSERT(clone != NULL);
    ASSERT_STR_EQ(clone->as.variable.name, "x");
    ASSERT(clone->as.variable.name != node.as.variable.name); // Deep copy
    free(clone->as.variable.name);
    free(clone);
}

TEST(ast_clone_with_type_substitution) {
    // Variable with compile_type = TYPE_PARAM "T"
    XrType param_t = { .kind = XR_KIND_TYPE_PARAM };
    param_t.type_param.name = "T";

    AstNode node = { .type = AST_VARIABLE, .line = 1 };
    node.as.variable.name = "result";
    node.compile_type_legacy = &param_t;

    XrType int_t = { .kind = XR_KIND_INT };
    XrMonoTypeMap map[] = { { "T", &int_t } };

    AstNode *clone = xr_ast_clone(&node, map, 1);
    ASSERT(clone != NULL);
    ASSERT(clone->compile_type_legacy != NULL);
    ASSERT_EQ(clone->compile_type_legacy->kind, XR_KIND_INT);
    free(clone->as.variable.name);
    free(clone);
}

/* ========== Mono Collector Tests ========== */

TEST(mono_collector_basic) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);
    ASSERT_EQ(c.count, 0);

    XrType int_t = { .kind = XR_KIND_INT };
    XrType *args[] = { &int_t };
    const char *name = xa_mono_collector_add(&c, "identity", args, 1);
    ASSERT(name != NULL);
    ASSERT_STR_EQ(name, "identity$i64");
    ASSERT_EQ(c.count, 1);

    xa_mono_collector_free(&c);
}

TEST(mono_collector_dedup) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);

    XrType int_t = { .kind = XR_KIND_INT };
    XrType *args1[] = { &int_t };
    xa_mono_collector_add(&c, "identity", args1, 1);

    // bool has different slot type from int (BOOL=11 vs I64=7) → separate instance
    XrType bool_t = { .kind = XR_KIND_BOOL };
    XrType *args2[] = { &bool_t };
    xa_mono_collector_add(&c, "identity", args2, 1);
    ASSERT_EQ(c.count, 2);

    // Same int type again → should deduplicate
    XrType int_t2 = { .kind = XR_KIND_INT };
    XrType *args2b[] = { &int_t2 };
    xa_mono_collector_add(&c, "identity", args2b, 1);
    ASSERT_EQ(c.count, 2);

    // float has different rep → separate instance
    XrType float_t = { .kind = XR_KIND_FLOAT };
    XrType *args3[] = { &float_t };
    xa_mono_collector_add(&c, "identity", args3, 1);
    ASSERT_EQ(c.count, 3);

    xa_mono_collector_free(&c);
}

/* ========== Main ========== */

int main(void) {
    RUN_TEST_SUITE("Name Mangling");
    RUN_TEST(mono_type_tag_basic);
    RUN_TEST(mono_mangle_single);
    RUN_TEST(mono_mangle_multi);
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
