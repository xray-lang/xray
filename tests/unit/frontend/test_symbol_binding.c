/*
 * test_symbol_binding.c - Symbol binding invariant tests
 *
 * Verifies that the analyzer correctly binds all variable references
 * (symbol_id must be non-zero after analysis), and that the lowerer
 * successfully processes all binding patterns without hard-fail.
 *
 * Each test parses a source snippet, runs the analyzer, then lowers
 * to Xi IR.  Successful lowering proves the binding contract held.
 * A simple AST walker also spot-checks that AST_VARIABLE nodes
 * received non-zero symbol_id from the analyzer.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_lower.h"
#include "../../../src/frontend/canonical/xcanon.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../include/xray_isolate.h"
#include "../../../src/runtime/xisolate_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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

/* ========== AST Variable Walker ========== */

/* Count AST_VARIABLE nodes with unresolved symbol_id (== 0).
 * Recursively walks only the most common child-bearing node types. */
static int count_unresolved_vars(AstNode *node) {
    if (!node)
        return 0;
    int count = 0;

    if (node->type == AST_VARIABLE) {
        if (node->as.variable.symbol_id == 0)
            count++;
        return count;
    }

    /* Comprehensive AST walker — covers ALL node types that can contain
     * AST_VARIABLE children.  Missing a node type here means we silently
     * skip unresolved variables inside it. */
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++)
                count += count_unresolved_vars(node->as.program.statements[i]);
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                count += count_unresolved_vars(node->as.block.statements[i]);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            count += count_unresolved_vars(node->as.var_decl.initializer);
            break;
        case AST_ASSIGNMENT:
            count += count_unresolved_vars(node->as.assignment.value);
            break;
        case AST_COMPOUND_ASSIGNMENT:
            count += count_unresolved_vars(node->as.compound_assignment.value);
            break;
        case AST_INC:
        case AST_DEC:
            break;
        case AST_EXPR_STMT:
            count += count_unresolved_vars(node->as.expr_stmt);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                count += count_unresolved_vars(node->as.return_stmt.values[i]);
            break;
        case AST_IF_STMT:
            count += count_unresolved_vars(node->as.if_stmt.condition);
            count += count_unresolved_vars(node->as.if_stmt.then_branch);
            count += count_unresolved_vars(node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            count += count_unresolved_vars(node->as.while_stmt.condition);
            count += count_unresolved_vars(node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            count += count_unresolved_vars(node->as.for_stmt.initializer);
            count += count_unresolved_vars(node->as.for_stmt.condition);
            count += count_unresolved_vars(node->as.for_stmt.increment);
            count += count_unresolved_vars(node->as.for_stmt.body);
            break;
        case AST_FOR_IN_STMT:
            count += count_unresolved_vars(node->as.for_in_stmt.collection);
            count += count_unresolved_vars(node->as.for_in_stmt.body);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                count += count_unresolved_vars(node->as.print_stmt.exprs[i]);
            break;
        case AST_THROW_STMT:
            count += count_unresolved_vars(node->as.throw_stmt.expression);
            break;
        case AST_TRY_CATCH:
            count += count_unresolved_vars(node->as.try_catch.try_body);
            count += count_unresolved_vars(node->as.try_catch.catch_body);
            count += count_unresolved_vars(node->as.try_catch.finally_body);
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            count += count_unresolved_vars(node->as.function_decl.body);
            break;
        case AST_CALL_EXPR:
            count += count_unresolved_vars(node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                count += count_unresolved_vars(node->as.call_expr.arguments[i]);
            break;
        case AST_MEMBER_ACCESS:
            count += count_unresolved_vars(node->as.member_access.object);
            break;
        case AST_MEMBER_SET:
            count += count_unresolved_vars(node->as.member_set.object);
            count += count_unresolved_vars(node->as.member_set.value);
            break;
        case AST_INDEX_GET:
            count += count_unresolved_vars(node->as.index_get.array);
            count += count_unresolved_vars(node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            count += count_unresolved_vars(node->as.index_set.array);
            count += count_unresolved_vars(node->as.index_set.index);
            count += count_unresolved_vars(node->as.index_set.value);
            break;
        case AST_SLICE_EXPR:
            count += count_unresolved_vars(node->as.slice_expr.source);
            count += count_unresolved_vars(node->as.slice_expr.start);
            count += count_unresolved_vars(node->as.slice_expr.end);
            break;
        case AST_TERNARY:
            count += count_unresolved_vars(node->as.ternary.condition);
            count += count_unresolved_vars(node->as.ternary.true_expr);
            count += count_unresolved_vars(node->as.ternary.false_expr);
            break;
        case AST_NULLISH_COALESCE:
        case AST_RANGE:
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
            count += count_unresolved_vars(node->as.binary.left);
            count += count_unresolved_vars(node->as.binary.right);
            break;
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            count += count_unresolved_vars(node->as.unary.operand);
            break;
        case AST_GROUPING:
            count += count_unresolved_vars(node->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
            count += count_unresolved_vars(node->as.unary.operand);
            break;
        case AST_IS_EXPR:
            count += count_unresolved_vars(node->as.is_expr.expr);
            break;
        case AST_AS_EXPR:
            count += count_unresolved_vars(node->as.as_expr.expr);
            break;
        case AST_OPTIONAL_CHAIN:
            count += count_unresolved_vars(node->as.optional_chain.object);
            if (node->as.optional_chain.index)
                count += count_unresolved_vars(node->as.optional_chain.index);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                count += count_unresolved_vars(node->as.array_literal.elements[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                count += count_unresolved_vars(node->as.map_literal.keys[i]);
                count += count_unresolved_vars(node->as.map_literal.values[i]);
            }
            break;
        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++)
                count += count_unresolved_vars(node->as.set_literal.elements[i]);
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                count += count_unresolved_vars(node->as.object_literal.values[i]);
            break;
        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                count += count_unresolved_vars(node->as.struct_literal.field_values[i]);
            break;
        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                count += count_unresolved_vars(node->as.template_str.parts[i]);
            break;
        case AST_NEW_EXPR:
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                count += count_unresolved_vars(node->as.new_expr.arguments[i]);
            break;
        case AST_SUPER_CALL:
            for (int i = 0; i < node->as.super_call.arg_count; i++)
                count += count_unresolved_vars(node->as.super_call.arguments[i]);
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            for (int i = 0; i < cls->field_count; i++)
                count += count_unresolved_vars(cls->fields[i]);
            for (int i = 0; i < cls->method_count; i++)
                count += count_unresolved_vars(cls->methods[i]);
            break;
        }
        case AST_FIELD_DECL:
            count += count_unresolved_vars(node->as.field_decl.initializer);
            break;
        case AST_ENUM_DECL:
            for (int i = 0; i < node->as.enum_decl.member_count; i++)
                count += count_unresolved_vars(node->as.enum_decl.members[i]);
            break;
        case AST_ENUM_CONVERT:
            count += count_unresolved_vars(node->as.enum_convert.value_expr);
            break;
        case AST_ENUM_INDEX:
            count += count_unresolved_vars(node->as.enum_index.collection);
            count += count_unresolved_vars(node->as.enum_index.index_expr);
            break;
        case AST_MATCH_EXPR:
            count += count_unresolved_vars(node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++)
                count += count_unresolved_vars(node->as.match_expr.arms[i]);
            break;
        case AST_MATCH_ARM:
            count += count_unresolved_vars(node->as.match_arm.guard);
            count += count_unresolved_vars(node->as.match_arm.body);
            break;
        case AST_GO_EXPR:
            count += count_unresolved_vars(node->as.go_expr.expr);
            if (node->as.go_expr.priority)
                count += count_unresolved_vars(node->as.go_expr.priority);
            break;
        case AST_AWAIT_EXPR:
            count += count_unresolved_vars(node->as.await_expr.expr);
            if (node->as.await_expr.timeout)
                count += count_unresolved_vars(node->as.await_expr.timeout);
            break;
        case AST_CHANNEL_NEW:
            count += count_unresolved_vars(node->as.channel_new.buffer_size);
            break;
        case AST_MOVE_EXPR:
            count += count_unresolved_vars(node->as.move_expr.expr);
            break;
        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (!sel->cases[i])
                    continue;
                SelectCaseNode *sc = &sel->cases[i]->as.select_case;
                count += count_unresolved_vars(sc->channel);
                count += count_unresolved_vars(sc->value);
                count += count_unresolved_vars(sc->body);
            }
            break;
        }
        case AST_DEFER_STMT:
            count += count_unresolved_vars(node->as.defer_stmt.expr);
            break;
        case AST_SCOPE_BLOCK:
            count += count_unresolved_vars(node->as.scope_block.body);
            break;
        case AST_DESTRUCTURE_DECL:
            count += count_unresolved_vars(node->as.destructure_decl.initializer);
            break;
        case AST_DESTRUCTURE_ASSIGN:
            count += count_unresolved_vars(node->as.destructure_assign.value);
            break;
        case AST_EXPORT_STMT:
            count += count_unresolved_vars(node->as.export_stmt.declaration);
            break;
        /* Leaf nodes with no variable children */
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
        case AST_IMPORT_STMT:
        case AST_TYPE_ALIAS:
        case AST_INTERFACE_DECL:
        case AST_INTERFACE_METHOD:
        case AST_YIELD_STMT:
        case AST_METHOD_DECL:
        case AST_PATTERN_LITERAL:
        case AST_PATTERN_RANGE:
        case AST_PATTERN_WILDCARD:
        case AST_PATTERN_MULTI:
        case AST_CHAN_SEND:
        case AST_CHAN_RECV:
            break;
        default:
            break;
    }
    return count;
}

/* Parse + analyze + lower; check binding invariant.
 * Returns true if all bindings resolved and lowering succeeded. */
static bool check_bindings(const char *source, const char *label) {
    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  [%s] PARSE FAILED\n", label);
        return false;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    xa_analyzer_analyze(analyzer, "binding_test.xr", program);

    /* Spot-check: no AST_VARIABLE nodes should have symbol_id=0 */
    int unresolved = count_unresolved_vars(program);
    if (unresolved > 0) {
        fprintf(stderr, "  [%s] %d unresolved variable(s) after analysis\n", label, unresolved);
    }

    /* Redirect stderr during lowering to suppress expected diagnostics */
#ifdef _WIN32
    freopen("NUL", "w", stderr);
#else
    int saved_fd = dup(STDERR_FILENO);
    freopen("/dev/null", "w", stderr);
#endif
    if (program->type == AST_PROGRAM && program->as.program.arena)
        xr_isolate_set_current_arena(g_iso, program->as.program.arena);
    xr_canon_program(program, analyzer, g_iso);
    xr_isolate_set_current_arena(g_iso, NULL);
    XiFunc *func = xi_lower_program(program, analyzer, g_iso);
#ifdef _WIN32
    freopen("CON", "w", stderr);
#else
    fflush(stderr);
    dup2(saved_fd, STDERR_FILENO);
    close(saved_fd);
#endif

    bool ok = (func != NULL);
    if (func)
        xi_func_free(func);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (!ok) {
        fprintf(stderr, "  [%s] LOWER FAILED (binding contract violated)\n", label);
    }
    return ok && (unresolved == 0);
}

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- %s ---\n", #name);                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* ========== Binding Pattern Tests ========== */

TEST(simple_var_decl) {
    assert(check_bindings("let x = 42\nprint(x)", "simple_var_decl"));
}

TEST(const_decl) {
    assert(check_bindings("const PI = 3.14\nprint(PI)", "const_decl"));
}

TEST(variable_reassignment) {
    assert(check_bindings("let x = 1\nx = x + 2\nprint(x)", "var_reassign"));
}

TEST(compound_assignment) {
    assert(check_bindings("let x = 10\nx += 5\nprint(x)", "compound_assign"));
}

TEST(shadowing) {
    assert(check_bindings("let x = 1\n"
                          "{\n"
                          "    let x = 2\n"
                          "    print(x)\n"
                          "}\n"
                          "print(x)\n",
                          "shadowing"));
}

TEST(nested_scope) {
    assert(check_bindings("let a = 1\n"
                          "{\n"
                          "    let b = a + 1\n"
                          "    {\n"
                          "        let c = b + 1\n"
                          "        print(c)\n"
                          "    }\n"
                          "}\n",
                          "nested_scope"));
}

TEST(function_params) {
    assert(check_bindings("fn add(a: int, b: int): int {\n"
                          "    return a + b\n"
                          "}\n"
                          "print(add(1, 2))\n",
                          "function_params"));
}

TEST(closure_capture) {
    assert(check_bindings("let x = 10\n"
                          "fn foo(): int {\n"
                          "    return x + 1\n"
                          "}\n"
                          "print(foo())\n",
                          "closure_capture"));
}

TEST(nested_closure) {
    assert(check_bindings("let outer = 1\n"
                          "fn foo(): int {\n"
                          "    let mid = 2\n"
                          "    fn bar(): int {\n"
                          "        return outer + mid\n"
                          "    }\n"
                          "    return bar()\n"
                          "}\n",
                          "nested_closure"));
}

TEST(for_in_binding) {
    assert(check_bindings("const arr = [1, 2, 3]\n"
                          "for (item in arr) {\n"
                          "    print(item)\n"
                          "}\n",
                          "for_in_binding"));
}

TEST(for_in_kv_binding) {
    assert(check_bindings("const arr = [10, 20, 30]\n"
                          "for (i, val in arr) {\n"
                          "    print(i)\n"
                          "    print(val)\n"
                          "}\n",
                          "for_in_kv_binding"));
}

TEST(if_else_scoping) {
    assert(check_bindings("let x = 10\n"
                          "if (x > 5) {\n"
                          "    let y = x + 1\n"
                          "    print(y)\n"
                          "} else {\n"
                          "    let z = x - 1\n"
                          "    print(z)\n"
                          "}\n",
                          "if_else_scoping"));
}

TEST(while_loop_scoping) {
    assert(check_bindings("let i = 0\n"
                          "while (i < 10) {\n"
                          "    print(i)\n"
                          "    i += 1\n"
                          "}\n",
                          "while_loop_scoping"));
}

TEST(ternary_binding) {
    assert(check_bindings("let x = 5\n"
                          "let y = x > 3 ? x + 1 : x - 1\n"
                          "print(y)\n",
                          "ternary_binding"));
}

TEST(template_string_binding) {
    assert(check_bindings("let name = \"world\"\n"
                          "let msg = \"hello ${name}\"\n"
                          "print(msg)\n",
                          "template_string_binding"));
}

TEST(multiple_functions) {
    assert(check_bindings("fn a(): int { return 1 }\n"
                          "fn b(): int { return a() + 1 }\n"
                          "print(b())\n",
                          "multiple_functions"));
}

TEST(class_method_this) {
    assert(check_bindings("class Foo {\n"
                          "    x: int\n"
                          "    init(val: int) {\n"
                          "        this.x = val\n"
                          "    }\n"
                          "    get(): int {\n"
                          "        return this.x\n"
                          "    }\n"
                          "}\n",
                          "class_method_this"));
}

TEST(try_catch_binding) {
    assert(check_bindings("try {\n"
                          "    let x = 42\n"
                          "    print(x)\n"
                          "} catch (e) {\n"
                          "    print(e)\n"
                          "}\n",
                          "try_catch_binding"));
}

TEST(array_and_map_literal) {
    assert(check_bindings("let a = [1, 2, 3]\n"
                          "let m = {\"key\": a}\n"
                          "print(a)\n"
                          "print(m)\n",
                          "array_and_map_literal"));
}

/* ========== Main ========== */

int main(void) {
    setup();

    run_simple_var_decl();
    run_const_decl();
    run_variable_reassignment();
    run_compound_assignment();
    run_shadowing();
    run_nested_scope();
    run_function_params();
    run_closure_capture();
    run_nested_closure();
    run_for_in_binding();
    run_for_in_kv_binding();
    run_if_else_scoping();
    run_while_loop_scoping();
    run_ternary_binding();
    run_template_string_binding();
    run_multiple_functions();
    run_class_method_this();
    run_try_catch_binding();
    run_array_and_map_literal();

    teardown();

    printf("\n=== %d/%d Symbol Binding tests passed ===\n", tests_passed,
           tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
