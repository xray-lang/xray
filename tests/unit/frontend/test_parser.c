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
#include "xray.h"
#include "runtime/xisolate_internal.h"

/* ========== Test Infrastructure ========== */

static XrayIsolate *X = NULL;

static void setup(void) {
    X = xray_isolate_new(NULL);
    ASSERT_NOT_NULL(X);
}

static void teardown(void) {
    if (X) {
        xray_isolate_delete(X);
        X = NULL;
    }
}

/* Helper: parse source and assert success */
static AstNode *parse_ok(const char *source) {
    AstNode *ast = xr_parse(X, source);
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

/* ========== Literal Tests ========== */

TEST(parser_int_literal) {
    setup();
    AstNode *stmt = parse_first("42");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_INT);
    ASSERT_EQ_INT((int) expr->as.literal.raw_value.int_val, 42);
    teardown();
}

TEST(parser_float_literal) {
    setup();
    AstNode *stmt = parse_first("3.14");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_FLOAT);
    ASSERT_TRUE(expr->as.literal.raw_value.float_val > 3.13 &&
                expr->as.literal.raw_value.float_val < 3.15);
    teardown();
}

TEST(parser_string_literal) {
    setup();
    AstNode *stmt = parse_first("\"hello\"");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_STRING);
    ASSERT_STR_EQ(expr->as.literal.raw_value.string_val, "hello");
    teardown();
}

TEST(parser_bool_literal) {
    setup();
    AstNode *stmt = parse_first("true");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_TRUE);
    teardown();
}

TEST(parser_null_literal) {
    setup();
    AstNode *stmt = parse_first("null");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_LITERAL_NULL);
    teardown();
}

/* ========== Expression Tests ========== */

TEST(parser_binary_add) {
    setup();
    AstNode *stmt = parse_first("1 + 2");
    ASSERT_EQ_INT(stmt->type, AST_EXPR_STMT);
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_LITERAL_INT);
    ASSERT_EQ_INT(expr->as.binary.right->type, AST_LITERAL_INT);
    teardown();
}

TEST(parser_binary_precedence) {
    setup();
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    AstNode *stmt = parse_first("1 + 2 * 3");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_BINARY_ADD);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_LITERAL_INT);
    ASSERT_EQ_INT(expr->as.binary.right->type, AST_BINARY_MUL);
    teardown();
}

TEST(parser_unary_neg) {
    setup();
    AstNode *stmt = parse_first("-42");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_UNARY_NEG);
    ASSERT_EQ_INT(expr->as.unary.operand->type, AST_LITERAL_INT);
    teardown();
}

TEST(parser_unary_not) {
    setup();
    AstNode *stmt = parse_first("!true");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_UNARY_NOT);
    ASSERT_EQ_INT(expr->as.unary.operand->type, AST_LITERAL_TRUE);
    teardown();
}

TEST(parser_grouping) {
    setup();
    // (1 + 2) * 3 should parse as (1+2) * 3
    AstNode *stmt = parse_first("(1 + 2) * 3");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_BINARY_MUL);
    // left should be grouping containing add
    AstNode *left = expr->as.binary.left;
    ASSERT_EQ_INT(left->type, AST_GROUPING);
    ASSERT_EQ_INT(left->as.grouping->type, AST_BINARY_ADD);
    teardown();
}

TEST(parser_comparison) {
    setup();
    AstNode *stmt = parse_first("a == b");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_BINARY_EQ);
    teardown();
}

TEST(parser_logical_and_or) {
    setup();
    // a && b || c should parse as (a && b) || c
    AstNode *stmt = parse_first("a && b || c");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_BINARY_OR);
    ASSERT_EQ_INT(expr->as.binary.left->type, AST_BINARY_AND);
    teardown();
}

/* ========== Variable Declaration Tests ========== */

TEST(parser_let_decl) {
    setup();
    AstNode *stmt = parse_first("let x = 10");
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
    AstNode *stmt = parse_first("for (let i = 0; i < 10; i++) {\n  print(i)\n}");
    ASSERT_EQ_INT(stmt->type, AST_FOR_STMT);
    ASSERT_NOT_NULL(stmt->as.for_stmt.initializer);
    ASSERT_NOT_NULL(stmt->as.for_stmt.condition);
    ASSERT_NOT_NULL(stmt->as.for_stmt.increment);
    ASSERT_NOT_NULL(stmt->as.for_stmt.body);
    teardown();
}

/* ========== Function Tests ========== */

TEST(parser_function_decl) {
    setup();
    AstNode *stmt = parse_first("fn add(a: int, b: int): int {\n  return a + b\n}");
    ASSERT_EQ_INT(stmt->type, AST_FUNCTION_DECL);
    ASSERT_STR_EQ(stmt->as.function_decl.name, "add");
    ASSERT_EQ_INT(stmt->as.function_decl.param_count, 2);
    ASSERT_NOT_NULL(stmt->as.function_decl.body);
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
    AstNode *program = parse_ok("fn f(): int {\n  return 42\n}");
    AstNode *fn = first_stmt(program);
    ASSERT_EQ_INT(fn->type, AST_FUNCTION_DECL);
    // body is a block
    AstNode *body = fn->as.function_decl.body;
    ASSERT_EQ_INT(body->type, AST_BLOCK);
    ASSERT_TRUE(body->as.block.count > 0);
    AstNode *ret = body->as.block.statements[0];
    ASSERT_EQ_INT(ret->type, AST_RETURN_STMT);
    teardown();
}

/* ========== Array Tests ========== */

TEST(parser_array_literal) {
    setup();
    AstNode *stmt = parse_first("[1, 2, 3]");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_ARRAY_LITERAL);
    ASSERT_EQ_INT(expr->as.array_literal.count, 3);
    teardown();
}

TEST(parser_index_get) {
    setup();
    AstNode *stmt = parse_first("arr[0]");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_INDEX_GET);
    teardown();
}

/* ========== Object/Map Tests ========== */

TEST(parser_object_literal) {
    setup();
    // Object literal in assignment context (bare {} is parsed as block)
    AstNode *stmt = parse_first("let obj = {a: 1, b: 2}");
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

/* ========== Error Handling Tests ========== */

TEST(parser_error_returns_null) {
    setup();
    // Unclosed brace should cause parse error
    AstNode *ast = xr_parse(X, "fn f() {");
    ASSERT_TRUE(ast == NULL);
    teardown();
}

TEST(parser_empty_source) {
    setup();
    AstNode *ast = xr_parse(X, "");
    ASSERT_NOT_NULL(ast);
    ASSERT_EQ_INT(ast->type, AST_PROGRAM);
    ASSERT_EQ_INT(ast->as.program.count, 0);
    xr_program_destroy(ast);
    teardown();
}

/* ========== Multiple Statement Tests ========== */

TEST(parser_multiple_stmts) {
    setup();
    AstNode *program = parse_ok("let x = 1\n"
                                "let y = 2\n"
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
    teardown();
}

TEST(parser_member_access) {
    setup();
    AstNode *stmt = parse_first("obj.field");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(expr->as.member_access.name, "field");
    teardown();
}

/* ========== Tuple Tests ========== */

TEST(parser_tuple_unit_literal) {
    setup();
    /* `()` is the unit literal: a 0-arity AST_TUPLE_LITERAL. */
    AstNode *stmt = parse_first("let u = ()");
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
    AstNode *stmt = parse_first("let u = (42,)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 1);
    teardown();
}

TEST(parser_tuple_multi_literal) {
    setup();
    AstNode *stmt = parse_first("let t = (1, \"hi\", true)");
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
    AstNode *stmt = parse_first("let n = ((1, 2), (3, 4))");
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
    AstNode *stmt = parse_first("let t = (1, 2, 3,)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_TUPLE_LITERAL);
    ASSERT_EQ_INT(init->as.tuple_literal.count, 3);
    teardown();
}

TEST(parser_grouping_still_works) {
    setup();
    /* `(x)` without a trailing comma must remain a grouping, otherwise
     * every existing parenthesised expression would silently turn into
     * a 1-tuple. */
    AstNode *stmt = parse_first("let g = (1 + 2)");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_GROUPING);
    teardown();
}

TEST(parser_arrow_fn_not_tuple) {
    setup();
    /* `(a, b) => a + b` must still parse as a function expression,
     * not as a 2-tuple of variable references followed by `=>`. */
    AstNode *stmt = parse_first("let f = (a, b) => a + b");
    AstNode *init = stmt->as.var_decl.initializer;
    ASSERT_EQ_INT(init->type, AST_FUNCTION_EXPR);
    teardown();
}

TEST(parser_tuple_field_access) {
    setup();
    /* `t.0` parses as AST_MEMBER_ACCESS with member name "0" -- the
     * analyzer recognises digit-only names on tuple receivers. */
    AstNode *stmt = parse_first("t.0");
    AstNode *expr = stmt->as.expr_stmt;
    ASSERT_EQ_INT(expr->type, AST_MEMBER_ACCESS);
    ASSERT_STR_EQ(expr->as.member_access.name, "0");
    teardown();
}

TEST(parser_tuple_destructure_let) {
    setup();
    /* `let (a, b) = pair` — produces AST_DESTRUCTURE_DECL whose
     * pattern is PATTERN_TUPLE with two identifier sub-patterns. */
    AstNode *stmt = parse_first("let (a, b) = pair");
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
    AstNode *stmt = parse_first("let (x, _, z) = triple");
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
    /* The is_const flag distinguishes `let` from `const`. */
    ASSERT_TRUE(stmt->as.destructure_decl.is_const);
    teardown();
}

TEST(parser_tuple_destructure_fn_param) {
    setup();
    /* `fn f((x, y): (int, int)) ...` — the parser hoists the
     * destructuring pattern off the param into a synthetic
     * `let (x, y) = __param0` at the head of the function body and
     * nulls out param->pattern. Verify the pattern landed on the body
     * with the right shape. */
    AstNode *stmt = parse_first("fn f((x, y): (int, int)): int { return x + y }");
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
     * destructuring `let` injected at the top of the body block. */
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

TEST(parser_tuple_field_access_chained) {
    setup();
    /* `t.0.1` -- the lexer recognises that the second digit run starts
     * right after a member-access dot and refuses to extend it into a
     * float literal, so the chain tokenises as `t . 0 . 1`. */
    AstNode *stmt = parse_first("t.0.1");
    AstNode *expr = stmt->as.expr_stmt;
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

    // Functions
    RUN_TEST(parser_function_decl);
    RUN_TEST(parser_function_no_params);
    RUN_TEST(parser_return_stmt);

    // Collections
    RUN_TEST(parser_array_literal);
    RUN_TEST(parser_index_get);
    RUN_TEST(parser_object_literal);

    // Classes
    RUN_TEST(parser_class_decl);

    // Calls
    RUN_TEST(parser_call_expr);
    RUN_TEST(parser_member_access);

    // Tuples
    RUN_TEST(parser_tuple_unit_literal);
    RUN_TEST(parser_tuple_unary_literal);
    RUN_TEST(parser_tuple_multi_literal);
    RUN_TEST(parser_tuple_nested_literal);
    RUN_TEST(parser_tuple_with_trailing_comma);
    RUN_TEST(parser_grouping_still_works);
    RUN_TEST(parser_arrow_fn_not_tuple);
    RUN_TEST(parser_tuple_field_access);
    RUN_TEST(parser_tuple_field_access_chained);
    RUN_TEST(parser_tuple_destructure_let);
    RUN_TEST(parser_tuple_destructure_with_skip);
    RUN_TEST(parser_tuple_destructure_const);
    RUN_TEST(parser_tuple_destructure_fn_param);
    RUN_TEST(parser_tuple_destructure_for_in);

    // Error handling
    RUN_TEST(parser_error_returns_null);
    RUN_TEST(parser_empty_source);
    RUN_TEST(parser_multiple_stmts);

    TEST_REPORT();
    return TEST_EXIT();
}
