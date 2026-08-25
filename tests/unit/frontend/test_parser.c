/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser.c - Unit tests for parser (source -> AST)
 *
 * KEY CONCEPT:
 *   Verifies that the parser produces correct AST structure for various
 *   language constructs: literals, expressions, statements, functions,
 *   classes, error recovery, etc.
 */

#include "../test_framework.h"
#include <string.h>
#include <assert.h>

#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_api.h"
#include "frontend/parser/xast_types.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xtype_ref.h"
#include "xray.h"
#include "runtime/xisolate_internal.h"

/* ========== Test Infrastructure ========== */

static XrVMRuntime *X = NULL;
static XrCompilerSession *X_session = NULL;

static void setup(void) {
    XrVMConfig params = {0};
    X = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(X);
    X_session = xr_compiler_session_current_for_isolate(X);
    ASSERT_NOT_NULL(X_session);
}

static void teardown(void) {
    if (X) {
        xray_vm_delete(X);
        X = NULL;
        X_session = NULL;
    }
}

/* Helper: parse source and assert success */
static AstNode *parse_ok(const char *source) {
    AstNode *ast = xr_parse(xr_compiler_session_current_for_isolate(X), source);
    assert(ast != NULL && "parse_ok: parse failed");
    assert(ast->type == AST_PROGRAM);
    return ast;
}

/* Helper: get first statement from program */
static AstNode *first_stmt(AstNode *program) {
    assert(program->as.program.count > 0 && "first_stmt: empty program");
    return program->as.program.statements[0];
}

/* Helper: parse and return first statement */
static AstNode *parse_first(const char *source) {
    AstNode *program = parse_ok(source);
    return first_stmt(program);
}

/* Helper: parse a bare expression and return its AST node.
 *
 * An expression is not a statement on its own — a statement whose value is
 * discarded and that cannot do anything is E0208 (see §1.2.1). Expression
 * shape is therefore probed through a binding, which is also how expressions
 * appear in real code. */
static AstNode *parse_expr(const char *expr) {
    char source[512];
    snprintf(source, sizeof(source), "var __probe = %s", expr);
    AstNode *stmt = parse_first(source);
    assert(stmt->type == AST_VAR_DECL && "parse_expr: expected a var declaration");
    AstNode *init = stmt->as.var_decl.initializer;
    assert(init != NULL && "parse_expr: missing initializer");
    return init;
}

/* ========== Literal Tests ========== */

TEST(parser_int_literal) {
    setup();
    AstNode *expr = parse_expr("42");
    ASSERT_EQ_INT(expr->type, AST_LITERAL_INT);
    ASSERT_EQ_INT((int) expr->as.literal.raw_value.int_val, 42);
    teardown();
}

TEST(parser_float_literal) {
    setup();
    AstNode *expr = parse_expr("3.14");
    ASSERT_EQ_INT(expr->type, AST_LITERAL_FLOAT);
    ASSERT_TRUE(expr->as.literal.raw_value.float_val > 3.13 &&
                expr->as.literal.raw_value.float_val < 3.15);
    teardown();
}

TEST(parser_string_literal) {
    setup();
    AstNode *expr = parse_expr("\"hello\"");
    ASSERT_EQ_INT(expr->type, AST_LITERAL_STRING);
    ASSERT_STR_EQ(expr->as.literal.raw_value.string_val, "hello");
    teardown();
}

TEST(parser_block_string_normalizes_crlf_and_margin) {
    setup();
    AstNode *stmt = parse_first("var s = \"\"\"\r\n"
                                "    alpha\r\n"
                                "    beta\r\n"
                                "    \"\"\"\r\n");
    AstNode *expr = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_STRING);
    ASSERT_STR_EQ(expr->as.literal.raw_value.string_val, "alpha\nbeta");
    ASSERT_EQ_INT(expr->as.literal.escape_mode, XR_LITERAL_ESCAPED);
    ASSERT_EQ_INT(expr->as.literal.source_form, XR_LITERAL_BLOCK);
    teardown();
}

TEST(parser_non_expression_string_consumers_use_shared_decoder) {
    setup();
    AstNode *program = parse_ok("import \"\"\"\n"
                                "    ./module\n"
                                "    \"\"\"\n"
                                "export * from \"\"\"\n"
                                "    ./dependency\n"
                                "    \"\"\"\n"
                                "asm {\n"
                                "    \"\"\"\n"
                                "    mov \\\"x\\\"\n"
                                "    \"\"\"\n"
                                "}\n");
    ASSERT_EQ_INT(program->as.program.count, 3);
    AstNode *import = program->as.program.statements[0];
    ASSERT_EQ_INT(import->type, AST_IMPORT_STMT);
    ASSERT_STR_EQ(import->as.import_stmt.module_name, "./module");
    AstNode *reexport = program->as.program.statements[1];
    ASSERT_EQ_INT(reexport->type, AST_EXPORT_STMT);
    ASSERT_STR_EQ(reexport->as.export_stmt.from_path, "./dependency");
    AstNode *global_asm = program->as.program.statements[2];
    ASSERT_EQ_INT(global_asm->type, AST_GLOBAL_ASM);
    ASSERT_STR_EQ(global_asm->as.global_asm.text, "mov \"x\"");
    teardown();
}

TEST(parser_block_object_key_and_coroutine_name_use_shared_decoder) {
    setup();
    AstNode *program = parse_ok("var object = {\n"
                                "    \"\"\"\n"
                                "    key\n"
                                "    \"\"\"\n"
                                "    : 1\n"
                                "}\n"
                                "var task = go(name: \"\"\"\n"
                                "    worker\n"
                                "    \"\"\"\n"
                                ") work()\n");
    AstNode *object = program->as.program.statements[0]->as.var_decl.initializer;
    ASSERT_EQ_INT(object->type, AST_OBJECT_LITERAL);
    ASSERT_EQ_INT(object->as.object_literal.count, 1);
    ASSERT_STR_EQ(object->as.object_literal.keys[0]->as.literal.raw_value.string_val, "key");
    ASSERT_EQ_INT(object->as.object_literal.keys[0]->as.literal.source_form, XR_LITERAL_BLOCK);
    AstNode *go_expr = program->as.program.statements[1]->as.var_decl.initializer;
    ASSERT_EQ_INT(go_expr->type, AST_GO_EXPR);
    ASSERT_STR_EQ(go_expr->as.go_expr.name, "worker");
    teardown();
}

TEST(parser_fixed_bytes_are_one_compact_payload_node) {
    setup();
    AstNode *stmt = parse_first("var bytes = br\"\"\"\"\n"
                                "    ${HOME}\\path\n"
                                "    \"\"\"\n"
                                "    \"\"\"\"\n");
    AstNode *expr = stmt->as.var_decl.initializer;
    static const uint8_t expected[] = "${HOME}\\path\n\"\"\"";
    ASSERT_EQ_INT(expr->type, AST_FIXED_BYTES_LITERAL);
    ASSERT_EQ_UINT(expr->as.fixed_bytes_literal.payload_length, sizeof(expected) - 1);
    ASSERT_TRUE(memcmp(expr->as.fixed_bytes_literal.payload, expected, sizeof(expected) - 1) == 0);
    ASSERT_FALSE(expr->as.fixed_bytes_literal.append_nul);
    ASSERT_EQ_INT(expr->as.fixed_bytes_literal.escape_mode, XR_LITERAL_RAW);
    ASSERT_EQ_INT(expr->as.fixed_bytes_literal.source_form, XR_LITERAL_BLOCK);
    teardown();
}

TEST(parser_c_literal_retains_nul_policy_without_element_ast) {
    setup();
    AstNode *stmt = parse_first("var bytes = c\"A\\xFF\"");
    AstNode *expr = stmt->as.var_decl.initializer;
    static const uint8_t expected[] = {'A', 0xFF};
    ASSERT_EQ_INT(expr->type, AST_FIXED_BYTES_LITERAL);
    ASSERT_EQ_UINT(expr->as.fixed_bytes_literal.payload_length, sizeof(expected));
    ASSERT_TRUE(memcmp(expr->as.fixed_bytes_literal.payload, expected, sizeof(expected)) == 0);
    ASSERT_TRUE(expr->as.fixed_bytes_literal.append_nul);
    ASSERT_EQ_INT(expr->as.fixed_bytes_literal.escape_mode, XR_LITERAL_ESCAPED);
    ASSERT_EQ_INT(expr->as.fixed_bytes_literal.source_form, XR_LITERAL_INLINE);
    teardown();
}

TEST(parser_q2_is_empty_for_every_prefix_family) {
    setup();
    AstNode *program = parse_ok("var s = \"\"\n"
                                "var r0 = r\"\"\n"
                                "var b0 = b\"\"\n"
                                "var br0 = br\"\"\n"
                                "var c0 = c\"\"\n"
                                "var cr0 = cr\"\"\n");
    ASSERT_EQ_INT(program->as.program.count, 6);
    for (int i = 0; i < 6; i++) {
        AstNode *literal = program->as.program.statements[i]->as.var_decl.initializer;
        if (i < 2) {
            ASSERT_EQ_INT(literal->type, AST_LITERAL_STRING);
            ASSERT_STR_EQ(literal->as.literal.raw_value.string_val, "");
        } else {
            ASSERT_EQ_INT(literal->type, AST_FIXED_BYTES_LITERAL);
            ASSERT_EQ_UINT(literal->as.fixed_bytes_literal.payload_length, 0);
        }
    }
    teardown();
}

TEST(parser_template_interpolation_skips_nested_variable_quote_block) {
    setup();
    AstNode *stmt = parse_first("var s = \"\"\"\n"
                                "value=${len(br\"\"\"\n"
                                "abc\n"
                                "\"\"\"\n"
                                ")}\n"
                                "\"\"\"\n");
    AstNode *expr = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(expr->type, AST_TEMPLATE_STRING);
    ASSERT_EQ_INT(expr->as.template_str.part_count, 2);
    ASSERT_EQ_INT(expr->as.template_str.parts[1]->type, AST_CALL_EXPR);
    AstNode *arg = expr->as.template_str.parts[1]->as.call_expr.arguments[0];
    ASSERT_EQ_INT(arg->type, AST_FIXED_BYTES_LITERAL);
    ASSERT_EQ_UINT(arg->as.fixed_bytes_literal.payload_length, 3);
    teardown();
}

TEST(parser_bool_literal) {
    setup();
    AstNode *expr = parse_expr("true");
    ASSERT_EQ_INT(expr->type, AST_LITERAL_TRUE);
    teardown();
}

TEST(parser_null_literal) {
    setup();
    AstNode *expr = parse_expr("null");
    ASSERT_EQ_INT(expr->type, AST_LITERAL_NULL);
    teardown();
}

/* ========== Expression Tests ========== */

TEST(parser_binary_add) {
    setup();
    AstNode *expr = parse_expr("1 + 2");
    ASSERT_EQ_INT(expr->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_LITERAL_INT);
    ASSERT_EQ_INT(expr->as.binary.right->type, AST_LITERAL_INT);
    teardown();
}

TEST(parser_binary_precedence) {
    setup();
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    AstNode *expr = parse_expr("1 + 2 * 3");
    ASSERT_EQ_INT(expr->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_LITERAL_INT);
    ASSERT_EQ_INT(expr->as.binary.right->type, AST_BINARY_MUL);
    teardown();
}

TEST(parser_unary_neg) {
    setup();
    AstNode *expr = parse_expr("-42");
    ASSERT_EQ_INT(expr->type, AST_UNARY_NEG);
    ASSERT_EQ_INT(expr->as.unary.operand->type, AST_LITERAL_INT);
    teardown();
}

TEST(parser_unary_not) {
    setup();
    AstNode *expr = parse_expr("!true");
    ASSERT_EQ_INT(expr->type, AST_UNARY_NOT);
    ASSERT_EQ_INT(expr->as.unary.operand->type, AST_LITERAL_TRUE);
    teardown();
}

TEST(parser_grouping) {
    setup();
    // (1 + 2) * 3 should parse as (1+2) * 3
    AstNode *expr = parse_expr("(1 + 2) * 3");
    ASSERT_EQ_INT(expr->type, AST_BINARY_MUL);
    // left should be grouping containing add
    AstNode *left = expr->as.binary.left;
    ASSERT_EQ_INT(left->type, AST_GROUPING);
    ASSERT_EQ_INT(left->as.grouping->type, AST_BINARY_ADD);
    teardown();
}

TEST(parser_comparison) {
    setup();
    AstNode *expr = parse_expr("a == b");
    ASSERT_EQ_INT(expr->type, AST_BINARY_EQ);
    teardown();
}

TEST(parser_logical_and_or) {
    setup();
    // a && b || c should parse as (a && b) || c
    AstNode *expr = parse_expr("a && b || c");
    ASSERT_EQ_INT(expr->type, AST_BINARY_OR);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_BINARY_AND);
    teardown();
}

/* ========== Variable Declaration Tests ========== */

TEST(parser_let_decl) {
    setup();
    AstNode *stmt = parse_first("var x = 10");
    ASSERT_EQ_INT(stmt->type, AST_VAR_DECL);
    ASSERT_STR_EQ(stmt->as.var_decl.name, "x");
    ASSERT_NOT_NULL(stmt->as.var_decl.initializer);
    ASSERT_EQ_INT(stmt->as.var_decl.initializer->type, AST_LITERAL_INT);
    teardown();
}

TEST(parser_const_decl) {
    setup();
    AstNode *stmt = parse_first("const PI = 3.14");
    ASSERT_EQ_INT(stmt->type, AST_CONST_DECL);
    ASSERT_STR_EQ(stmt->as.var_decl.name, "PI");
    ASSERT_NOT_NULL(stmt->as.var_decl.initializer);
    teardown();
}

/* ========== Control Flow Tests ========== */

TEST(parser_if_stmt) {
    setup();
    AstNode *stmt = parse_first("if (x > 0) {\n  print(x)\n}");
    ASSERT_EQ_INT(stmt->type, AST_IF_STMT);
    ASSERT_NOT_NULL(stmt->as.if_stmt.condition);
    ASSERT_NOT_NULL(stmt->as.if_stmt.then_branch);
    teardown();
}

TEST(parser_if_else) {
    setup();
    AstNode *stmt = parse_first("if (x > 0) {\n  print(1)\n} else {\n  print(0)\n}");
    ASSERT_EQ_INT(stmt->type, AST_IF_STMT);
    ASSERT_NOT_NULL(stmt->as.if_stmt.else_branch);
    teardown();
}

TEST(parser_while_stmt) {
    setup();
    AstNode *stmt = parse_first("while (x > 0) {\n  x = x - 1\n}");
    ASSERT_EQ_INT(stmt->type, AST_WHILE_STMT);
    ASSERT_NOT_NULL(stmt->as.while_stmt.condition);
    ASSERT_NOT_NULL(stmt->as.while_stmt.body);
    teardown();
}

TEST(parser_for_stmt) {
    setup();
    AstNode *stmt = parse_first("for (var i = 0; i < 10; i++) {\n  print(i)\n}");
    ASSERT_EQ_INT(stmt->type, AST_FOR_STMT);
    ASSERT_NOT_NULL(stmt->as.for_stmt.initializer);
    ASSERT_NOT_NULL(stmt->as.for_stmt.condition);
    ASSERT_NOT_NULL(stmt->as.for_stmt.increment);
    ASSERT_NOT_NULL(stmt->as.for_stmt.body);
    teardown();
}

TEST(parser_select_wildcard_default_arm) {
    setup();
    AstNode *stmt = parse_first("select {\n  _ -> { print(\"idle\") }\n}");
    ASSERT_EQ_INT(stmt->type, AST_SELECT_STMT);
    ASSERT_EQ_INT(stmt->as.select_stmt.case_count, 1);
    AstNode *case_node = stmt->as.select_stmt.cases[0];
    ASSERT_NOT_NULL(case_node);
    ASSERT_EQ_INT(case_node->type, AST_SELECT_CASE);
    ASSERT(case_node->as.select_case.is_default);
    ASSERT(!case_node->as.select_case.is_send);
    ASSERT(!case_node->as.select_case.is_timeout);
    teardown();
}

TEST(parser_select_channel_arrow_remains_arm_delimiter) {
    setup();
    AstNode *stmt = parse_first("select {\n  value from ch -> { print(value) }\n}");
    ASSERT_EQ_INT(stmt->type, AST_SELECT_STMT);
    ASSERT_EQ_INT(stmt->as.select_stmt.case_count, 1);
    AstNode *case_node = stmt->as.select_stmt.cases[0];
    ASSERT_NOT_NULL(case_node);
    ASSERT_EQ_INT(case_node->type, AST_SELECT_CASE);
    ASSERT_NOT_NULL(case_node->as.select_case.channel);
    ASSERT_EQ_INT(case_node->as.select_case.channel->type, AST_VARIABLE);
    ASSERT_STR_EQ(case_node->as.select_case.channel->as.variable.name, "ch");
    teardown();
}

TEST(parser_select_default_keyword_rejected) {
    setup();
    AstNode *ast = xr_parse(xr_compiler_session_current_for_isolate(X),
                            "select {\n  default -> { print(\"idle\") }\n}");
    ASSERT_NULL(ast);
    teardown();
}

/* ========== Function Tests ========== */

TEST(parser_function_decl) {
    setup();
    AstNode *stmt = parse_first("fn add(a: i64, b: i64) -> i64 {\n  return a + b\n}");
    ASSERT_EQ_INT(stmt->type, AST_FUNCTION_DECL);
    ASSERT_STR_EQ(stmt->as.function_decl.name, "add");
    ASSERT_EQ_INT(stmt->as.function_decl.param_count, 2);
    ASSERT_NOT_NULL(stmt->as.function_decl.body);
    teardown();
}

TEST(parser_const_type_qualifier_is_canonical_in_every_position) {
    setup();
    AstNode *decl =
        parse_first("fn inspect(value: const Array<const string>) -> const Array<i64> {\n"
                    "  return value\n"
                    "}");
    ASSERT_EQ_INT(decl->type, AST_FUNCTION_DECL);
    XrTypeRef *param = decl->as.function_decl.params[0]->type;
    ASSERT_EQ_INT(param->kind, XR_TREF_CONST);
    ASSERT_EQ_INT(param->children[0]->kind, XR_TREF_GENERIC);
    ASSERT_EQ_INT(param->children[0]->children[0]->kind, XR_TREF_CONST);
    ASSERT_EQ_INT(decl->as.function_decl.return_type->kind, XR_TREF_CONST);

    char text[128];
    xr_tref_to_string_buf(param, text, (int) sizeof(text));
    ASSERT_STR_EQ(text, "const Array<const string>");

    AstNode *idempotent = parse_first("var x: const const Array<i64>");
    ASSERT_EQ_INT(idempotent->as.var_decl.type_annotation->kind, XR_TREF_CONST);
    ASSERT_EQ_INT(idempotent->as.var_decl.type_annotation->children[0]->kind, XR_TREF_GENERIC);
    teardown();
}

TEST(parser_extern_block_flattens_typed_descriptors) {
    setup();
    AstNode *program = parse_ok("extern \"C\" {\n"
                                "  export fn cos(x: f64) -> f64\n"
                                "  fn clear(value: i32)\n"
                                "}");
    ASSERT_EQ_INT(program->as.program.count, 2);
    for (int i = 0; i < program->as.program.count; i++) {
        AstNode *decl = program->as.program.statements[i];
        ASSERT_EQ_INT(decl->type, AST_FUNCTION_DECL);
        ASSERT_NULL(decl->as.function_decl.body);
        ASSERT_TRUE(decl->as.function_decl.is_extern);
        ASSERT_STR_EQ(decl->as.function_decl.extern_abi, "C");
        ASSERT_EQ_INT(decl->as.function_decl.attr_count, 0);
    }
    ASSERT_TRUE(program->as.program.statements[0]->is_exported);
    ASSERT_FALSE(program->as.program.statements[1]->is_exported);
    ASSERT_EQ_INT(program->as.program.statements[1]->as.function_decl.params[0]->passing_mode,
                  XR_PARAM_READ);
    teardown();
}

TEST(parser_extern_block_rejects_layout_declarations) {
    setup();
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X),
                         "extern \"C\" { struct Header { tag: u8 } }"));
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X),
                         "extern \"C\" dylib(\"m\") { fn cos(x: f64) -> f64 }"));
    teardown();
}

static void assert_ast_param_modes(XrParamNode **params, int actual_count,
                                   const XrParamMode *expected, int expected_count) {
    ASSERT_EQ_INT(actual_count, expected_count);
    for (int i = 0; i < expected_count; i++)
        ASSERT_EQ_INT(params[i]->passing_mode, expected[i]);
}

TEST(parser_parameter_modes_share_annotation_parser) {
    setup();

    const XrParamMode modes[] = {XR_PARAM_READ, XR_PARAM_READ, XR_PARAM_REF, XR_PARAM_MOVE};
    const int mode_count = (int) (sizeof(modes) / sizeof(modes[0]));

    AstNode *decl = parse_first("fn touch(a: i64, b: i64, c: ref i64, d: move i64) {\n}");
    ASSERT_EQ_INT(decl->type, AST_FUNCTION_DECL);
    assert_ast_param_modes(decl->as.function_decl.params, decl->as.function_decl.param_count, modes,
                           mode_count);

    AstNode *stmt =
        parse_first("var f = fn(a: i64, b: i64, c: ref i64, d: move i64) {\n  return a\n}");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_FUNCTION_EXPR);
    assert_ast_param_modes(init->as.function_expr.params, init->as.function_expr.param_count, modes,
                           mode_count);

    AstNode *method_stmt =
        parse_first("class Box {\n  touch(a: i64, b: i64, c: ref i64, d: move i64) {}\n}");
    AstNode *method = method_stmt->as.class_decl.methods[0];
    ASSERT_EQ_INT(method->type, AST_METHOD_DECL);
    assert_ast_param_modes(method->as.method_decl.params, method->as.method_decl.param_count, modes,
                           mode_count);

    AstNode *iface =
        parse_first("interface Sink {\n  write(a: i64, b: i64, c: ref i64, d: move i64) -> ()\n}");
    AstNode *iface_method = iface->as.interface_decl.methods[0];
    ASSERT_EQ_INT(iface_method->type, AST_INTERFACE_METHOD);
    assert_ast_param_modes(iface_method->as.interface_method.params,
                           iface_method->as.interface_method.param_count, modes, mode_count);

    teardown();
}

TEST(parser_oop_parameter_modes_share_annotation_parser) {
    setup();

    AstNode *ctor_class = parse_first("class Box {\n"
                                      "  constructor(value: ref i64) {}\n"
                                      "}");
    AstNode *ctor = ctor_class->as.class_decl.methods[0];
    ASSERT_EQ_INT(ctor->type, AST_METHOD_DECL);
    ASSERT(ctor->as.method_decl.is_constructor);
    ASSERT_EQ_INT(ctor->as.method_decl.param_count, 1);
    ASSERT_EQ_INT(ctor->as.method_decl.params[0]->passing_mode, XR_PARAM_REF);

    AstNode *operator_class = parse_first("class Matrix {\n"
                                          "  operator[]=(index: i64, value: move i64) {}\n"
                                          "}");
    AstNode *op = operator_class->as.class_decl.methods[0];
    ASSERT_EQ_INT(op->type, AST_METHOD_DECL);
    ASSERT(op->as.method_decl.is_operator);
    ASSERT_EQ_INT(op->as.method_decl.param_count, 2);
    ASSERT_EQ_INT(op->as.method_decl.params[0]->passing_mode, XR_PARAM_READ);
    ASSERT_EQ_INT(op->as.method_decl.params[1]->passing_mode, XR_PARAM_MOVE);

    AstNode *setter_class = parse_first("class Meter {\n"
                                        "  value: i64 {\n"
                                        "    fn(v: ref i64) {}\n"
                                        "  }\n"
                                        "}");
    AstNode *setter = setter_class->as.class_decl.methods[0];
    ASSERT_EQ_INT(setter->type, AST_METHOD_DECL);
    ASSERT(setter->as.method_decl.is_setter);
    ASSERT_EQ_INT(setter->as.method_decl.param_count, 1);
    ASSERT_EQ_INT(setter->as.method_decl.params[0]->passing_mode, XR_PARAM_REF);

    AstNode *contract_class = parse_first("class ContractBox {\n"
                                          "  configure(limit: i64 = 4) {}\n"
                                          "  collect(...values: i64) {}\n"
                                          "}");
    MethodDeclNode *configure = &contract_class->as.class_decl.methods[0]->as.method_decl;
    ASSERT_EQ_INT(configure->param_count, 1);
    ASSERT_STR_EQ(configure->params[0]->name, "limit");
    ASSERT_EQ_INT(configure->params[0]->passing_mode, XR_PARAM_READ);
    ASSERT_NOT_NULL(configure->params[0]->type);
    ASSERT_NOT_NULL(configure->params[0]->default_value);
    ASSERT_FALSE(configure->params[0]->is_rest);

    MethodDeclNode *collect = &contract_class->as.class_decl.methods[1]->as.method_decl;
    ASSERT_EQ_INT(collect->param_count, 1);
    ASSERT_STR_EQ(collect->params[0]->name, "values");
    ASSERT_EQ_INT(collect->params[0]->passing_mode, XR_PARAM_READ);
    ASSERT_NOT_NULL(collect->params[0]->type);
    ASSERT_NULL(collect->params[0]->default_value);
    ASSERT(collect->params[0]->is_rest);

    teardown();
}

TEST(parser_function_type_param_modes) {
    setup();

    AstNode *alias = parse_first("type Handler = fn(i64, ref string, move bool) -> i64");
    ASSERT_EQ_INT(alias->type, AST_TYPE_ALIAS);
    XrTypeRef *tref = alias->as.type_alias.resolved_type;
    ASSERT_NOT_NULL(tref);
    ASSERT_EQ_INT(tref->kind, XR_TREF_FUNCTION);
    ASSERT_EQ_INT(tref->nchildren, 4);
    ASSERT_NOT_NULL(tref->function_param_modes);
    ASSERT_EQ_INT(tref->function_param_modes[0], XR_PARAM_READ);
    ASSERT_EQ_INT(tref->function_param_modes[1], XR_PARAM_REF);
    ASSERT_EQ_INT(tref->function_param_modes[2], XR_PARAM_MOVE);

    char buf[128];
    xr_tref_to_string_buf(tref, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "fn(i64, ref string, move bool) -> i64");

    AstNode *complex_alias =
        parse_first("type ComplexHandler = fn(Array<i64>, ref Slice<u8>?, "
                    "move [u8; 16], (i64, string), fn(ref i64) -> bool,) -> Array<string>");
    XrTypeRef *complex = complex_alias->as.type_alias.resolved_type;
    ASSERT_NOT_NULL(complex);
    ASSERT_EQ_INT(complex->kind, XR_TREF_FUNCTION);
    ASSERT_EQ_INT(complex->nchildren, 6);
    ASSERT_NOT_NULL(complex->function_param_modes);
    ASSERT_EQ_INT(complex->function_param_modes[0], XR_PARAM_READ);
    ASSERT_EQ_INT(complex->function_param_modes[1], XR_PARAM_REF);
    ASSERT_EQ_INT(complex->function_param_modes[2], XR_PARAM_MOVE);
    ASSERT_EQ_INT(complex->function_param_modes[3], XR_PARAM_READ);
    ASSERT_EQ_INT(complex->function_param_modes[4], XR_PARAM_READ);
    ASSERT_EQ_INT(complex->children[0]->kind, XR_TREF_GENERIC);
    ASSERT_STR_EQ(complex->children[0]->name, "Array");
    ASSERT_EQ_INT(complex->children[1]->kind, XR_TREF_OPTIONAL);
    ASSERT_EQ_INT(complex->children[1]->children[0]->kind, XR_TREF_GENERIC);
    ASSERT_STR_EQ(complex->children[1]->children[0]->name, "Slice");
    ASSERT_EQ_INT(complex->children[2]->kind, XR_TREF_FIXED_ARRAY);
    ASSERT_EQ_INT(complex->children[2]->fixed_length, 16);
    ASSERT_EQ_INT(complex->children[3]->kind, XR_TREF_TUPLE);
    ASSERT_EQ_INT(complex->children[3]->nchildren, 2);
    ASSERT_EQ_INT(complex->children[4]->kind, XR_TREF_FUNCTION);
    ASSERT_NOT_NULL(complex->children[4]->function_param_modes);
    ASSERT_EQ_INT(complex->children[4]->function_param_modes[0], XR_PARAM_REF);
    ASSERT_EQ_INT(complex->children[5]->kind, XR_TREF_GENERIC);
    ASSERT_STR_EQ(complex->children[5]->name, "Array");
    char complex_buf[512];
    xr_tref_to_string_buf(complex, complex_buf, sizeof(complex_buf));
    ASSERT_STR_EQ(complex_buf, "fn(Array<i64>, ref Slice<u8>?, move [u8; 16], (i64, string), "
                               "fn(ref i64) -> bool) -> Array<string>");

    teardown();
}

TEST(parser_function_no_params) {
    setup();
    AstNode *stmt = parse_first("fn greet() {\n  print(\"hi\")\n}");
    ASSERT_EQ_INT(stmt->type, AST_FUNCTION_DECL);
    ASSERT_STR_EQ(stmt->as.function_decl.name, "greet");
    ASSERT_EQ_INT(stmt->as.function_decl.param_count, 0);
    teardown();
}

TEST(parser_return_stmt) {
    setup();
    AstNode *program = parse_ok("fn f() -> i64 {\n  return 42\n}");
    AstNode *fn = first_stmt(program);
    ASSERT_EQ_INT(fn->type, AST_FUNCTION_DECL);
    // body is a block
    AstNode *body = fn->as.function_decl.body;
    ASSERT_EQ_INT(body->type, AST_BLOCK);
    ASSERT_TRUE(body->as.block.count > 0);
    AstNode *ret = body->as.block.statements[0];
    ASSERT_EQ_INT(ret->type, AST_RETURN_STMT);
    ASSERT_EQ_INT(ret->line, 2);
    teardown();
}

/* ========== Array Tests ========== */

TEST(parser_array_literal) {
    setup();
    AstNode *expr = parse_expr("[1, 2, 3]");
    ASSERT_EQ_INT(expr->type, AST_ARRAY_LITERAL);
    ASSERT_EQ_INT(expr->as.array_literal.count, 3);
    teardown();
}

TEST(parser_index_get) {
    setup();
    AstNode *expr = parse_expr("arr[0]");
    ASSERT_EQ_INT(expr->type, AST_INDEX_GET);
    teardown();
}

/* ========== Object/Map Tests ========== */

TEST(parser_object_literal) {
    setup();
    // Object literal in assignment context (bare {} is parsed as block)
    AstNode *stmt = parse_first("var obj = {a: 1, b: 2}");
    ASSERT_EQ_INT(stmt->type, AST_VAR_DECL);
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_NOT_NULL(init);
    ASSERT_EQ_INT(init->type, AST_OBJECT_LITERAL);
    ASSERT_EQ_INT(init->as.object_literal.count, 2);
    teardown();
}

/* ========== Class Tests ========== */

TEST(parser_class_decl) {
    setup();
    AstNode *stmt = parse_first("class Dog {\n"
                                "  name: string\n"
                                "  bark() {\n"
                                "    print(\"woof\")\n"
                                "  }\n"
                                "}");
    ASSERT_EQ_INT(stmt->type, AST_CLASS_DECL);
    ASSERT_STR_EQ(stmt->as.class_decl.name, "Dog");
    teardown();
}

TEST(parser_direct_visibility_owns_declaration) {
    setup();
    AstNode *program = parse_ok("@deprecated(\"use hash64\")\n"
                                "export fn hash() -> i64 { return 1 }\n"
                                "export final class FinalBox {}\n"
                                "export struct Word { value: u32 }\n");
    ASSERT_EQ_INT(program->as.program.count, 3);

    AstNode *fn = program->as.program.statements[0];
    ASSERT_EQ_INT(fn->type, AST_FUNCTION_DECL);
    ASSERT_TRUE(fn->is_exported);
    ASSERT_EQ_INT(fn->as.function_decl.attr_count, 1);
    ASSERT_EQ_INT(fn->as.function_decl.attributes[0]->kind, ATTR_DEPRECATED);
    ASSERT_STR_EQ(fn->as.function_decl.attributes[0]->str_arg, "use hash64");

    AstNode *cls = program->as.program.statements[1];
    ASSERT_EQ_INT(cls->type, AST_CLASS_DECL);
    ASSERT_TRUE(cls->is_exported);
    ASSERT_TRUE(cls->as.class_decl.explicit_final);

    AstNode *st = program->as.program.statements[2];
    ASSERT_EQ_INT(st->type, AST_STRUCT_DECL);
    ASSERT_TRUE(st->is_exported);
    teardown();
}

TEST(parser_method_deprecated_attribute) {
    setup();
    AstNode *program = parse_ok("struct Word {\n"
                                "  @deprecated(\"use rotateLeft\")\n"
                                "  rotate(n: i64) -> u32 { return 0 }\n"
                                "}\n");
    AstNode *st = program->as.program.statements[0];
    ASSERT_EQ_INT(st->type, AST_STRUCT_DECL);
    ASSERT_EQ_INT(st->as.struct_decl.method_count, 1);
    MethodDeclNode *method = &st->as.struct_decl.methods[0]->as.method_decl;
    ASSERT_EQ_INT(method->attr_count, 1);
    ASSERT_EQ_INT(method->attributes[0]->kind, ATTR_DEPRECATED);
    ASSERT_STR_EQ(method->attributes[0]->str_arg, "use rotateLeft");
    teardown();
}

TEST(parser_inline_control_attributes_and_conflict) {
    setup();
    AstNode *program = parse_ok("@inline\n"
                                "fn hot(value: i64) -> i64 { return value }\n"
                                "struct Worker {\n"
                                "  @noinline\n"
                                "  cold(value: i64) -> i64 { return value }\n"
                                "}\n");
    AstNode *fn = program->as.program.statements[0];
    ASSERT_EQ_INT(fn->type, AST_FUNCTION_DECL);
    ASSERT_EQ_INT(fn->as.function_decl.attr_count, 1);
    ASSERT_EQ_INT(fn->as.function_decl.attributes[0]->kind, ATTR_INLINE);

    AstNode *st = program->as.program.statements[1];
    ASSERT_EQ_INT(st->type, AST_STRUCT_DECL);
    ASSERT_EQ_INT(st->as.struct_decl.method_count, 1);
    MethodDeclNode *method = &st->as.struct_decl.methods[0]->as.method_decl;
    ASSERT_EQ_INT(method->attr_count, 1);
    ASSERT_EQ_INT(method->attributes[0]->kind, ATTR_NOINLINE);

    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X),
                         "@inline\n@noinline\nfn conflict() {}\n"));
    teardown();
}

TEST(parser_rejects_posthoc_local_export) {
    setup();
    AstNode *program =
        xr_parse(xr_compiler_session_current_for_isolate(X), "fn local() {}\nexport { local }\n");
    ASSERT_NULL(program);
    teardown();
}

TEST(parser_reexport_preserves_module_identity_kind) {
    setup();
    AstNode *program = parse_ok("export { U32x4 } from simd\n"
                                "export * from \"./local_vectors\"\n");
    ASSERT_EQ_INT(program->as.program.count, 2);

    AstNode *stdlib_export = program->as.program.statements[0];
    ASSERT_EQ_INT(stdlib_export->type, AST_EXPORT_STMT);
    ASSERT_STR_EQ(stdlib_export->as.export_stmt.from_path, "simd");
    ASSERT_FALSE(stdlib_export->as.export_stmt.from_is_quoted);

    AstNode *local_export = program->as.program.statements[1];
    ASSERT_EQ_INT(local_export->type, AST_EXPORT_STMT);
    ASSERT_STR_EQ(local_export->as.export_stmt.from_path, "./local_vectors");
    ASSERT_TRUE(local_export->as.export_stmt.from_is_quoted);
    teardown();
}

TEST(parser_enum_static_method) {
    setup();
    AstNode *stmt = parse_first("enum Color {\n"
                                "  Red,\n"
                                "  Green\n"
                                "  @deprecated\n"
                                "  static fn fromInt(v: i64) -> Color {\n"
                                "    return Color.Red\n"
                                "  }\n"
                                "  fn label() -> string {\n"
                                "    return this.name\n"
                                "  }\n"
                                "}");
    ASSERT_EQ_INT(stmt->type, AST_ENUM_DECL);
    ASSERT_STR_EQ(stmt->as.enum_decl.name, "Color");
    ASSERT_EQ_INT(stmt->as.enum_decl.member_count, 2);
    ASSERT_EQ_INT(stmt->as.enum_decl.method_count, 2);
    AstNode *factory = stmt->as.enum_decl.methods[0];
    AstNode *label = stmt->as.enum_decl.methods[1];
    ASSERT_EQ_INT(factory->type, AST_METHOD_DECL);
    ASSERT(factory->as.method_decl.is_static);
    ASSERT_STR_EQ(factory->as.method_decl.name, "fromInt");
    ASSERT_EQ_INT(factory->as.method_decl.attr_count, 1);
    ASSERT_EQ_INT(factory->as.method_decl.attributes[0]->kind, ATTR_DEPRECATED);
    ASSERT_EQ_INT(label->type, AST_METHOD_DECL);
    ASSERT(!label->as.method_decl.is_static);
    ASSERT_STR_EQ(label->as.method_decl.name, "label");
    teardown();
}

/* ========== Error Handling Tests ========== */

TEST(parser_error_returns_null) {
    setup();
    // Unclosed brace should cause parse error
    AstNode *ast = xr_parse(xr_compiler_session_current_for_isolate(X), "fn f() {");
    ASSERT_TRUE(ast == NULL);
    teardown();
}

TEST(parser_rejects_retired_storage_declarations) {
    setup();
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X), "shared value = 1"));
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X), "owned value = [1]"));
    teardown();
}

TEST(parser_rejects_move_const_parameter_capability) {
    setup();
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X),
                         "class Config {}\nfn publish(config: move const Config) {}"));
    ASSERT_NULL(xr_parse(xr_compiler_session_current_for_isolate(X),
                         "type Publisher = (move const i64) -> ()"));
    teardown();
}

TEST(parser_empty_source) {
    setup();
    AstNode *ast = xr_parse(xr_compiler_session_current_for_isolate(X), "");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ_INT(ast->type, AST_PROGRAM);
    ASSERT_EQ_INT(ast->as.program.count, 0);
    xr_program_destroy(ast);
    teardown();
}

/* ========== Multiple Statement Tests ========== */

TEST(parser_multiple_stmts) {
    setup();
    AstNode *program = parse_ok("var x = 1\n"
                                "var y = 2\n"
                                "print(x + y)");
    ASSERT_TRUE(program->as.program.count >= 3);
    ASSERT_EQ_INT(program->as.program.statements[0]->type, AST_VAR_DECL);
    ASSERT_EQ_INT(program->as.program.statements[1]->type, AST_VAR_DECL);
    teardown();
}

/* ========== Call Expression Tests ========== */

TEST(parser_call_expr) {
    setup();
    AstNode *stmt = parse_first("foo(1, 2, 3)");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(expr->as.call_expr.arg_count, 3);
    ASSERT_EQ_INT(expr->line, 1);
    ASSERT_EQ_INT(expr->column, 1);
    ASSERT_EQ_INT(expr->end_line, 1);
    ASSERT_EQ_INT(expr->end_column, 13);
    teardown();
}

TEST(parser_call_arg_access_markers_only_in_direct_argument_slot) {
    setup();

    AstNode *stmt = parse_first("foo("
                                "ref x, "
                                "move y, "
                                "ref this.value, "
                                "move z)");
    AstNode *call = stmt->as.expr_stmt;
    ASSERT_EQ_INT(call->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(call->as.call_expr.arg_count, 4);
    ASSERT_NOT_NULL(call->as.call_expr.arg_accesses);
    ASSERT_EQ_INT(call->as.call_expr.arg_accesses[0], XR_CALL_ARG_REF);
    ASSERT_EQ_INT(call->as.call_expr.arg_accesses[1], XR_CALL_ARG_MOVE);
    ASSERT_EQ_INT(call->as.call_expr.arg_accesses[2], XR_CALL_ARG_REF);
    ASSERT_EQ_INT(call->as.call_expr.arg_accesses[3], XR_CALL_ARG_MOVE);
    ASSERT_EQ_INT(call->as.call_expr.arguments[0]->type, AST_VARIABLE);
    ASSERT_STR_EQ(call->as.call_expr.arguments[0]->as.variable.name, "x");
    ASSERT_EQ_INT(call->as.call_expr.arguments[1]->type, AST_MOVE_EXPR);
    ASSERT_EQ_INT(call->as.call_expr.arguments[2]->type, AST_MEMBER_ACCESS);
    ASSERT_EQ_INT(call->as.call_expr.arguments[3]->type, AST_MOVE_EXPR);

    AstNode *plain_stmt = parse_first("foo(out, out + 1, ref + 1, ref.value, out.value)");
    AstNode *plain = plain_stmt->as.expr_stmt;
    ASSERT_EQ_INT(plain->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(plain->as.call_expr.arg_count, 5);
    for (int i = 0; i < plain->as.call_expr.arg_count; i++)
        ASSERT_EQ_INT(plain->as.call_expr.arg_accesses[i], XR_CALL_ARG_PLAIN);
    ASSERT_EQ_INT(plain->as.call_expr.arguments[0]->type, AST_VARIABLE);
    ASSERT_STR_EQ(plain->as.call_expr.arguments[0]->as.variable.name, "out");
    ASSERT_EQ_INT(plain->as.call_expr.arguments[1]->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(plain->as.call_expr.arguments[2]->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(plain->as.call_expr.arguments[3]->type, AST_MEMBER_ACCESS);
    ASSERT_EQ_INT(plain->as.call_expr.arguments[4]->type, AST_MEMBER_ACCESS);

    AstNode *generic_stmt = parse_first("foo<i64>("
                                        "ref x, "
                                        "move y)");
    AstNode *generic = generic_stmt->as.expr_stmt;
    ASSERT_EQ_INT(generic->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(generic->as.call_expr.type_arg_count, 1);
    ASSERT_EQ_INT(generic->as.call_expr.arg_accesses[0], XR_CALL_ARG_REF);
    ASSERT_EQ_INT(generic->as.call_expr.arg_accesses[1], XR_CALL_ARG_MOVE);

    AstNode *new_stmt = parse_first("Array<i64>("
                                    "move y)");
    AstNode *new_expr = new_stmt->as.expr_stmt;
    ASSERT_EQ_INT(new_expr->type, AST_NEW_EXPR);
    ASSERT_EQ_INT(new_expr->as.new_expr.arg_count, 1);
    ASSERT_EQ_INT(new_expr->as.new_expr.arg_accesses[0], XR_CALL_ARG_MOVE);

    AstNode *class_stmt = parse_first("class Child extends Base {\n"
                                      "  constructor(value: "
                                      "ref i64, result: "
                                      "move i64) {\n"
                                      "    super("
                                      "ref value, "
                                      "move result)\n"
                                      "  }\n"
                                      "}");
    AstNode *ctor = class_stmt->as.class_decl.methods[0];
    ASSERT_EQ_INT(ctor->type, AST_METHOD_DECL);
    ASSERT_NOT_NULL(ctor->as.method_decl.body);
    AstNode *super_stmt = ctor->as.method_decl.body->as.block.statements[0];
    ASSERT_EQ_INT(super_stmt->type, AST_EXPR_STMT);
    AstNode *super_call = super_stmt->as.expr_stmt;
    ASSERT_EQ_INT(super_call->type, AST_SUPER_CALL);
    ASSERT_EQ_INT(super_call->as.super_call.arg_count, 2);
    ASSERT_EQ_INT(super_call->as.super_call.arg_accesses[0], XR_CALL_ARG_REF);
    ASSERT_EQ_INT(super_call->as.super_call.arg_accesses[1], XR_CALL_ARG_MOVE);

    teardown();
}

TEST(parser_member_access) {
    setup();
    AstNode *expr = parse_expr("obj.field");
    ASSERT_EQ_INT(expr->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(expr->as.member_access.name, "field");
    teardown();
}

TEST(parser_member_generic_call_uintsize_type_arg) {
    setup();
    AstNode *expr = parse_expr("mem.sizeOf<usize>()");
    ASSERT_EQ_INT(expr->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(expr->as.call_expr.type_arg_count, 1);
    ASSERT_NOT_NULL(expr->as.call_expr.type_args);
    ASSERT_EQ_INT(expr->as.call_expr.type_args[0]->kind, XR_TREF_SCALAR);
    ASSERT_EQ_INT(expr->as.call_expr.type_args[0]->scalar_rep, XR_NATIVE_USIZE);
    teardown();
}

TEST(parser_member_generic_call_uintsize_after_binary_op) {
    setup();
    AstNode *expr = parse_expr("mem.sizeOf<usize>() + mem.alignOf<isize>()");
    ASSERT_EQ_INT(expr->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(expr->as.binary.left->as.call_expr.type_arg_count, 1);
    ASSERT_EQ_INT(expr->as.binary.right->type, AST_CALL_EXPR);
    ASSERT_EQ_INT(expr->as.binary.right->as.call_expr.type_arg_count, 1);
    ASSERT_EQ_INT(expr->as.binary.right->as.call_expr.type_args[0]->kind, XR_TREF_SCALAR);
    ASSERT_EQ_INT(expr->as.binary.right->as.call_expr.type_args[0]->scalar_rep, XR_NATIVE_ISIZE);
    teardown();
}

TEST(parser_scalar_spelling_registry_roundtrip) {
    setup();
    AstNode *alias = parse_first("type Scalars = (i8, i16, i32, i64, u8, u16, u32, u64, "
                                 "f32, f64, isize, usize)");
    ASSERT_EQ_INT(alias->type, AST_TYPE_ALIAS);
    XrTypeRef *tuple = alias->as.type_alias.resolved_type;
    ASSERT_NOT_NULL(tuple);
    ASSERT_EQ_INT(tuple->kind, XR_TREF_TUPLE);
    ASSERT_EQ_INT(tuple->nchildren, 12);
    char formatted[256];
    xr_tref_to_string_buf(tuple, formatted, sizeof(formatted));
    ASSERT_STR_EQ(formatted, "(i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, isize, usize)");
    ASSERT_EQ_INT(tuple->children[3]->scalar_rep, XR_NATIVE_I64);
    ASSERT_EQ_INT(tuple->children[4]->scalar_rep, XR_NATIVE_U8);
    ASSERT_EQ_INT(tuple->children[9]->scalar_rep, XR_NATIVE_F64);
    ASSERT_EQ_INT(tuple->children[10]->scalar_rep, XR_NATIVE_ISIZE);
    ASSERT_EQ_INT(tuple->children[11]->scalar_rep, XR_NATIVE_USIZE);
    teardown();
}

TEST(parser_retired_scalar_spelling_is_ordinary_identifier) {
    setup();
    AstNode *alias = parse_first("type int32 = i32");
    ASSERT_EQ_INT(alias->type, AST_TYPE_ALIAS);
    ASSERT_STR_EQ(alias->as.type_alias.name, "int32");
    ASSERT_NOT_NULL(alias->as.type_alias.resolved_type);
    ASSERT_EQ_INT(alias->as.type_alias.resolved_type->kind, XR_TREF_SCALAR);
    ASSERT_EQ_INT(alias->as.type_alias.resolved_type->scalar_rep, XR_NATIVE_I32);
    teardown();
}

/* ========== Tuple Tests ========== */

TEST(parser_tuple_unit_literal) {
    setup();
    /* `()` is the unit literal: a 0-arity AST_TUPLE_LITERAL. */
    AstNode *stmt = parse_first("var u = ()");
    ASSERT_EQ_INT(stmt->type, AST_VAR_DECL);
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_NOT_NULL(init);
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 0);
    teardown();
}

TEST(parser_tuple_unary_literal) {
    setup();
    /* `(x,)` -- the trailing comma is what distinguishes a 1-tuple from
     * a parenthesised scalar. */
    AstNode *stmt = parse_first("var u = (42,)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 1);
    teardown();
}

TEST(parser_tuple_multi_literal) {
    setup();
    AstNode *stmt = parse_first("var t = (1, \"hi\", true)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 3);
    /* Element types should reflect the heterogeneous payload. */
    ASSERT_EQ_INT(init->as.tuple_literal.elements[0]->type, AST_LITERAL_INT);
    ASSERT_EQ_INT(init->as.tuple_literal.elements[1]->type, AST_LITERAL_STRING);
    ASSERT_EQ_INT(init->as.tuple_literal.elements[2]->type, AST_LITERAL_TRUE);
    teardown();
}

TEST(parser_tuple_nested_literal) {
    setup();
    AstNode *stmt = parse_first("var n = ((1, 2), (3, 4))");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 2);
    AstNode *first = init->as.tuple_literal.elements[0];
    ASSERT_EQ_INT(first->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(first->as.tuple_literal.count, 2);
    teardown();
}

TEST(parser_tuple_with_trailing_comma) {
    setup();
    /* Trailing comma is allowed for arity > 1 too. */
    AstNode *stmt = parse_first("var t = (1, 2, 3,)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 3);
    teardown();
}

TEST(parser_tuple_type_over_16_elements) {
    setup();
    AstNode *stmt =
        parse_first("var t: (i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, "
                    "i64, i64, i64, i64, i64, i64, i64, i64, i64, i64) = "
                    "(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19)");
    ASSERT_EQ_INT(stmt->type, AST_VAR_DECL);
    XrTypeRef *ann = stmt->as.var_decl.type_annotation;
    ASSERT_NOT_NULL(ann);
    ASSERT_EQ_INT(ann->kind, XR_TREF_TUPLE);
    ASSERT_EQ_INT(ann->nchildren, 20);
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_NOT_NULL(init);
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 20);
    teardown();
}

TEST(parser_grouping_still_works) {
    setup();
    /* `(x)` without a trailing comma must remain a grouping, otherwise
     * every existing parenthesised expression would silently turn into
     * a 1-tuple. */
    AstNode *stmt = parse_first("var g = (1 + 2)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_GROUPING);
    teardown();
}

TEST(parser_arrow_fn_not_tuple) {
    setup();
    /* `(a, b) -> a + b` must still parse as a function expression,
     * not as a 2-tuple of variable references followed by `->`. */
    AstNode *stmt = parse_first("var f = (a, b) -> a + b");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_FUNCTION_EXPR);
    teardown();
}

TEST(parser_arrow_parameters_support_independent_annotations_and_modes) {
    setup();
    AstNode *stmt = parse_first("var f = (a: i64, b, c: ref i64, d: move Buffer,) -> a + b");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_FUNCTION_EXPR);
    ASSERT_EQ_INT(init->as.function_expr.param_count, 4);
    ASSERT_NOT_NULL(init->as.function_expr.params[0]->type);
    ASSERT_NULL(init->as.function_expr.params[1]->type);
    ASSERT_NOT_NULL(init->as.function_expr.params[2]->type);
    ASSERT_NOT_NULL(init->as.function_expr.params[3]->type);
    ASSERT_EQ_INT(init->as.function_expr.params[0]->passing_mode, XR_PARAM_READ);
    ASSERT_EQ_INT(init->as.function_expr.params[1]->passing_mode, XR_PARAM_READ);
    ASSERT_EQ_INT(init->as.function_expr.params[2]->passing_mode, XR_PARAM_REF);
    ASSERT_EQ_INT(init->as.function_expr.params[3]->passing_mode, XR_PARAM_MOVE);

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(X);
    ASSERT_NULL(xr_parse(session, "var f = (ref x) -> x"));
    ASSERT_NULL(xr_parse(session, "var f = (x: i64 = 1) -> x"));
    ASSERT_NULL(xr_parse(session, "var f = (...xs: i64) -> xs"));
    teardown();
}

TEST(parser_bare_lambda_in_expression_position) {
    setup();
    AstNode *stmt = parse_first("var f: fn(i64) -> i64 = x -> x * 2");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_FUNCTION_EXPR);
    ASSERT_EQ_INT(init->as.function_expr.param_count, 1);
    ASSERT_STR_EQ(init->as.function_expr.params[0]->name, "x");
    teardown();
}

TEST(parser_tuple_field_access) {
    setup();
    /* `t.0` parses as AST_MEMBER_ACCESS with member name "0" -- the
     * analyzer recognises digit-only names on tuple receivers. */
    AstNode *expr = parse_expr("t.0");
    ASSERT_EQ_INT(expr->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(expr->as.member_access.name, "0");
    teardown();
}

TEST(parser_tuple_destructure_let) {
    setup();
    /* `var (a, b) = pair` — produces AST_DESTRUCTURE_DECL whose
     * pattern is PATTERN_TUPLE with two identifier sub-patterns. */
    AstNode *stmt = parse_first("var (a, b) = pair");
    ASSERT_EQ_INT(stmt->type, AST_DESTRUCTURE_DECL);
    XrDestructurePattern *p = stmt->as.destructure_decl.pattern;
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->type, PATTERN_TUPLE);
    ASSERT_EQ_INT(p->as.array.element_count, 2);
    ASSERT_EQ_INT(p->as.array.elements[0]->type, PATTERN_IDENTIFIER);
    ASSERT_STR_EQ(p->as.array.elements[0]->as.identifier.name, "a");
    ASSERT_EQ_INT(p->as.array.elements[1]->type, PATTERN_IDENTIFIER);
    ASSERT_STR_EQ(p->as.array.elements[1]->as.identifier.name, "b");
    teardown();
}

TEST(parser_tuple_destructure_with_skip) {
    setup();
    /* `_` slots produce PATTERN_SKIP rather than PATTERN_IDENTIFIER,
     * matching the array-pattern convention. */
    AstNode *stmt = parse_first("var (x, _, z) = triple");
    XrDestructurePattern *p = stmt->as.destructure_decl.pattern;
    ASSERT_EQ_INT(p->type, PATTERN_TUPLE);
    ASSERT_EQ_INT(p->as.array.element_count, 3);
    ASSERT_EQ_INT(p->as.array.elements[1]->type, PATTERN_SKIP);
    teardown();
}

TEST(parser_tuple_destructure_const) {
    setup();
    AstNode *stmt = parse_first("const (a, b) = pair");
    ASSERT_EQ_INT(stmt->type, AST_DESTRUCTURE_DECL);
    /* The is_const flag distinguishes `var` from `const`. */
    ASSERT_TRUE(stmt->as.destructure_decl.is_const);
    teardown();
}

TEST(parser_tuple_destructure_fn_param) {
    setup();
    /* `fn f((x, y): (int, int)) ...` — the parser hoists the
     * destructuring pattern off the param into a synthetic
     * `var (x, y) = __param0` at the head of the function body and
     * nulls out param->pattern. Verify the pattern landed on the body
     * with the right shape. */
    AstNode *stmt = parse_first("fn f((x, y): (i64, i64)) -> i64 { return x + y }");
    ASSERT_EQ_INT(stmt->type, AST_FUNCTION_DECL);
    ASSERT_EQ_INT(stmt->as.function_decl.param_count, 1);
    AstNode *body = stmt->as.function_decl.body;
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_INT(body->type, AST_BLOCK);
    ASSERT_TRUE(body->as.block.count >= 1);
    AstNode *first = body->as.block.statements[0];
    ASSERT_EQ_INT(first->type, AST_DESTRUCTURE_DECL);
    XrDestructurePattern *p = first->as.destructure_decl.pattern;
    ASSERT_EQ_INT(p->type, PATTERN_TUPLE);
    ASSERT_EQ_INT(p->as.array.element_count, 2);
    teardown();
}

TEST(parser_tuple_destructure_for_in) {
    setup();
    /* `for ((k, v) in pairs) { body }` desugars at parse time to a
     * for-in loop over a synthesised hidden iterator variable, with a
     * destructuring `var` injected at the top of the body block. */
    AstNode *stmt = parse_first("for ((k, v) in pairs) { print(k) }");
    ASSERT_EQ_INT(stmt->type, AST_FOR_IN_STMT);
    /* The injected destructure_decl is the first statement of the body. */
    AstNode *body = stmt->as.for_in_stmt.body;
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_INT(body->type, AST_BLOCK);
    ASSERT_TRUE(body->as.block.count >= 1);
    AstNode *first = body->as.block.statements[0];
    ASSERT_EQ_INT(first->type, AST_DESTRUCTURE_DECL);
    ASSERT_EQ_INT(first->as.destructure_decl.pattern->type, PATTERN_TUPLE);
    teardown();
}

TEST(parser_object_destructure_rename) {
    setup();
    AstNode *stmt = parse_first("var { name: localName, age } = user");
    ASSERT_EQ_INT(stmt->type, AST_DESTRUCTURE_DECL);
    XrDestructurePattern *p = stmt->as.destructure_decl.pattern;
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->type, PATTERN_OBJECT);
    ASSERT_EQ_INT(p->as.object.field_count, 2);
    ASSERT_STR_EQ(p->as.object.field_names[0], "name");
    ASSERT_EQ_INT(p->as.object.patterns[0]->type, PATTERN_IDENTIFIER);
    ASSERT_STR_EQ(p->as.object.patterns[0]->as.identifier.name, "localName");
    ASSERT_STR_EQ(p->as.object.field_names[1], "age");
    ASSERT_EQ_INT(p->as.object.patterns[1]->type, PATTERN_IDENTIFIER);
    ASSERT_STR_EQ(p->as.object.patterns[1]->as.identifier.name, "age");
    teardown();
}

TEST(parser_object_destructure_assign_rename) {
    setup();
    AstNode *stmt = parse_first("{ name: localName, age } = user");
    ASSERT_EQ_INT(stmt->type, AST_DESTRUCTURE_ASSIGN);
    XrDestructurePattern *p = stmt->as.destructure_assign.pattern;
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(p->type, PATTERN_OBJECT);
    ASSERT_EQ_INT(p->as.object.field_count, 2);
    ASSERT_STR_EQ(p->as.object.field_names[0], "name");
    ASSERT_EQ_INT(p->as.object.patterns[0]->type, PATTERN_IDENTIFIER);
    ASSERT_STR_EQ(p->as.object.patterns[0]->as.identifier.name, "localName");
    teardown();
}

TEST(parser_tuple_field_access_chained) {
    setup();
    /* `t.0.1` -- the lexer recognises that the second digit run starts
     * right after a member-access dot and refuses to extend it into a
     * float literal, so the chain tokenises as `t . 0 . 1`. */
    AstNode *expr = parse_expr("t.0.1");
    ASSERT_EQ_INT(expr->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(expr->as.member_access.name, "1");
    AstNode *inner = expr->as.member_access.object;
    ASSERT_EQ_INT(inner->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(inner->as.member_access.name, "0");
    teardown();
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    RUN_TEST_SUITE("Parser Tests");

    // Literals
    RUN_TEST(parser_int_literal);
    RUN_TEST(parser_float_literal);
    RUN_TEST(parser_string_literal);
    RUN_TEST(parser_block_string_normalizes_crlf_and_margin);
    RUN_TEST(parser_non_expression_string_consumers_use_shared_decoder);
    RUN_TEST(parser_block_object_key_and_coroutine_name_use_shared_decoder);
    RUN_TEST(parser_fixed_bytes_are_one_compact_payload_node);
    RUN_TEST(parser_c_literal_retains_nul_policy_without_element_ast);
    RUN_TEST(parser_q2_is_empty_for_every_prefix_family);
    RUN_TEST(parser_template_interpolation_skips_nested_variable_quote_block);
    RUN_TEST(parser_bool_literal);
    RUN_TEST(parser_null_literal);

    // Expressions
    RUN_TEST(parser_binary_add);
    RUN_TEST(parser_binary_precedence);
    RUN_TEST(parser_unary_neg);
    RUN_TEST(parser_unary_not);
    RUN_TEST(parser_grouping);
    RUN_TEST(parser_comparison);
    RUN_TEST(parser_logical_and_or);

    // Variable declarations
    RUN_TEST(parser_let_decl);
    RUN_TEST(parser_const_decl);

    // Control flow
    RUN_TEST(parser_if_stmt);
    RUN_TEST(parser_if_else);
    RUN_TEST(parser_while_stmt);
    RUN_TEST(parser_for_stmt);
    RUN_TEST(parser_select_wildcard_default_arm);
    RUN_TEST(parser_select_channel_arrow_remains_arm_delimiter);
    RUN_TEST(parser_select_default_keyword_rejected);

    // Functions
    RUN_TEST(parser_function_decl);
    RUN_TEST(parser_const_type_qualifier_is_canonical_in_every_position);
    RUN_TEST(parser_extern_block_flattens_typed_descriptors);
    RUN_TEST(parser_extern_block_rejects_layout_declarations);
    RUN_TEST(parser_parameter_modes_share_annotation_parser);
    RUN_TEST(parser_oop_parameter_modes_share_annotation_parser);
    RUN_TEST(parser_function_type_param_modes);
    RUN_TEST(parser_function_no_params);
    RUN_TEST(parser_return_stmt);

    // Collections
    RUN_TEST(parser_array_literal);
    RUN_TEST(parser_index_get);
    RUN_TEST(parser_object_literal);

    // Classes
    RUN_TEST(parser_class_decl);
    RUN_TEST(parser_direct_visibility_owns_declaration);
    RUN_TEST(parser_method_deprecated_attribute);
    RUN_TEST(parser_inline_control_attributes_and_conflict);
    RUN_TEST(parser_rejects_posthoc_local_export);
    RUN_TEST(parser_reexport_preserves_module_identity_kind);
    RUN_TEST(parser_enum_static_method);

    // Calls
    RUN_TEST(parser_call_expr);
    RUN_TEST(parser_call_arg_access_markers_only_in_direct_argument_slot);
    RUN_TEST(parser_member_access);
    RUN_TEST(parser_member_generic_call_uintsize_type_arg);
    RUN_TEST(parser_member_generic_call_uintsize_after_binary_op);
    RUN_TEST(parser_scalar_spelling_registry_roundtrip);
    RUN_TEST(parser_retired_scalar_spelling_is_ordinary_identifier);

    // Tuples
    RUN_TEST(parser_tuple_unit_literal);
    RUN_TEST(parser_tuple_unary_literal);
    RUN_TEST(parser_tuple_multi_literal);
    RUN_TEST(parser_tuple_nested_literal);
    RUN_TEST(parser_tuple_with_trailing_comma);
    RUN_TEST(parser_tuple_type_over_16_elements);
    RUN_TEST(parser_grouping_still_works);
    RUN_TEST(parser_arrow_fn_not_tuple);
    RUN_TEST(parser_arrow_parameters_support_independent_annotations_and_modes);
    RUN_TEST(parser_bare_lambda_in_expression_position);
    RUN_TEST(parser_tuple_field_access);
    RUN_TEST(parser_tuple_field_access_chained);
    RUN_TEST(parser_tuple_destructure_let);
    RUN_TEST(parser_tuple_destructure_with_skip);
    RUN_TEST(parser_tuple_destructure_const);
    RUN_TEST(parser_tuple_destructure_fn_param);
    RUN_TEST(parser_tuple_destructure_for_in);
    RUN_TEST(parser_object_destructure_rename);
    RUN_TEST(parser_object_destructure_assign_rename);

    // Error handling
    RUN_TEST(parser_error_returns_null);
    RUN_TEST(parser_rejects_retired_storage_declarations);
    RUN_TEST(parser_rejects_move_const_parameter_capability);
    RUN_TEST(parser_empty_source);
    RUN_TEST(parser_multiple_stmts);

    TEST_REPORT();
    return TEST_EXIT();
}
