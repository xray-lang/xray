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
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xanalyzer_capability.h"
#include "../../../src/frontend/parser/xtype_ref.h"
#include "../../../src/runtime/class/xclass_info.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/toolchain/xcompiler_session.h"

/* ========== Name Mangling Tests ========== */

TEST(mono_type_tag_basic) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef float_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_F64};
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
        {XR_TREF_SCALAR, XR_NATIVE_I8, "i8"},       {XR_TREF_SCALAR, XR_NATIVE_U8, "u8"},
        {XR_TREF_SCALAR, XR_NATIVE_I16, "i16"},     {XR_TREF_SCALAR, XR_NATIVE_U16, "u16"},
        {XR_TREF_SCALAR, XR_NATIVE_I32, "i32"},     {XR_TREF_SCALAR, XR_NATIVE_U32, "u32"},
        {XR_TREF_SCALAR, XR_NATIVE_I64, "i64"},     {XR_TREF_SCALAR, XR_NATIVE_U64, "u64"},
        {XR_TREF_SCALAR, XR_NATIVE_ISIZE, "isize"}, {XR_TREF_SCALAR, XR_NATIVE_USIZE, "usize"},
        {XR_TREF_SCALAR, XR_NATIVE_F32, "f32"},     {XR_TREF_SCALAR, XR_NATIVE_F64, "f64"},
    };
    XrTypeRef refs[sizeof(cases) / sizeof(cases[0])];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        refs[i] = (XrTypeRef) {.kind = cases[i].kind, .scalar_rep = cases[i].scalar_rep};
        ASSERT_STR_EQ(xr_mono_type_tag(&refs[i]), cases[i].tag);
        for (size_t j = 0; j < i; j++)
            ASSERT(strcmp(xr_mono_type_tag(&refs[i]), xr_mono_type_tag(&refs[j])) != 0);
    }

    XrTypeRef int_default = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef float_default = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_F64};
    ASSERT_STR_EQ(xr_mono_type_tag(&int_default), xr_mono_type_tag(&refs[6]));
    ASSERT_STR_EQ(xr_mono_type_tag(&float_default), xr_mono_type_tag(&refs[11]));
}

TEST(mono_mangle_single) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *args[] = {&int_t};
    char *result = xr_mono_mangle("identity", args, 1);
    ASSERT_STR_EQ(result, "identity$i64");
    free(result);
}

TEST(mono_mangle_multi) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *args[] = {&int_t, &str_t};
    char *result = xr_mono_mangle("map", args, 2);
    ASSERT_STR_EQ(result, "map$i64_str");
    free(result);
}

TEST(mono_mangle_uses_exact_nominal_declaration_identity) {
    XrCompilerSession *session = xr_compiler_session_new(NULL);
    ASSERT_NOT_NULL(session);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    ASSERT_NOT_NULL(analyzer);

    XrClassInfo first_info = {.name = "LocalCounter", .declaration_key = UINT64_C(0x100001111)};
    XrClassInfo second_info = {.name = "LocalCounter", .declaration_key = UINT64_C(0x200001111)};
    XrType *first_type = xr_type_new_instance(analyzer->isolate, &first_info);
    XrType *same_first_type = xr_type_new_instance(analyzer->isolate, &first_info);
    XrType *second_type = xr_type_new_instance(analyzer->isolate, &second_info);
    ASSERT_NOT_NULL(first_type);
    ASSERT_NOT_NULL(same_first_type);
    ASSERT_NOT_NULL(second_type);
    same_first_type->is_value_type = true;

    XrTypeRef first_ref = {.kind = XR_TREF_NAMED, .name = "LocalCounter"};
    XrTypeRef same_first_ref = {.kind = XR_TREF_NAMED, .name = "LocalCounter"};
    XrTypeRef second_ref = {.kind = XR_TREF_NAMED, .name = "LocalCounter"};
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &first_ref, first_type));
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &same_first_ref, same_first_type));
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &second_ref, second_type));

    XrTypeRef *first_args[] = {&first_ref};
    XrTypeRef *same_first_args[] = {&same_first_ref};
    XrTypeRef *second_args[] = {&second_ref};
    char *first = xr_mono_mangle_in_analyzer(analyzer, "read", first_args, 1);
    char *same_first = xr_mono_mangle_in_analyzer(analyzer, "read", same_first_args, 1);
    char *second = xr_mono_mangle_in_analyzer(analyzer, "read", second_args, 1);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(same_first);
    ASSERT_NOT_NULL(second);
    ASSERT_STR_EQ(first, same_first);
    ASSERT_TRUE(strcmp(first, second) != 0);
    ASSERT_NOT_NULL(strstr(first, "read$LocalCounter$x"));

    XrTypeRef param_ref = {.kind = XR_TREF_TYPE_PARAM, .name = "T"};
    XrTypeRef *array_children[] = {&param_ref};
    XrTypeRef array_ref = {
        .kind = XR_TREF_GENERIC, .name = "Array", .nchildren = 1, .children = array_children};
    XrType *param_type = xr_type_new_type_param(analyzer->isolate, "T", 0);
    XrType *array_type = xr_type_new_array(analyzer->isolate, param_type);
    ASSERT_NOT_NULL(param_type);
    ASSERT_NOT_NULL(array_type);
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &param_ref, param_type));
    ASSERT_TRUE(xa_analyzer_bind_type_ref_type(analyzer, &array_ref, array_type));

    XrMonoTypeMap first_map[] = {{.param_name = "T",
                                  .concrete_type = &first_ref,
                                  .concrete_semantic_type = first_type}};
    XrMonoTypeMap second_map[] = {{.param_name = "T",
                                   .concrete_type = &second_ref,
                                   .concrete_semantic_type = second_type}};
    XrTypeRef *first_array =
        xr_mono_type_substitute_in_analyzer(analyzer, &array_ref, first_map, 1);
    XrTypeRef *second_array =
        xr_mono_type_substitute_in_analyzer(analyzer, &array_ref, second_map, 1);
    ASSERT_NOT_NULL(first_array);
    ASSERT_NOT_NULL(second_array);
    XrType *first_array_type = xa_analyzer_get_type_ref_type(analyzer, first_array);
    XrType *second_array_type = xa_analyzer_get_type_ref_type(analyzer, second_array);
    ASSERT_NOT_NULL(first_array_type);
    ASSERT_NOT_NULL(second_array_type);
    ASSERT_EQ_PTR(first_array_type->container.element_type->instance.class_ref, &first_info);
    ASSERT_EQ_PTR(second_array_type->container.element_type->instance.class_ref, &second_info);
    XrTypeRef *first_array_args[] = {first_array};
    XrTypeRef *second_array_args[] = {second_array};
    char *first_nested = xr_mono_mangle_in_analyzer(analyzer, "nested", first_array_args, 1);
    char *second_nested = xr_mono_mangle_in_analyzer(analyzer, "nested", second_array_args, 1);
    ASSERT_NOT_NULL(first_nested);
    ASSERT_NOT_NULL(second_nested);
    ASSERT_TRUE(strcmp(first_nested, second_nested) != 0);

    first_info.xg_nominal_key = UINT64_MAX;
    first_info.xg_decl_id = UINT32_MAX;
    char *after_evidence = xr_mono_mangle_in_analyzer(analyzer, "read", first_args, 1);
    ASSERT_NOT_NULL(after_evidence);
    ASSERT_STR_EQ(first, after_evidence);
    xr_free(after_evidence);
    XaSymbol declaration = {0};
    first_info.declaration_symbol = &declaration;
    first_info.nominal_kind = XA_NOMINAL_STRUCT;
    first_info.declaration_key = 0u;
    ASSERT_NULL(xr_mono_mangle_in_analyzer(analyzer, "read", first_args, 1));

    first_ref.name = "MutatedCounter";
    ASSERT_NULL(xa_analyzer_get_type_ref_type(analyzer, &first_ref));

    free(first);
    free(same_first);
    free(second);
    free(first_nested);
    free(second_nested);
    free(first_array->children);
    free(first_array);
    free(second_array->children);
    free(second_array);
    xa_analyzer_free(analyzer);
    xr_compiler_session_delete(session);
}

TEST(mono_mangle_preserves_const_capability_identity) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
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

TEST(mono_mangle_distinguishes_container_element_types) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *int_children[] = {&int_t};
    XrTypeRef *str_children[] = {&str_t};
    XrTypeRef int_array = {
        .kind = XR_TREF_GENERIC, .name = "Array", .nchildren = 1, .children = int_children};
    XrTypeRef str_array = {
        .kind = XR_TREF_GENERIC, .name = "Array", .nchildren = 1, .children = str_children};
    XrTypeRef int_set = {
        .kind = XR_TREF_GENERIC, .name = "Set", .nchildren = 1, .children = int_children};

    XrTypeRef *int_array_args[] = {&int_array};
    XrTypeRef *str_array_args[] = {&str_array};
    XrTypeRef *int_set_args[] = {&int_set};
    char *int_array_name = xr_mono_mangle("tagOf", int_array_args, 1);
    char *str_array_name = xr_mono_mangle("tagOf", str_array_args, 1);
    char *int_set_name = xr_mono_mangle("tagOf", int_set_args, 1);
    /* Array<int> and Array<string> must not share an instance: the clone the
     * first call site produces types its parameters against int. */
    ASSERT_STR_EQ(int_array_name, "tagOf$Array_i64");
    ASSERT_STR_EQ(str_array_name, "tagOf$Array_str");
    ASSERT_STR_EQ(int_set_name, "tagOf$Set_i64");
    ASSERT(strcmp(int_array_name, str_array_name) != 0);
    ASSERT(strcmp(int_array_name, int_set_name) != 0);
    free(int_array_name);
    free(str_array_name);
    free(int_set_name);
}

TEST(mono_mangle_distinguishes_tuple_and_optional_shapes) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *pair_children[] = {&int_t, &int_t};
    XrTypeRef *swap_children[] = {&int_t, &str_t};
    XrTypeRef *one_children[] = {&int_t};
    XrTypeRef pair = {.kind = XR_TREF_TUPLE, .nchildren = 2, .children = pair_children};
    XrTypeRef swap = {.kind = XR_TREF_TUPLE, .nchildren = 2, .children = swap_children};
    XrTypeRef single = {.kind = XR_TREF_TUPLE, .nchildren = 1, .children = one_children};
    XrTypeRef opt_int = {.kind = XR_TREF_OPTIONAL, .nchildren = 1, .children = one_children};

    XrTypeRef *pair_args[] = {&pair};
    XrTypeRef *swap_args[] = {&swap};
    XrTypeRef *single_args[] = {&single};
    XrTypeRef *opt_args[] = {&opt_int};
    char *pair_name = xr_mono_mangle("tagOf", pair_args, 1);
    char *swap_name = xr_mono_mangle("tagOf", swap_args, 1);
    char *single_name = xr_mono_mangle("tagOf", single_args, 1);
    char *opt_name = xr_mono_mangle("tagOf", opt_args, 1);
    /* Arity is part of the tag: without it a 1-tuple of int and a 2-tuple whose
     * element tags concatenate the same way would share an instance. */
    ASSERT_STR_EQ(pair_name, "tagOf$tup2_i64_i64");
    ASSERT_STR_EQ(swap_name, "tagOf$tup2_i64_str");
    ASSERT_STR_EQ(single_name, "tagOf$tup1_i64");
    ASSERT_STR_EQ(opt_name, "tagOf$opt_i64");
    ASSERT(strcmp(pair_name, swap_name) != 0);
    ASSERT(strcmp(pair_name, single_name) != 0);
    ASSERT(strcmp(opt_name, single_name) != 0);
    free(pair_name);
    free(swap_name);
    free(single_name);
    free(opt_name);
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

    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&param_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_TREF_SCALAR);
}

TEST(type_substitute_no_match) {
    XrTypeRef param_t = {.kind = XR_TREF_TYPE_PARAM};
    param_t.name = "U";

    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&param_t, map, 1);
    // No match, returns original
    ASSERT(result == &param_t);
}

TEST(type_substitute_non_param) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef concrete = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_F64};
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

    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    XrTypeRef *result = xr_mono_type_substitute(&array_t, map, 1);
    ASSERT(result != NULL);
    ASSERT_EQ(result->kind, XR_TREF_NAMED);
    ASSERT(result->nchildren == 1 && result->children != NULL);
    ASSERT_EQ(result->children[0]->kind, XR_TREF_SCALAR);
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

    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrMonoTypeMap map[] = {{"T", &int_t}};

    AstNode *clone = xr_ast_clone(&node, map, 1);
    ASSERT(clone != NULL);
    ASSERT(clone->as.var_decl.type_annotation != NULL);
    ASSERT_EQ(clone->as.var_decl.type_annotation->kind, XR_TREF_SCALAR);
    free(clone->as.var_decl.name);
    free(clone);
}

TEST(ast_clone_named_enum_record_nodes) {
    AstNode variant = {.type = AST_ENUM_ACCESS, .line = 1};
    variant.as.enum_access.enum_name = "PairPayload";
    variant.as.enum_access.member_name = "Pair";
    AstNode value = {.type = AST_LITERAL_INT, .line = 1};
    value.as.literal.kind = LITERAL_KIND_INT;
    value.as.literal.raw_value.int_val = 7;
    char *construct_names[] = {"left"};
    AstNode *construct_values[] = {&value};
    AstNode construct = {.type = AST_ENUM_CONSTRUCT, .line = 1};
    construct.as.enum_construct.variant_path = &variant;
    construct.as.enum_construct.field_names = construct_names;
    construct.as.enum_construct.field_values = construct_values;
    construct.as.enum_construct.field_count = 1;

    AstNode *construct_clone = xr_ast_clone(&construct, NULL, 0);
    ASSERT(construct_clone != NULL && construct_clone->type == AST_ENUM_CONSTRUCT);
    ASSERT(construct_clone->as.enum_construct.variant_path != &variant);
    ASSERT(construct_clone->as.enum_construct.field_names != construct_names);
    ASSERT(construct_clone->as.enum_construct.field_names[0] != construct_names[0]);
    ASSERT_STR_EQ(construct_clone->as.enum_construct.field_names[0], "left");
    ASSERT(construct_clone->as.enum_construct.field_values != construct_values);
    ASSERT(construct_clone->as.enum_construct.field_values[0] != &value);

    AstNode wildcard = {.type = AST_PATTERN_WILDCARD, .line = 2};
    char *pattern_names[] = {"right"};
    AstNode *patterns[] = {&wildcard};
    AstNode pattern = {.type = AST_PATTERN_ADT, .line = 2};
    pattern.as.pattern_adt.variant = &variant;
    pattern.as.pattern_adt.field_names = pattern_names;
    pattern.as.pattern_adt.patterns = patterns;
    pattern.as.pattern_adt.count = 1;

    AstNode *pattern_clone = xr_ast_clone(&pattern, NULL, 0);
    ASSERT(pattern_clone != NULL && pattern_clone->type == AST_PATTERN_ADT);
    ASSERT(pattern_clone->as.pattern_adt.variant != &variant);
    ASSERT(pattern_clone->as.pattern_adt.field_names != pattern_names);
    ASSERT(pattern_clone->as.pattern_adt.field_names[0] != pattern_names[0]);
    ASSERT_STR_EQ(pattern_clone->as.pattern_adt.field_names[0], "right");
    ASSERT(pattern_clone->as.pattern_adt.patterns != patterns);
    ASSERT(pattern_clone->as.pattern_adt.patterns[0] != &wildcard);

    AstNode method = {.type = AST_METHOD_DECL, .line = 3};
    method.as.method_decl.name = "inspect";
    method.as.method_decl.receiver_mode = XR_PARAM_MOVE;
    method.as.method_decl.is_override = true;
    AstNode *method_clone = xr_ast_clone(&method, NULL, 0);
    ASSERT(method_clone != NULL && method_clone->type == AST_METHOD_DECL);
    ASSERT(method_clone->as.method_decl.receiver_mode == XR_PARAM_MOVE);
    ASSERT(method_clone->as.method_decl.is_override);

    free(construct_clone->as.enum_construct.variant_path->as.enum_access.enum_name);
    free(construct_clone->as.enum_construct.variant_path->as.enum_access.member_name);
    free(construct_clone->as.enum_construct.variant_path);
    free(construct_clone->as.enum_construct.field_names[0]);
    free(construct_clone->as.enum_construct.field_names);
    free(construct_clone->as.enum_construct.field_values[0]);
    free(construct_clone->as.enum_construct.field_values);
    free(construct_clone);
    free(pattern_clone->as.pattern_adt.variant->as.enum_access.enum_name);
    free(pattern_clone->as.pattern_adt.variant->as.enum_access.member_name);
    free(pattern_clone->as.pattern_adt.variant);
    free(pattern_clone->as.pattern_adt.field_names[0]);
    free(pattern_clone->as.pattern_adt.field_names);
    free(pattern_clone->as.pattern_adt.patterns[0]);
    free(pattern_clone->as.pattern_adt.patterns);
    free(pattern_clone);
    free(method_clone->as.method_decl.name);
    free(method_clone);
}

/* ========== Mono Collector Tests ========== */

TEST(mono_collector_basic) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);
    ASSERT_EQ(c.count, 0);

    AstNode declaration = {.type = AST_FUNCTION_DECL};
    declaration.as.function_decl.type_param_count = 1;
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *args[] = {&int_t};
    XaGenericSpecializationFact identity = {
        .generic_decl = &declaration,
        .declaration_type_args = args,
        .declaration_type_arg_count = 1u,
    };
    const char *name = xa_mono_collector_add(&c, "identity", &identity, NULL);
    ASSERT(name != NULL);
    ASSERT_STR_EQ(name, "identity$i64");
    ASSERT_EQ(c.count, 1);

    xa_mono_collector_free(&c);
}

TEST(mono_collector_dedup) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);

    AstNode declaration = {.type = AST_FUNCTION_DECL};
    declaration.as.function_decl.type_param_count = 1;
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *args1[] = {&int_t};
    XaGenericSpecializationFact identity = {
        .generic_decl = &declaration,
        .declaration_type_args = args1,
        .declaration_type_arg_count = 1u,
    };
    xa_mono_collector_add(&c, "identity", &identity, NULL);

    // bool has different slot type from int (BOOL=11 vs I64=7) ?separate instance
    XrTypeRef bool_t = {.kind = XR_TREF_BOOL};
    XrTypeRef *args2[] = {&bool_t};
    identity.declaration_type_args = args2;
    xa_mono_collector_add(&c, "identity", &identity, NULL);
    ASSERT_EQ(c.count, 2);

    // Same int type again ?should deduplicate
    XrTypeRef int_t2 = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *args2b[] = {&int_t2};
    identity.declaration_type_args = args2b;
    xa_mono_collector_add(&c, "identity", &identity, NULL);
    ASSERT_EQ(c.count, 2);

    // float has different rep ?separate instance
    XrTypeRef float_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_F64};
    XrTypeRef *args3[] = {&float_t};
    identity.declaration_type_args = args3;
    xa_mono_collector_add(&c, "identity", &identity, NULL);
    ASSERT_EQ(c.count, 3);

    xa_mono_collector_free(&c);
}

TEST(mono_collector_uses_exact_method_owner_and_effect_identity) {
    XaMonoCollector c;
    xa_mono_collector_init(&c);

    XrTypeRef callback_type = {.kind = XR_TREF_FUNCTION};
    XrParamNode callback_param = {.type = &callback_type};
    XrParamNode *params[1] = {&callback_param};
    AstNode first_method = {.type = AST_METHOD_DECL};
    first_method.as.method_decl.type_param_count = 1;
    first_method.as.method_decl.params = params;
    first_method.as.method_decl.param_count = 1;
    AstNode *first_methods[1] = {&first_method};
    AstNode first_owner = {.type = AST_STRUCT_DECL};
    first_owner.as.struct_decl.methods = first_methods;
    first_owner.as.struct_decl.method_count = 1;

    XrTypeRef int_type = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef equivalent_int_type = {.kind = XR_TREF_SCALAR,
                                     .scalar_rep = XR_NATIVE_I64};
    XrTypeRef *method_args[1] = {&int_type};
    XaGenericSpecializationFact identity = {
        .generic_decl = &first_method,
        .owner_decl = &first_owner,
        .declaration_type_args = method_args,
        .declaration_type_arg_count = 1u,
        .effect = XA_GENERIC_SPECIALIZATION_EFFECT_MAY_THROW,
    };
    ASSERT(xa_mono_collector_add(&c, "apply", &identity, NULL) != NULL);
    method_args[0] = &equivalent_int_type;
    ASSERT(xa_mono_collector_add(&c, "apply", &identity, NULL) != NULL);
    ASSERT_EQ(c.count, 1);
    ASSERT(c.instances[0].identity.declaration_type_args != method_args);
    ASSERT(c.instances[0].identity.declaration_type_args[0] == &int_type);

    identity.effect = XA_GENERIC_SPECIALIZATION_EFFECT_NO_THROW;
    ASSERT(xa_mono_collector_add(&c, "apply", &identity, NULL) != NULL);
    ASSERT_EQ(c.count, 2);

    AstNode second_method = first_method;
    AstNode *second_methods[1] = {&second_method};
    AstNode second_owner = {.type = AST_CLASS_DECL};
    second_owner.as.class_decl.methods = second_methods;
    second_owner.as.class_decl.method_count = 1;
    identity.generic_decl = &second_method;
    identity.owner_decl = &second_owner;
    identity.effect = XA_GENERIC_SPECIALIZATION_EFFECT_MAY_THROW;
    ASSERT(xa_mono_collector_add(&c, "apply", &identity, NULL) != NULL);
    ASSERT_EQ(c.count, 3);

    AstNode generic_method = first_method;
    AstNode *generic_methods[1] = {&generic_method};
    AstNode generic_owner = {.type = AST_STRUCT_DECL};
    generic_owner.as.struct_decl.name = "Carrier";
    generic_owner.as.struct_decl.type_param_count = 1;
    generic_owner.as.struct_decl.methods = generic_methods;
    generic_owner.as.struct_decl.method_count = 1;
    XrTypeRef *receiver_args[1] = {&int_type};
    identity.generic_decl = &generic_method;
    identity.owner_decl = &generic_owner;
    identity.receiver_type_args = receiver_args;
    identity.receiver_type_arg_count = 1u;
    ASSERT(xa_generic_specialization_fact_valid(&identity));
    const char *generic_value_method = xa_mono_collector_add(&c, "apply", &identity, NULL);
    ASSERT(generic_value_method != NULL);
    ASSERT(strstr(generic_value_method, "$on$") != NULL);
    ASSERT_EQ(c.count, 5);
    ASSERT(c.instances[3].identity.generic_decl == &generic_owner);
    ASSERT(c.instances[3].identity.owner_decl == NULL);
    ASSERT_EQ(c.instances[3].identity.declaration_type_arg_count, 1u);
    ASSERT(c.instances[4].identity.generic_decl == &generic_method);
    ASSERT(c.instances[4].identity.owner_decl == &generic_owner);
    ASSERT_EQ(c.instances[4].identity.receiver_type_arg_count, 1u);
    ASSERT_EQ(c.instances[4].identity.declaration_type_arg_count, 1u);

    /* A generic method on a generic class instantiation is admitted the same
     * way as on a generic struct: the concrete class instantiation and the
     * method clone are both collected. */
    AstNode generic_class_method = first_method;
    AstNode *generic_class_methods[1] = {&generic_class_method};
    AstNode generic_class_owner = {.type = AST_CLASS_DECL};
    generic_class_owner.as.class_decl.name = "HeapCarrier";
    generic_class_owner.as.class_decl.type_param_count = 1;
    generic_class_owner.as.class_decl.methods = generic_class_methods;
    generic_class_owner.as.class_decl.method_count = 1;
    identity.generic_decl = &generic_class_method;
    identity.owner_decl = &generic_class_owner;
    ASSERT(xa_generic_specialization_fact_valid(&identity));
    const char *generic_class_method_name = xa_mono_collector_add(&c, "apply", &identity, NULL);
    ASSERT(generic_class_method_name != NULL);
    ASSERT(strstr(generic_class_method_name, "$on$") != NULL);
    ASSERT_EQ(c.count, 7);
    ASSERT(c.instances[5].identity.generic_decl == &generic_class_owner);
    ASSERT(c.instances[5].identity.owner_decl == NULL);
    ASSERT_EQ(c.instances[5].identity.declaration_type_arg_count, 1u);
    ASSERT(c.instances[6].identity.generic_decl == &generic_class_method);
    ASSERT(c.instances[6].identity.owner_decl == &generic_class_owner);
    ASSERT_EQ(c.instances[6].identity.receiver_type_arg_count, 1u);
    ASSERT_EQ(c.instances[6].identity.declaration_type_arg_count, 1u);
    ASSERT(!c.rewrite_failed);

    xa_mono_collector_free(&c);
}

/* The mangled name IS instance identity, so it must never drop an argument.
 * A fixed-size tag array used to make xr_mono_mangle fall back to the bare
 * generic name past its bound, collapsing every instance of that generic onto
 * one symbol -- silently, and only for wide parameter lists. */
TEST(mono_mangle_keeps_every_argument_when_wide) {
    enum {
        WIDE = 48
    };
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *args[WIDE];
    for (int i = 0; i < WIDE; i++)
        args[i] = (i % 2 == 0) ? &int_t : &str_t;

    char *wide = xr_mono_mangle("wide", args, WIDE);
    ASSERT(wide != NULL);
    /* Not the fallback: the base name alone would collide with every other
     * instantiation of the same generic. */
    ASSERT(strcmp(wide, "wide") != 0);

    int i64_tags = 0;
    for (const char *p = wide; (p = strstr(p, "i64")) != NULL; p += 3)
        i64_tags++;
    ASSERT_EQ(i64_tags, WIDE / 2);

    /* Changing one argument must change the name. */
    args[WIDE - 1] = &int_t;
    char *other = xr_mono_mangle("wide", args, WIDE);
    ASSERT(other != NULL);
    ASSERT(strcmp(wide, other) != 0);

    free(wide);
    free(other);
}

TEST(mono_mangle_structural_object_identity_is_complete) {
    XrTypeRef int_t = {.kind = XR_TREF_SCALAR, .scalar_rep = XR_NATIVE_I64};
    XrTypeRef str_t = {.kind = XR_TREF_STRING};
    XrTypeRef *field_types[] = {&str_t, &int_t};
    const char *user_names[] = {"name", "age"};
    const char *tagged_names[] = {"name", "tag"};
    bool mutable_fields[] = {false, false};
    bool readonly_name[] = {true, false};
    XrTypeRef user = {.kind = XR_TREF_OBJECT,
                      .nchildren = 2,
                      .children = field_types,
                      .field_names = user_names,
                      .field_readonly = mutable_fields};
    XrTypeRef tagged = {.kind = XR_TREF_OBJECT,
                        .nchildren = 2,
                        .children = field_types,
                        .field_names = tagged_names,
                        .field_readonly = mutable_fields};
    XrTypeRef readonly_user = {.kind = XR_TREF_OBJECT,
                               .nchildren = 2,
                               .children = field_types,
                               .field_names = user_names,
                               .field_readonly = readonly_name};
    XrTypeRef *user_args[] = {&user};
    XrTypeRef *tagged_args[] = {&tagged};
    XrTypeRef *readonly_args[] = {&readonly_user};
    char *user_name = xr_mono_mangle("read", user_args, 1);
    char *tagged_name = xr_mono_mangle("read", tagged_args, 1);
    char *readonly_user_name = xr_mono_mangle("read", readonly_args, 1);
    ASSERT(user_name != NULL && strstr(user_name, "read$obj2_") == user_name);
    ASSERT(tagged_name != NULL && strcmp(user_name, tagged_name) != 0);
    ASSERT(readonly_user_name != NULL && strcmp(user_name, readonly_user_name) != 0);
    free(user_name);
    free(tagged_name);
    free(readonly_user_name);
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("Name Mangling");
    RUN_TEST(mono_type_tag_basic);
    RUN_TEST(mono_scalar_tags_are_semantic_and_unique);
    RUN_TEST(mono_mangle_single);
    RUN_TEST(mono_mangle_multi);
    RUN_TEST(mono_mangle_uses_exact_nominal_declaration_identity);
    RUN_TEST(mono_mangle_preserves_const_capability_identity);
    RUN_TEST(mono_mangle_distinguishes_container_element_types);
    RUN_TEST(mono_mangle_distinguishes_tuple_and_optional_shapes);
    RUN_TEST(monomorphized_stdlib_type_preserves_sealed_capabilities);
    RUN_TEST(mono_mangle_null_name);
    RUN_TEST(mono_mangle_zero_args);
    RUN_TEST(mono_mangle_keeps_every_argument_when_wide);
    RUN_TEST(mono_mangle_structural_object_identity_is_complete);

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
    RUN_TEST(ast_clone_named_enum_record_nodes);

    RUN_TEST_SUITE("Mono Collector");
    RUN_TEST(mono_collector_basic);
    RUN_TEST(mono_collector_dedup);
    RUN_TEST(mono_collector_uses_exact_method_owner_and_effect_identity);

    TEST_REPORT();
    return xr_tests_failed > 0 ? 1 : 0;
}
