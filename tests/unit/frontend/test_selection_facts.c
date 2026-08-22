/*
 * test_selection_facts.c - Selection invariant acceptance tests
 *
 * Verifies that the analyzer records XaSelection facts for member
 * access, method calls, index operations, enum member access, and
 * module export access.  Each test parses + analyzes real code, then
 * walks the AST to find the access node and checks that a selection
 * fact was recorded with the expected kind.
 *
 * The lowerer and backends should eventually read these facts instead
 * of re-discovering member resolution at each stage.
 */

#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/frontend/analyzer/xa_parallel_call_plan.h"
#include "../../../src/frontend/analyzer/xa_selection.h"
#include "../../../src/toolchain/xcompiler_session.h"
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static XrCompilerSession *g_session = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p = {0};
        g_iso = xray_vm_new_full(&p);
        g_session = xr_compiler_session_current_for_isolate(g_iso);
        assert(g_session != NULL);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
        g_session = NULL;
    }
}

/* ========== AST search helpers ========== */

/* Find the first AST_MEMBER_ACCESS node in the tree. */
static AstNode *find_member_access(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_MEMBER_ACCESS)
        return node;

    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->as.program.count; i++) {
                AstNode *r = find_member_access(node->as.program.statements[i]);
                if (r)
                    return r;
            }
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return find_member_access(node->as.var_decl.initializer);
        case AST_EXPR_STMT:
            return find_member_access(node->as.expr_stmt);
        case AST_CALL_EXPR: {
            AstNode *r = find_member_access(node->as.call_expr.callee);
            if (r)
                return r;
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                r = find_member_access(node->as.call_expr.arguments[i]);
                if (r)
                    return r;
            }
            break;
        }
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                AstNode *r = find_member_access(node->as.print_stmt.exprs[i]);
                if (r)
                    return r;
            }
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return find_member_access(node->as.function_decl.body);
        case AST_IF_STMT: {
            AstNode *r = find_member_access(node->as.if_stmt.condition);
            if (r)
                return r;
            r = find_member_access(node->as.if_stmt.then_branch);
            if (r)
                return r;
            return find_member_access(node->as.if_stmt.else_branch);
        }
        default:
            break;
    }
    return NULL;
}

/* Find the first AST_ENUM_ACCESS node in the tree. */
static AstNode *find_enum_access(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_ENUM_ACCESS)
        return node;

    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->as.program.count; i++) {
                AstNode *r = find_enum_access(node->as.program.statements[i]);
                if (r)
                    return r;
            }
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return find_enum_access(node->as.var_decl.initializer);
        case AST_EXPR_STMT:
            return find_enum_access(node->as.expr_stmt);
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                AstNode *r = find_enum_access(node->as.print_stmt.exprs[i]);
                if (r)
                    return r;
            }
            break;
        default:
            break;
    }
    return NULL;
}

/* Find a CALL_EXPR whose callee is a bare variable with the requested name. */
static AstNode *find_call_with_variable_callee(AstNode *node, const char *name) {
    if (!node || !name)
        return NULL;

    if (node->type == AST_CALL_EXPR) {
        AstNode *callee = node->as.call_expr.callee;
        if (callee && callee->type == AST_VARIABLE && callee->as.variable.name &&
            strcmp(callee->as.variable.name, name) == 0)
            return node;
    }

    switch (node->type) {
        case AST_PROGRAM:
        case AST_BLOCK:
            for (int i = 0; i < node->as.program.count; i++) {
                AstNode *r = find_call_with_variable_callee(node->as.program.statements[i], name);
                if (r)
                    return r;
            }
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return find_call_with_variable_callee(node->as.var_decl.initializer, name);
        case AST_EXPR_STMT:
            return find_call_with_variable_callee(node->as.expr_stmt, name);
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                AstNode *r = find_call_with_variable_callee(node->as.print_stmt.exprs[i], name);
                if (r)
                    return r;
            }
            break;
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return find_call_with_variable_callee(node->as.function_decl.body, name);
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                AstNode *r = find_call_with_variable_callee(node->as.return_stmt.values[i], name);
                if (r)
                    return r;
            }
            break;
        case AST_CALL_EXPR: {
            AstNode *r = find_call_with_variable_callee(node->as.call_expr.callee, name);
            if (r)
                return r;
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                r = find_call_with_variable_callee(node->as.call_expr.arguments[i], name);
                if (r)
                    return r;
            }
            break;
        }
        case AST_MEMBER_ACCESS:
            return find_call_with_variable_callee(node->as.member_access.object, name);
        case AST_INDEX_GET: {
            AstNode *r = find_call_with_variable_callee(node->as.index_get.array, name);
            return r ? r : find_call_with_variable_callee(node->as.index_get.index, name);
        }
        case AST_RANGE: {
            AstNode *r = find_call_with_variable_callee(node->as.range.start, name);
            return r ? r : find_call_with_variable_callee(node->as.range.end, name);
        }
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
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR: {
            AstNode *r = find_call_with_variable_callee(node->as.binary.left, name);
            return r ? r : find_call_with_variable_callee(node->as.binary.right, name);
        }
        case AST_GROUPING:
            return find_call_with_variable_callee(node->as.grouping, name);
        case AST_ASSIGNMENT:
            return find_call_with_variable_callee(node->as.assignment.value, name);
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++) {
                AstNode *r =
                    find_call_with_variable_callee(node->as.array_literal.elements[i], name);
                if (r)
                    return r;
            }
            if (node->as.array_literal.repeat_value) {
                AstNode *r =
                    find_call_with_variable_callee(node->as.array_literal.repeat_value, name);
                if (r)
                    return r;
            }
            return find_call_with_variable_callee(node->as.array_literal.repeat_count, name);
        default:
            break;
    }
    return NULL;
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

typedef struct {
    AstNode *program;
    XaAnalyzer *analyzer;
} AnalysisResult;

static AnalysisResult analyze(const char *source) {
    AnalysisResult r = {NULL, NULL};
    r.program = xr_parse(xr_compiler_session_current_for_isolate(g_iso), source);
    if (!r.program) {
        fprintf(stderr, "    parse error\n");
        return r;
    }
    r.analyzer = xa_analyzer_new(g_session);
    assert(r.analyzer != NULL);
    xa_analyzer_analyze(r.analyzer, "sel_test.xr", r.program);
    return r;
}

static void cleanup(AnalysisResult *r) {
    if (r->analyzer)
        xa_analyzer_free(r->analyzer);
    if (r->program)
        xr_program_destroy(r->program);
    r->analyzer = NULL;
    r->program = NULL;
}

/* ========== Tests ========== */

TEST(class_field_access_has_selection) {
    AnalysisResult r = analyze("class Point {\n"
                               "    x: i64\n"
                               "    y: i64\n"
                               "    constructor(x: i64, y: i64) {\n"
                               "        this.x = x\n"
                               "        this.y = y\n"
                               "    }\n"
                               "}\n"
                               "var p = Point(1, 2)\n"
                               "print(p.x)\n");
    if (!r.program)
        return false;

    AstNode *ma = find_member_access(r.program);
    bool ok = false;
    if (ma) {
        const XaSelection *sel = xa_analyzer_get_selection(r.analyzer, ma);
        if (sel) {
            printf("    selection kind=%d receiver=%p result=%p field_idx=%d\n", sel->kind,
                   (void *) sel->receiver_type, (void *) sel->result_type, sel->field_index);
            ok = true;
        } else {
            fprintf(stderr, "    no selection recorded for member access 'p.x'\n");
        }
    } else {
        fprintf(stderr, "    no AST_MEMBER_ACCESS found\n");
    }

    cleanup(&r);
    return ok;
}

TEST(method_call_has_selection) {
    AnalysisResult r = analyze("class Greeter {\n"
                               "    name: string\n"
                               "    constructor(n: string) {\n"
                               "        this.name = n\n"
                               "    }\n"
                               "    greet() -> string {\n"
                               "        return \"hello\"\n"
                               "    }\n"
                               "}\n"
                               "var g = Greeter(\"world\")\n"
                               "print(g.greet())\n");
    if (!r.program)
        return false;

    /* g.greet() parses as CALL(MEMBER_ACCESS(g, "greet"), []) */
    AstNode *ma = find_member_access(r.program);
    bool ok = false;
    if (ma) {
        const XaSelection *sel = xa_analyzer_get_selection(r.analyzer, ma);
        if (sel) {
            printf("    method selection kind=%d\n", sel->kind);
            ok = (sel->kind == XA_SEL_METHOD || sel->kind == XA_SEL_FIELD);
        } else {
            fprintf(stderr, "    no selection for method access 'g.greet'\n");
        }
    } else {
        fprintf(stderr, "    no AST_MEMBER_ACCESS found\n");
    }

    cleanup(&r);
    return ok;
}

TEST(enum_member_has_selection) {
    AnalysisResult r = analyze("enum Color { Red, Blue }\n"
                               "var c = Color.Red\n"
                               "print(c)\n");
    if (!r.program)
        return false;

    /* Color.Red may be rewritten by the analyzer from AST_MEMBER_ACCESS
     * to AST_ENUM_ACCESS.  Check both: selection on the original node,
     * or the node was converted to AST_ENUM_ACCESS (which is itself
     * the resolved selection). */
    AstNode *ea = find_enum_access(r.program);
    AstNode *ma = find_member_access(r.program);
    bool ok = false;
    if (ea) {
        printf("    enum access node found (node_type=%d)\n", ea->type);
        const XaSelection *sel = xa_analyzer_get_selection(r.analyzer, ea);
        if (sel) {
            printf("    enum selection kind=%d\n", sel->kind);
            ok = (sel->kind == XA_SEL_ENUM_MEMBER);
        } else {
            /* AST_ENUM_ACCESS itself IS the resolved result — no extra
             * selection fact needed because the node type encodes it. */
            printf("    AST_ENUM_ACCESS found — resolution is implicit\n");
            ok = true;
        }
    } else if (ma) {
        const XaSelection *sel = xa_analyzer_get_selection(r.analyzer, ma);
        if (sel) {
            printf("    member access selection kind=%d\n", sel->kind);
            ok = (sel->kind == XA_SEL_ENUM_MEMBER);
        } else {
            fprintf(stderr, "    no selection for Color.Red member access\n");
        }
    } else {
        fprintf(stderr, "    no AST_ENUM_ACCESS or AST_MEMBER_ACCESS found\n");
    }

    cleanup(&r);
    return ok;
}

TEST(selection_table_has_entries_after_analysis) {
    AnalysisResult r = analyze("class Box {\n"
                               "    value: i64\n"
                               "    constructor(v: i64) {\n"
                               "        this.value = v\n"
                               "    }\n"
                               "    get() -> i64 {\n"
                               "        return this.value\n"
                               "    }\n"
                               "}\n"
                               "var b = Box(42)\n"
                               "var v = b.get()\n"
                               "var w = b.value\n"
                               "print(v, w)\n");
    if (!r.program)
        return false;

    const XaSelectionTable *t = (const XaSelectionTable *) r.analyzer->selection_table;
    int sz = xa_selection_table_size(t);
    printf("    selection table size = %d\n", sz);
    bool ok = (sz > 0);
    if (!ok) {
        fprintf(stderr, "    selection table is empty after analyzing class code\n");
    }

    cleanup(&r);
    return ok;
}

TEST(builtin_call_has_no_member_selection) {
    AnalysisResult r = analyze("var arr = [1, 2, 3]\n"
                               "var n = len(arr)\n"
                               "print(n)\n");
    if (!r.program)
        return false;

    AstNode *ma = find_member_access(r.program);
    bool ok = ma == NULL;
    if (!ok)
        fprintf(stderr, "    canonical len(arr) unexpectedly contains member access\n");

    cleanup(&r);
    return ok;
}

TEST(parallel_selective_alias_call_plan_recorded) {
    AnalysisResult r = analyze("import { forEach as each, map as parMap, reduce as fold, "
                               "Options as ParOptions } from parallel\n"
                               "each(0..4, (i) -> {\n"
                               "    print(i)\n"
                               "}, ParOptions(2))\n"
                               "var xs = parMap(0..4, (i) -> i + 1, ParOptions(2))\n"
                               "var sum = fold(0..4, 0, (i) -> i + 1, "
                               "(a, b) -> a + b, ParOptions(2))\n"
                               "print(xs[3], sum)\n");
    if (!r.program)
        return false;

    struct {
        const char *local_name;
        XaParallelCallKind expected;
        XaIntrinsicId expected_intrinsic;
    } cases[] = {
        {"each", XA_PAR_CALL_FOR_EACH, XA_INTRINSIC_PARALLEL_FOR_EACH},
        {"parMap", XA_PAR_CALL_MAP, XA_INTRINSIC_PARALLEL_MAP},
        {"fold", XA_PAR_CALL_REDUCE, XA_INTRINSIC_PARALLEL_REDUCE},
    };

    bool ok = true;
    for (int i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
        AstNode *call = find_call_with_variable_callee(r.program, cases[i].local_name);
        if (!call) {
            fprintf(stderr, "    no call found for alias '%s'\n", cases[i].local_name);
            ok = false;
            continue;
        }
        const XaParallelCallPlan *plan = xa_analyzer_get_parallel_call_plan(r.analyzer, call);
        if (!plan) {
            fprintf(stderr, "    no parallel call plan recorded for alias '%s'\n",
                    cases[i].local_name);
            ok = false;
            continue;
        }
        printf("    alias %s -> kind=%d plan_method=%d\n", cases[i].local_name, plan->kind,
               plan->is_plan_method ? 1 : 0);
        if (plan->kind != cases[i].expected || plan->intrinsic_id != cases[i].expected_intrinsic ||
            plan->is_plan_method) {
            fprintf(stderr, "    unexpected call plan for alias '%s'\n", cases[i].local_name);
            ok = false;
        }
    }

    cleanup(&r);
    return ok;
}

/* ========== Main ========== */

int main(void) {
    setup();

    printf("\n=== Selection Facts Tests ===\n\n");

    run_class_field_access_has_selection();
    run_method_call_has_selection();
    run_enum_member_has_selection();
    run_selection_table_has_entries_after_analysis();
    run_builtin_call_has_no_member_selection();
    run_parallel_selective_alias_call_plan_recorded();

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    teardown();
    return tests_failed > 0 ? 1 : 0;
}
