/*
 * test_xi_lower.c - Unit tests for AST to Xi IR lowering
 *
 * Uses a minimal isolate + analyzer to test the full lowering pipeline.
 * Each test parses a small xray source snippet, runs the analyzer,
 * lowers to Xi IR, and verifies the dump output.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_lower.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../include/xray_isolate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Test Infrastructure ========== */

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

/* Parse source, run analyzer, lower to Xi, dump and return XiFunc.
 * Caller must call xi_func_free() on the result. */
static XiFunc *lower_source(const char *source) {
    assert(g_iso != NULL);

    /* Parse */
    AstNode *program = xr_parse(g_iso, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        return NULL;
    }

    /* Analyze */
    XaAnalyzer *analyzer = xa_analyzer_new(g_iso);
    if (!analyzer) {
        fprintf(stderr, "  ANALYZER ALLOC FAILED\n");
        xr_program_destroy(program);
        return NULL;
    }
    xa_analyzer_analyze(analyzer, "test.xr", program);

    /* Lower */
    XiFunc *func = xi_lower_program(program, analyzer, g_iso);
    if (!func) {
        fprintf(stderr, "  LOWER FAILED for: %s\n", source);
        xa_analyzer_free(analyzer);
        xr_program_destroy(program);
        return NULL;
    }

    /* Dump to stdout for visual verification */
    xi_func_dump(func, stdout);

    /* Cleanup AST and analyzer (Xi IR is independent) */
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    return func;
}

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("--- %s ---\n", #name); \
        test_##name(); \
        printf("  PASS\n\n"); \
        tests_passed++; \
    } \
    static void test_##name(void)

/* ========== Tests ========== */

TEST(simple_arithmetic) {
    XiFunc *f = lower_source("let x = 1 + 2\nlet y = x * 3\nprint(y)");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    /* Entry block should have: const 1, const 2, add, const 3, mul, print */
    assert(f->entry->nvalues >= 5);
    xi_func_free(f);
}

TEST(variable_assignment) {
    XiFunc *f = lower_source("let x = 10\nx = x + 5\nprint(x)");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    xi_func_free(f);
}

TEST(if_else) {
    XiFunc *f = lower_source(
        "let x = 10\n"
        "if (x > 5) {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}\n"
    );
    assert(f != NULL);
    /* Should have: entry, then, else, merge blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(while_loop) {
    XiFunc *f = lower_source(
        "let i = 0\n"
        "while (i < 10) {\n"
        "    i = i + 1\n"
        "}\n"
        "print(i)\n"
    );
    assert(f != NULL);
    /* Should have: entry, cond, body, exit blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(for_loop) {
    XiFunc *f = lower_source(
        "let sum = 0\n"
        "for (let i = 0; i < 5; i = i + 1) {\n"
        "    sum = sum + i\n"
        "}\n"
        "print(sum)\n"
    );
    assert(f != NULL);
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(nested_if) {
    XiFunc *f = lower_source(
        "let x = 10\n"
        "if (x > 5) {\n"
        "    if (x > 8) {\n"
        "        print(2)\n"
        "    } else {\n"
        "        print(1)\n"
        "    }\n"
        "} else {\n"
        "    print(0)\n"
        "}\n"
    );
    assert(f != NULL);
    assert(f->nblocks >= 5);
    xi_func_free(f);
}

TEST(bool_literals) {
    XiFunc *f = lower_source(
        "let a = true\n"
        "let b = false\n"
        "let c = !a\n"
        "print(c)\n"
    );
    assert(f != NULL);
    xi_func_free(f);
}

TEST(float_arithmetic) {
    XiFunc *f = lower_source(
        "let x = 3.14\n"
        "let y = x * 2.0\n"
        "print(y)\n"
    );
    assert(f != NULL);
    xi_func_free(f);
}

TEST(string_const) {
    XiFunc *f = lower_source(
        "let msg = \"hello\"\n"
        "print(msg)\n"
    );
    assert(f != NULL);
    xi_func_free(f);
}

TEST(comparison_ops) {
    XiFunc *f = lower_source(
        "let a = 1\n"
        "let b = 2\n"
        "let eq = a == b\n"
        "let ne = a != b\n"
        "let lt = a < b\n"
        "print(eq)\n"
        "print(ne)\n"
        "print(lt)\n"
    );
    assert(f != NULL);
    xi_func_free(f);
}

TEST(compound_assignment) {
    XiFunc *f = lower_source(
        "let x = 10\n"
        "x += 5\n"
        "x -= 2\n"
        "print(x)\n"
    );
    assert(f != NULL);
    assert(f->nblocks >= 1);
    /* const 10, const 5, add, const 2, sub, print */
    assert(f->entry->nvalues >= 5);
    xi_func_free(f);
}

TEST(inc_dec) {
    XiFunc *f = lower_source(
        "let x = 0\n"
        "x++\n"
        "x++\n"
        "x--\n"
        "print(x)\n"
    );
    assert(f != NULL);
    /* const 0, [const 1, add] * 2, [const 1, sub], print */
    assert(f->entry->nvalues >= 6);
    xi_func_free(f);
}

TEST(ternary_expr) {
    XiFunc *f = lower_source(
        "let x = 10\n"
        "let y = (x > 5) ? 1 : 0\n"
        "print(y)\n"
    );
    assert(f != NULL);
    /* ternary produces: entry, then, else, merge blocks */
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(break_continue) {
    XiFunc *f = lower_source(
        "let i = 0\n"
        "while (i < 100) {\n"
        "    i = i + 1\n"
        "    if (i == 5) {\n"
        "        break\n"
        "    }\n"
        "    if (i == 3) {\n"
        "        continue\n"
        "    }\n"
        "}\n"
        "print(i)\n"
    );
    assert(f != NULL);
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(nested_while) {
    XiFunc *f = lower_source(
        "let sum = 0\n"
        "let i = 0\n"
        "while (i < 3) {\n"
        "    let j = 0\n"
        "    while (j < 3) {\n"
        "        sum = sum + 1\n"
        "        j = j + 1\n"
        "    }\n"
        "    i = i + 1\n"
        "}\n"
        "print(sum)\n"
    );
    assert(f != NULL);
    /* Two nested loops: each needs cond + body + exit blocks */
    assert(f->nblocks >= 6);
    xi_func_free(f);
}

TEST(type_propagation) {
    XiFunc *f = lower_source(
        "let a = 1\n"
        "let b = 2.0\n"
        "let c = a + a\n"
        "let d = b + b\n"
        "let e = a > 0\n"
        "print(c)\n"
        "print(d)\n"
        "print(e)\n"
    );
    assert(f != NULL);
    /* Verify types: walk entry block values */
    XiBlock *b0 = f->entry;
    assert(b0 != NULL);
    /* Find add operations and check their types */
    int found_int_add = 0, found_float_add = 0, found_bool_gt = 0;
    for (uint32_t i = 0; i < b0->nvalues; i++) {
        XiValue *v = b0->values[i];
        if (v->op == XI_ADD && v->type && v->type->kind == XR_KIND_INT)
            found_int_add = 1;
        if (v->op == XI_ADD && v->type && v->type->kind == XR_KIND_FLOAT)
            found_float_add = 1;
        if (v->op == XI_GT && v->type && v->type->kind == XR_KIND_BOOL)
            found_bool_gt = 1;
    }
    assert(found_int_add && "int + int should produce int");
    assert(found_float_add && "float + float should produce float");
    assert(found_bool_gt && "a > 0 should produce bool");
    xi_func_free(f);
}

TEST(array_literal) {
    XiFunc *f = lower_source(
        "let arr = [1, 2, 3]\n"
        "print(arr)\n"
    );
    assert(f != NULL);
    /* Should have: CONST*3 elements, ARRAY_NEW, INDEX_SET*3, PRINT */
    int found_array_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_ARRAY_NEW)
            found_array_new = 1;
    }
    assert(found_array_new && "should have ARRAY_NEW op");
    xi_func_free(f);
}

TEST(index_access) {
    XiFunc *f = lower_source(
        "let arr = [10, 20, 30]\n"
        "let x = arr[1]\n"
        "print(x)\n"
    );
    assert(f != NULL);
    int found_index_get = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_INDEX_GET)
            found_index_get = 1;
    }
    assert(found_index_get && "should have INDEX_GET op");
    xi_func_free(f);
}

TEST(member_access) {
    XiFunc *f = lower_source(
        "let arr = [1, 2, 3]\n"
        "let n = arr.length\n"
        "print(n)\n"
    );
    assert(f != NULL);
    int found_load_field = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_LOAD_FIELD)
            found_load_field = 1;
    }
    assert(found_load_field && "should have LOAD_FIELD op");
    xi_func_free(f);
}

TEST(throw_stmt) {
    XiFunc *f = lower_source(
        "let x = 1\n"
        "if (x == 0) {\n"
        "    throw \"error\"\n"
        "}\n"
        "print(x)\n"
    );
    assert(f != NULL);
    /* Should have: entry + then(throw) + else + merge */
    assert(f->nblocks >= 3);
    /* Find the throw block — should be UNREACHABLE */
    int found_throw = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_THROW) {
                found_throw = 1;
                assert(blk->kind == XI_BLOCK_UNREACHABLE);
            }
        }
    }
    assert(found_throw && "should have THROW op");
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Lower Unit Tests ===\n\n");

    setup();

    run_simple_arithmetic();
    run_variable_assignment();
    run_if_else();
    run_while_loop();
    run_for_loop();
    run_nested_if();
    run_bool_literals();
    run_float_arithmetic();
    run_string_const();
    run_comparison_ops();
    run_compound_assignment();
    run_inc_dec();
    run_ternary_expr();
    run_break_continue();
    run_nested_while();
    run_type_propagation();
    run_array_literal();
    run_index_access();
    run_member_access();
    run_throw_stmt();

    teardown();

    printf("=== %d/%d Xi Lower tests passed ===\n",
           tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
