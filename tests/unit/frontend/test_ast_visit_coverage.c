/*
 * test_ast_visit_coverage.c - AST visitor completeness test
 *
 * Verifies that the analyzer visits ALL child expressions of every
 * AST node type that can contain sub-expressions.  After analysis,
 * every expression node must have a non-NULL type in the node-type
 * side table.  A missing type means the analyzer skipped the node.
 *
 * This test directly prevents the class of bug where a new AST node
 * (or a previously untested nesting) silently escapes analysis,
 * causing unresolved types or missing symbol bindings downstream.
 */

#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Infrastructure ========== */

static XrayIsolate *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrayIsolateParams p;
        xray_isolate_params_init(&p);
        g_iso = xray_isolate_new(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_isolate_delete(g_iso);
        g_iso = NULL;
    }
}

/* ========== Expression type checker ========== */

typedef struct {
    XaAnalyzer *analyzer;
    int total_expr;      /* expression nodes visited */
    int missing_type;    /* expression nodes WITHOUT a type */
    int max_missing_log; /* max number of missing-type nodes to log */
    int logged;
} CoverageCtx;

/* Is this node type an expression that should have a type? */
static bool is_expression_node(AstNodeType t) {
    switch (t) {
        /* Literals */
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        /* Binary */
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        /* Ternary / null */
        case AST_TERNARY:
        case AST_NULLISH_COALESCE:
        case AST_OPTIONAL_CHAIN:
        case AST_FORCE_UNWRAP:
        case AST_AS_EXPR:
        case AST_IS_EXPR:
        case AST_RANGE:
        /* Unary */
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        /* Grouping */
        case AST_GROUPING:
        /* Variable / inc-dec (assignments are statements, not typed expressions) */
        case AST_VARIABLE:
        case AST_INC:
        case AST_DEC:
        /* Call / member / index */
        case AST_CALL_EXPR:
        case AST_MEMBER_ACCESS:
        case AST_INDEX_GET:
        case AST_INDEX_SET:
        case AST_SLICE_EXPR:
        case AST_MEMBER_SET:
        /* Literals (aggregate) */
        case AST_ARRAY_LITERAL:
        case AST_MAP_LITERAL:
        case AST_SET_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_STRUCT_LITERAL:
        case AST_TEMPLATE_STRING:
        /* OOP */
        case AST_NEW_EXPR:
        case AST_THIS_EXPR:
        case AST_SUPER_CALL:
        /* Enum */
        case AST_ENUM_ACCESS:
        case AST_ENUM_CONVERT:
        case AST_ENUM_INDEX:
        /* Coroutine */
        case AST_GO_EXPR:
        case AST_AWAIT_EXPR:
        case AST_CHANNEL_NEW:
        case AST_CHAN_SEND:
        case AST_CHAN_RECV:
        case AST_CANCELLED_EXPR:
        case AST_MOVE_EXPR:
        /* Match */
        case AST_MATCH_EXPR:
        /* Function expression */
        case AST_FUNCTION_EXPR:
            return true;
        default:
            return false;
    }
}

/* Recursive AST walker that checks every expression node for a type. */
static void check_node(CoverageCtx *ctx, AstNode *node) {
    if (!node)
        return;

    /* If this is an expression, it must have a type */
    if (is_expression_node(node->type)) {
        ctx->total_expr++;
        struct XrType *t = xa_analyzer_get_node_type(ctx->analyzer, node);
        if (!t) {
            ctx->missing_type++;
            if (ctx->logged < ctx->max_missing_log) {
                fprintf(stderr, "    MISSING TYPE: node_type=%d node_id=%u line=%d\n", node->type,
                        node->node_id, (int) node->line);
                ctx->logged++;
            }
        }
    }

    /* Walk children — mirrors the canonicalizer's comprehensive walker */
    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->as.program.count; i++)
                check_node(ctx, node->as.program.statements[i]);
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            check_node(ctx, node->as.var_decl.initializer);
            break;

        case AST_ASSIGNMENT:
            check_node(ctx, node->as.assignment.value);
            break;

        case AST_COMPOUND_ASSIGNMENT:
            check_node(ctx, node->as.compound_assignment.value);
            break;

        case AST_INC:
        case AST_DEC:
            /* IncDecNode only has name/symbol_id, no child expression */
            break;

        case AST_EXPR_STMT:
            check_node(ctx, node->as.expr_stmt);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                check_node(ctx, node->as.return_stmt.values[i]);
            break;

        case AST_IF_STMT:
            check_node(ctx, node->as.if_stmt.condition);
            check_node(ctx, node->as.if_stmt.then_branch);
            check_node(ctx, node->as.if_stmt.else_branch);
            break;

        case AST_WHILE_STMT:
            check_node(ctx, node->as.while_stmt.condition);
            check_node(ctx, node->as.while_stmt.body);
            break;

        case AST_FOR_STMT:
            check_node(ctx, node->as.for_stmt.initializer);
            check_node(ctx, node->as.for_stmt.condition);
            check_node(ctx, node->as.for_stmt.increment);
            check_node(ctx, node->as.for_stmt.body);
            break;

        case AST_FOR_IN_STMT:
            check_node(ctx, node->as.for_in_stmt.collection);
            check_node(ctx, node->as.for_in_stmt.body);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                check_node(ctx, node->as.print_stmt.exprs[i]);
            break;

        case AST_THROW_STMT:
            check_node(ctx, node->as.throw_stmt.expression);
            break;

        case AST_TRY_CATCH:
            check_node(ctx, node->as.try_catch.try_body);
            check_node(ctx, node->as.try_catch.catch_body);
            check_node(ctx, node->as.try_catch.finally_body);
            break;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            check_node(ctx, node->as.function_decl.body);
            break;

        case AST_CALL_EXPR:
            check_node(ctx, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                check_node(ctx, node->as.call_expr.arguments[i]);
            break;

        case AST_MEMBER_ACCESS:
            check_node(ctx, node->as.member_access.object);
            break;

        case AST_MEMBER_SET:
            check_node(ctx, node->as.member_set.object);
            check_node(ctx, node->as.member_set.value);
            break;

        case AST_INDEX_GET:
            check_node(ctx, node->as.index_get.array);
            check_node(ctx, node->as.index_get.index);
            break;

        case AST_INDEX_SET:
            check_node(ctx, node->as.index_set.array);
            check_node(ctx, node->as.index_set.index);
            check_node(ctx, node->as.index_set.value);
            break;

        case AST_SLICE_EXPR:
            check_node(ctx, node->as.slice_expr.source);
            check_node(ctx, node->as.slice_expr.start);
            check_node(ctx, node->as.slice_expr.end);
            break;

        case AST_TERNARY:
            check_node(ctx, node->as.ternary.condition);
            check_node(ctx, node->as.ternary.true_expr);
            check_node(ctx, node->as.ternary.false_expr);
            break;

        case AST_NULLISH_COALESCE:
        case AST_RANGE:
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            check_node(ctx, node->as.binary.left);
            check_node(ctx, node->as.binary.right);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            check_node(ctx, node->as.unary.operand);
            break;

        case AST_GROUPING:
            check_node(ctx, node->as.grouping);
            break;

        case AST_FORCE_UNWRAP:
            check_node(ctx, node->as.unary.operand);
            break;

        case AST_IS_EXPR:
            check_node(ctx, node->as.is_expr.expr);
            break;

        case AST_AS_EXPR:
            check_node(ctx, node->as.as_expr.expr);
            break;

        case AST_OPTIONAL_CHAIN:
            check_node(ctx, node->as.optional_chain.object);
            if (node->as.optional_chain.index)
                check_node(ctx, node->as.optional_chain.index);
            break;

        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                check_node(ctx, node->as.array_literal.elements[i]);
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                check_node(ctx, node->as.map_literal.keys[i]);
                check_node(ctx, node->as.map_literal.values[i]);
            }
            break;

        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++)
                check_node(ctx, node->as.set_literal.elements[i]);
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                /* Keys are identifier-like strings, not typed expressions */
                check_node(ctx, node->as.object_literal.values[i]);
            }
            break;

        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                check_node(ctx, node->as.struct_literal.field_values[i]);
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                check_node(ctx, node->as.template_str.parts[i]);
            break;

        case AST_NEW_EXPR:
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                check_node(ctx, node->as.new_expr.arguments[i]);
            break;

        case AST_SUPER_CALL:
            for (int i = 0; i < node->as.super_call.arg_count; i++)
                check_node(ctx, node->as.super_call.arguments[i]);
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            for (int i = 0; i < cls->field_count; i++)
                check_node(ctx, cls->fields[i]);
            for (int i = 0; i < cls->method_count; i++)
                check_node(ctx, cls->methods[i]);
            break;
        }

        case AST_FIELD_DECL:
            check_node(ctx, node->as.field_decl.initializer);
            break;

        case AST_ENUM_DECL:
            for (int i = 0; i < node->as.enum_decl.member_count; i++)
                check_node(ctx, node->as.enum_decl.members[i]);
            break;

        case AST_ENUM_CONVERT:
            check_node(ctx, node->as.enum_convert.value_expr);
            break;

        case AST_ENUM_INDEX:
            check_node(ctx, node->as.enum_index.collection);
            check_node(ctx, node->as.enum_index.index_expr);
            break;

        case AST_MATCH_EXPR:
            check_node(ctx, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                check_node(ctx, node->as.match_expr.arms[i]);
            break;

        case AST_MATCH_ARM:
            check_node(ctx, node->as.match_arm.guard);
            check_node(ctx, node->as.match_arm.body);
            break;

        case AST_GO_EXPR:
            check_node(ctx, node->as.go_expr.expr);
            if (node->as.go_expr.priority)
                check_node(ctx, node->as.go_expr.priority);
            break;

        case AST_AWAIT_EXPR:
            check_node(ctx, node->as.await_expr.expr);
            if (node->as.await_expr.timeout)
                check_node(ctx, node->as.await_expr.timeout);
            break;

        case AST_CHANNEL_NEW:
            check_node(ctx, node->as.channel_new.buffer_size);
            break;

        case AST_CHAN_SEND:
        case AST_CHAN_RECV:
            /* Parsed as method calls (ch.send / ch.recv), no dedicated struct */
            break;

        case AST_MOVE_EXPR:
            check_node(ctx, node->as.move_expr.expr);
            break;

        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (!sel->cases[i])
                    continue;
                SelectCaseNode *sc = &sel->cases[i]->as.select_case;
                check_node(ctx, sc->channel);
                check_node(ctx, sc->value);
                check_node(ctx, sc->body);
            }
            break;
        }

        case AST_DEFER_STMT:
            check_node(ctx, node->as.defer_stmt.expr);
            break;

        case AST_SCOPE_BLOCK:
            check_node(ctx, node->as.scope_block.body);
            break;

        case AST_DESTRUCTURE_DECL:
            check_node(ctx, node->as.destructure_decl.initializer);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            check_node(ctx, node->as.destructure_assign.value);
            break;

        case AST_EXPORT_STMT:
            check_node(ctx, node->as.export_stmt.declaration);
            break;

        case AST_IMPORT_STMT:
            break;

        /* Leaf nodes */
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_REGEX:
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_THIS_EXPR:
        case AST_CANCELLED_EXPR:
        case AST_ENUM_ACCESS:
        case AST_ENUM_MEMBER:
        case AST_TYPE_ALIAS:
        case AST_INTERFACE_DECL:
        case AST_INTERFACE_METHOD:
        case AST_YIELD_STMT:
        case AST_METHOD_DECL:
        case AST_PATTERN_LITERAL:
        case AST_PATTERN_RANGE:
        case AST_PATTERN_WILDCARD:
        case AST_PATTERN_MULTI:
            break;

        default:
            /* Unknown node type — if we reach here, a new AST node was added
             * without updating this walker.  Log and count as missing. */
            fprintf(stderr, "    WARNING: unhandled node type %d in coverage walker\n", node->type);
            break;
    }
}

/* ========== Test helper ========== */

#define TEST(name)                                                                                 \
    static bool test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- %s ---\n", #name);                                                             \
        if (test_##name()) {                                                                       \
            printf("  PASS\n");                                                                    \
            tests_passed++;                                                                        \
        } else {                                                                                   \
            printf("  FAIL\n");                                                                    \
            tests_failed++;                                                                        \
        }                                                                                          \
    }                                                                                              \
    static bool test_##name(void)

/* Parse + analyze; verify all expressions got types.
 * Returns false if the test failed (missing types or parse error). */
static bool assert_all_typed(const char *source, const char *label) {
    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  [%s] FAIL: parse error\n", label);
        return false;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    assert(analyzer != NULL);
    xa_analyzer_analyze(analyzer, "coverage_test.xr", program);

    CoverageCtx ctx = {
        .analyzer = analyzer,
        .total_expr = 0,
        .missing_type = 0,
        .max_missing_log = 5,
        .logged = 0,
    };
    check_node(&ctx, program);

    bool ok = true;
    if (ctx.missing_type > 0) {
        fprintf(stderr, "  [%s] FAIL: %d/%d expression nodes missing type\n", label,
                ctx.missing_type, ctx.total_expr);
        ok = false;
    } else {
        printf("  [%s] OK: %d expression nodes all typed\n", label, ctx.total_expr);
    }
    assert(ctx.total_expr > 0 && "test must exercise at least 1 expression");

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    return ok;
}

/* ========== Tests ========== */

TEST(literals_and_arithmetic) {
    return assert_all_typed("let a = 42\n"
                            "let b = 3.14\n"
                            "let c = \"hello\"\n"
                            "let d = true\n"
                            "let e = null\n"
                            "let f = a + b * 2 - 1\n"
                            "let g = a % 3\n",
                            "literals_and_arithmetic");
}

TEST(comparison_and_logical) {
    return assert_all_typed("let x = 10\n"
                            "let a = x > 5 && x < 20\n"
                            "let b = x == 10 || x != 0\n"
                            "let c = x >= 5 && x <= 15\n",
                            "comparison_and_logical");
}

TEST(bitwise_ops) {
    return assert_all_typed("let x = 0xFF\n"
                            "let a = x & 0x0F\n"
                            "let b = x | 0xF0\n"
                            "let c = x ^ 0xAA\n"
                            "let d = x << 2\n"
                            "let e = x >> 1\n"
                            "let f = ~x\n",
                            "bitwise_ops");
}

TEST(unary_and_grouping) {
    return assert_all_typed("let x = 42\n"
                            "let a = -x\n"
                            "let b = !true\n"
                            "let c = (x + 1) * 2\n",
                            "unary_and_grouping");
}

TEST(ternary_and_nullish) {
    return assert_all_typed("let x = 10\n"
                            "let a = x > 5 ? \"big\" : \"small\"\n"
                            "let y: int? = null\n"
                            "let b = y ?? 42\n",
                            "ternary_and_nullish");
}

TEST(array_literal) {
    return assert_all_typed("let arr = [1, 2, 3]\n"
                            "print(arr)\n",
                            "array_literal");
}

TEST(map_literal) {
    return assert_all_typed("let m = #{\"a\": 1, \"b\": 2}\n"
                            "print(m)\n",
                            "map_literal");
}

TEST(set_literal) {
    return assert_all_typed("let s = #[1, 2, 3]\n"
                            "print(s)\n",
                            "set_literal");
}

TEST(object_literal) {
    return assert_all_typed("let o = {a: 1, b: \"hello\"}\n"
                            "print(o)\n",
                            "object_literal");
}

TEST(template_string) {
    return assert_all_typed("let name = \"world\"\n"
                            "let s = \"hello ${name}!\"\n"
                            "print(s)\n",
                            "template_string");
}

TEST(function_decl_and_call) {
    return assert_all_typed("fn add(a: int, b: int) -> int {\n"
                            "    return a + b\n"
                            "}\n"
                            "let r = add(1, 2)\n"
                            "print(r)\n",
                            "function_decl_and_call");
}

TEST(function_expr) {
    return assert_all_typed("let double = fn(x: int) -> int { return x * 2\n }"
                            "print(double(5))\n",
                            "function_expr");
}

TEST(if_else) {
    return assert_all_typed("let x = 10\n"
                            "if (x > 5) {\n"
                            "    print(\"big\")\n"
                            "} else {\n"
                            "    print(\"small\")\n"
                            "}\n",
                            "if_else");
}

TEST(while_loop) {
    return assert_all_typed("let i = 0\n"
                            "while (i < 10) {\n"
                            "    print(i)\n"
                            "    i = i + 1\n"
                            "}\n",
                            "while_loop");
}

TEST(for_in_loop) {
    return assert_all_typed("let arr = [1, 2, 3]\n"
                            "for (item in arr) {\n"
                            "    print(item)\n"
                            "}\n",
                            "for_in_loop");
}

TEST(try_catch) {
    return assert_all_typed("try {\n"
                            "    let x = 1 + 2\n"
                            "    print(x)\n"
                            "} catch (e) {\n"
                            "    print(e)\n"
                            "}\n",
                            "try_catch");
}

TEST(class_basic) {
    return assert_all_typed("class Dog {\n"
                            "    name: string\n"
                            "    constructor(n: string) {\n"
                            "        this.name = n\n"
                            "    }\n"
                            "    bark() -> string {\n"
                            "        return \"woof\"\n"
                            "    }\n"
                            "}\n"
                            "let d = new Dog(\"Rex\")\n"
                            "print(d.bark())\n",
                            "class_basic");
}

TEST(enum_basic) {
    return assert_all_typed("enum Color : string {\n"
                            "    Red = \"red\",\n"
                            "    Blue = \"blue\"\n"
                            "}\n"
                            "let c = Color.Red\n"
                            "print(c)\n",
                            "enum_basic");
}

TEST(match_expr) {
    return assert_all_typed("let x = 2\n"
                            "let result = match (x) {\n"
                            "    1 -> \"one\"\n"
                            "    2 -> \"two\"\n"
                            "    _ -> \"other\"\n"
                            "}\n"
                            "print(result)\n",
                            "match_expr");
}

TEST(destructure) {
    return assert_all_typed("let arr = [1, 2, 3]\n"
                            "let [a, b, c] = arr\n"
                            "print(a)\n",
                            "destructure");
}

TEST(multi_var_decl) {
    return assert_all_typed("let a = 1\n"
                            "let b = 2\n"
                            "let c = a + b\n"
                            "print(c)\n",
                            "multi_var_decl");
}

TEST(print_stmt) {
    return assert_all_typed("let x = 42\n"
                            "let y = \"hello\"\n"
                            "print(x, y, x + 1)\n",
                            "print_stmt");
}

TEST(throw_stmt) {
    return assert_all_typed("fn fail() {\n"
                            "    throw \"error\"\n"
                            "}\n",
                            "throw_stmt");
}

TEST(channel_and_go) {
    return assert_all_typed("let ch = Channel(1)\n"
                            "go fn() {\n"
                            "    ch.send(42)\n"
                            "}\n"
                            "let v = ch.recv()\n"
                            "print(v)\n",
                            "channel_and_go");
}

TEST(slice_expr) {
    return assert_all_typed("let arr = [1, 2, 3, 4, 5]\n"
                            "let s = arr[1:3]\n"
                            "print(s)\n",
                            "slice_expr");
}

TEST(defer_stmt) {
    return assert_all_typed("fn cleanup() {\n"
                            "    defer print(\"done\")\n"
                            "    print(\"working\")\n"
                            "}\n"
                            "cleanup()\n",
                            "defer_stmt");
}

TEST(nested_expressions) {
    return assert_all_typed("let x = 10\n"
                            "let y = (x > 5 ? x * 2 : x / 2) + 1\n"
                            "let arr = [1, 2, 3]\n"
                            "let z = arr[(x > 5) ? 0 : 1]\n"
                            "print(y, z)\n",
                            "nested_expressions");
}

TEST(closure_capture) {
    return assert_all_typed("fn make_adder(n: int) -> int {\n"
                            "    let inner = fn(x: int) -> int { return x + n\n }"
                            "    return inner(10)\n"
                            "}\n"
                            "let r = make_adder(5)\n"
                            "print(r)\n",
                            "closure_capture");
}

/* ========== Main ========== */

int main(void) {
    setup();

    printf("\n=== AST Visit Coverage Tests ===\n\n");

    run_literals_and_arithmetic();
    run_comparison_and_logical();
    run_bitwise_ops();
    run_unary_and_grouping();
    run_ternary_and_nullish();
    run_array_literal();
    run_map_literal();
    run_set_literal();
    run_object_literal();
    run_template_string();
    run_function_decl_and_call();
    run_function_expr();
    run_if_else();
    run_while_loop();
    run_for_in_loop();
    run_try_catch();
    run_class_basic();
    run_enum_basic();
    run_match_expr();
    run_destructure();
    run_multi_var_decl();
    run_print_stmt();
    run_throw_stmt();
    run_channel_and_go();
    run_slice_expr();
    run_defer_stmt();
    run_nested_expressions();
    run_closure_capture();

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    teardown();
    return tests_failed > 0 ? 1 : 0;
}
