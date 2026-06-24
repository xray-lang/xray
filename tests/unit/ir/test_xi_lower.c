/*
 * test_xi_lower.c - Unit tests for AST to Xi IR lowering
 *
 * Uses a minimal isolate + analyzer to test the full lowering pipeline.
 * Each test parses a small xray source snippet, runs the analyzer,
 * lowers to Xi IR, and verifies the dump output.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_lower.h"
#include "../../../src/frontend/canonical/xcanon.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/value/xstruct_layout.h"
#include "../../../src/frontend/parser/xast_nodes.h"
#include "../../../src/frontend/parser/xast_types.h"
#include "../../../src/frontend/parser/xparse.h"
#include "../../../src/frontend/analyzer/xanalyzer.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/toolchain/xcompiler_session.h"
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
        xray_isolate_setup_full(&p);
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
    AstNode *program = xr_parse(xr_compiler_session_current_for_isolate(g_iso), source);
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

    /* Re-install parse arena for canonicalizer allocations */
    XrCompilerSessionScope canon_scope;
    bool has_canon_scope =
        program->type == AST_PROGRAM && program->as.program.arena &&
        xr_compiler_session_push_arena(xr_compiler_session_current_for_isolate(g_iso),
                                       program->as.program.arena, "test.xr", &canon_scope);

    /* Canonicalize + Lower */
    xr_canon_program(program, analyzer, g_iso);
    if (has_canon_scope)
        xr_compiler_session_pop_arena(&canon_scope);
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

static int func_tree_has_op(XiFunc *f, uint16_t op) {
    if (!f)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] && blk->values[i]->op == op)
                return 1;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (func_tree_has_op(f->children[i], op))
            return 1;
    }
    return 0;
}

static XiValue *func_tree_find_op(XiFunc *f, uint16_t op) {
    if (!f)
        return NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i] && blk->values[i]->op == op)
                return blk->values[i];
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiValue *v = func_tree_find_op(f->children[i], op);
        if (v)
            return v;
    }
    return NULL;
}

static int func_tree_has_builtin_name(XiFunc *f, const char *name) {
    if (!f || !name)
        return 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_CALL_BUILTIN && v->aux && strcmp((const char *) v->aux, name) == 0)
                return 1;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (func_tree_has_builtin_name(f->children[i], name))
            return 1;
    }
    return 0;
}

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- %s ---\n", #name);                                                             \
        test_##name();                                                                             \
        printf("  PASS\n\n");                                                                      \
        tests_passed++;                                                                            \
    }                                                                                              \
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
    XiFunc *f = lower_source("let x = 10\n"
                             "if (x > 5) {\n"
                             "    print(1)\n"
                             "} else {\n"
                             "    print(0)\n"
                             "}\n");
    assert(f != NULL);
    /* Should have: entry, then, else, merge blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(while_loop) {
    XiFunc *f = lower_source("let i = 0\n"
                             "while (i < 10) {\n"
                             "    i = i + 1\n"
                             "}\n"
                             "print(i)\n");
    assert(f != NULL);
    /* Should have: entry, cond, body, exit blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(for_loop) {
    XiFunc *f = lower_source("let sum = 0\n"
                             "for (let i = 0; i < 5; i = i + 1) {\n"
                             "    sum = sum + i\n"
                             "}\n"
                             "print(sum)\n");
    assert(f != NULL);
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(nested_if) {
    XiFunc *f = lower_source("let x = 10\n"
                             "if (x > 5) {\n"
                             "    if (x > 8) {\n"
                             "        print(2)\n"
                             "    } else {\n"
                             "        print(1)\n"
                             "    }\n"
                             "} else {\n"
                             "    print(0)\n"
                             "}\n");
    assert(f != NULL);
    assert(f->nblocks >= 5);
    xi_func_free(f);
}

TEST(bool_literals) {
    XiFunc *f = lower_source("let a = true\n"
                             "let b = false\n"
                             "let c = !a\n"
                             "print(c)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(float_arithmetic) {
    XiFunc *f = lower_source("let x = 3.14\n"
                             "let y = x * 2.0\n"
                             "print(y)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(string_const) {
    XiFunc *f = lower_source("let msg = \"hello\"\n"
                             "print(msg)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(comparison_ops) {
    XiFunc *f = lower_source("let a = 1\n"
                             "let b = 2\n"
                             "let eq = a == b\n"
                             "let ne = a != b\n"
                             "let lt = a < b\n"
                             "print(eq)\n"
                             "print(ne)\n"
                             "print(lt)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(compound_assignment) {
    XiFunc *f = lower_source("let x = 10\n"
                             "x += 5\n"
                             "x -= 2\n"
                             "print(x)\n");
    assert(f != NULL);
    assert(f->nblocks >= 1);
    /* const 10, const 5, add, const 2, sub, print */
    assert(f->entry->nvalues >= 5);
    xi_func_free(f);
}

TEST(inc_dec) {
    XiFunc *f = lower_source("let x = 0\n"
                             "x++\n"
                             "x++\n"
                             "x--\n"
                             "print(x)\n");
    assert(f != NULL);
    /* const 0, [const 1, add] * 2, [const 1, sub], print */
    assert(f->entry->nvalues >= 6);
    xi_func_free(f);
}

TEST(ternary_expr) {
    XiFunc *f = lower_source("let x = 10\n"
                             "let y = (x > 5) ? 1 : 0\n"
                             "print(y)\n");
    assert(f != NULL);
    /* ternary produces: entry, then, else, merge blocks */
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(break_continue) {
    XiFunc *f = lower_source("let i = 0\n"
                             "while (i < 100) {\n"
                             "    i = i + 1\n"
                             "    if (i == 5) {\n"
                             "        break\n"
                             "    }\n"
                             "    if (i == 3) {\n"
                             "        continue\n"
                             "    }\n"
                             "}\n"
                             "print(i)\n");
    assert(f != NULL);
    assert(f->nblocks >= 4);
    xi_func_free(f);
}

TEST(nested_while) {
    XiFunc *f = lower_source("let sum = 0\n"
                             "let i = 0\n"
                             "while (i < 3) {\n"
                             "    let j = 0\n"
                             "    while (j < 3) {\n"
                             "        sum = sum + 1\n"
                             "        j = j + 1\n"
                             "    }\n"
                             "    i = i + 1\n"
                             "}\n"
                             "print(sum)\n");
    assert(f != NULL);
    /* Two nested loops: each needs cond + body + exit blocks */
    assert(f->nblocks >= 6);
    xi_func_free(f);
}

TEST(type_propagation) {
    XiFunc *f = lower_source("let a = 1\n"
                             "let b = 2.0\n"
                             "let c = a + a\n"
                             "let d = b + b\n"
                             "let e = a > 0\n"
                             "print(c)\n"
                             "print(d)\n"
                             "print(e)\n");
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
    XiFunc *f = lower_source("let arr = [1, 2, 3]\n"
                             "print(arr)\n");
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
    XiFunc *f = lower_source("let arr = [10, 20, 30]\n"
                             "let x = arr[1]\n"
                             "print(x)\n");
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
    XiFunc *f = lower_source("let arr = [1, 2, 3]\n"
                             "let n = arr.length\n"
                             "print(n)\n");
    assert(f != NULL);
    int found_load_field = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_LOAD_FIELD)
            found_load_field = 1;
    }
    assert(found_load_field && "should have LOAD_FIELD op");
    xi_func_free(f);
}

TEST(member_access_field_symbols_are_distinct) {
    XiFunc *f = lower_source("fn worker() -> int {\n"
                             "    yield\n"
                             "    return 1\n"
                             "}\n"
                             "let task = go worker()\n"
                             "print(task.done)\n"
                             "print(task.cancelled)\n");
    assert(f != NULL);

    int64_t done_symbol = 0;
    int64_t cancelled_symbol = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; blk && i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_LOAD_FIELD || !v->aux)
                continue;
            const char *name = (const char *) v->aux;
            if (strcmp(name, "done") == 0)
                done_symbol = v->aux_int;
            if (strcmp(name, "cancelled") == 0)
                cancelled_symbol = v->aux_int;
        }
    }

    assert(done_symbol > 0 && "task.done should carry a field symbol");
    assert(cancelled_symbol > 0 && "task.cancelled should carry a field symbol");
    assert(done_symbol != cancelled_symbol && "different task fields need distinct symbols");
    xi_func_free(f);
}

TEST(bytes_methods_lower_to_semantic_ops) {
    XiFunc *f = lower_source("let src = Bytes(8)\n"
                             "let dst = Bytes(8)\n"
                             "let a = src.loadU32LE(0)\n"
                             "let b = src.loadU64LE(0)\n"
                             "src.copyWithin(1, 0, 2)\n"
                             "dst.copyFrom(src, 0, 0, 2)\n"
                             "dst.repeatFrom(2, 2, 4)\n"
                             "print(a)\n"
                             "print(b)\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_BYTES_LOAD_U32_LE) && "loadU32LE should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_LOAD_U64_LE) && "loadU64LE should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_COPY_WITHIN) && "copyWithin should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_COPY_FROM) && "copyFrom should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_REPEAT_FROM) && "repeatFrom should lower to Bytes op");
    assert(!func_tree_has_builtin_name(f, "bytes_load_u32_le") &&
           "loadU32LE should not lower through string builtin");
    assert(!func_tree_has_builtin_name(f, "bytes_copy_within") &&
           "copyWithin should not lower through string builtin");
    assert(!func_tree_has_builtin_name(f, "bytes_repeat_from") &&
           "repeatFrom should not lower through string builtin");
    xi_func_free(f);
}

TEST(throw_stmt) {
    XiFunc *f = lower_source("enum LowerErr { Error }\n"
                             "let x = 1\n"
                             "if (x == 0) {\n"
                             "    throw LowerErr.Error\n"
                             "}\n"
                             "print(x)\n");
    assert(f != NULL);
    /* Should have: entry + then(throw) + else + merge */
    assert(f->nblocks >= 3);
    /* Find the throw block — should use XI_ERR_RETURN (value-return
     * error channel) and terminate with RETURN kind. */
    int found_err_return = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ERR_RETURN) {
                found_err_return = 1;
                assert(blk->kind == XI_BLOCK_RETURN);
            }
        }
    }
    assert(found_err_return && "should have ERR_RETURN op");
    xi_func_free(f);
}

TEST(for_in_loop) {
    XiFunc *f = lower_source("let arr = [10, 20, 30]\n"
                             "for (x in arr) {\n"
                             "    print(x)\n"
                             "}\n");
    assert(f != NULL);
    /* Should have: entry, cond, body, incr, exit blocks (loop structure) */
    assert(f->nblocks >= 4);
    /* Array for-in is desugared to index-based loop:
     *   LOAD_FIELD(.length), INDEX_GET, LT, ADD (increment) */
    int found_load_field = 0, found_index_get = 0, found_lt = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            uint16_t op = blk->values[i]->op;
            if (op == XI_LOAD_FIELD)
                found_load_field = 1;
            if (op == XI_INDEX_GET)
                found_index_get = 1;
            if (op == XI_LT)
                found_lt = 1;
        }
    }
    assert(found_load_field && "should have LOAD_FIELD for .length");
    assert(found_index_get && "should have INDEX_GET for coll[idx]");
    assert(found_lt && "should have LT for idx < len");
    xi_func_free(f);
}

TEST(nullish_coalesce) {
    XiFunc *f = lower_source("let x: int? = null\n"
                             "let y = x ?? 42\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Canonicalized to: x == null ? 42 : x → ternary with EQ null check.
     * Produces: entry, then_branch, else_branch, merge blocks. */
    assert(f->nblocks >= 3);
    /* Verify EQ op exists (null-check from canonicalized ternary) */
    int found_eq = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_EQ)
                found_eq = 1;
        }
    }
    assert(found_eq && "should have EQ op for null check");
    xi_func_free(f);
}

TEST(map_literal) {
    XiFunc *f = lower_source("let m = #{\"a\": 1, \"b\": 2}\n"
                             "print(m)\n");
    assert(f != NULL);
    int found_map_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_MAP_NEW)
            found_map_new = 1;
    }
    assert(found_map_new && "should have MAP_NEW op");
    xi_func_free(f);
}

TEST(match_expr) {
    XiFunc *f = lower_source("let x = 2\n"
                             "let y = match (x) {\n"
                             "    1 -> 10,\n"
                             "    2 -> 20,\n"
                             "    _ -> 0\n"
                             "}\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Match generates: chain of test blocks + body blocks + merge */
    assert(f->nblocks >= 3);
    /* Verify EQ comparisons for pattern matching */
    int found_eq = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_EQ)
                found_eq++;
        }
    }
    assert(found_eq >= 2 && "should have >= 2 EQ ops for pattern tests");
    xi_func_free(f);
}

TEST(try_catch) {
    XiFunc *f = lower_source("let result = 0\n"
                             "try {\n"
                             "    result = 42\n"
                             "} catch (e) {\n"
                             "    result = -1\n"
                             "}\n"
                             "print(result)\n");
    assert(f != NULL);
    /* try-catch generates: entry, try_blk, catch_blk, merge */
    assert(f->nblocks >= 3);
    /* New error model: catch block uses XI_ERR_CATCH to read the
     * error channel.  No XI_TRY/XI_CATCH (those are for panic). */
    int found_err_catch = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v->op == XI_ERR_CATCH)
                found_err_catch = 1;
        }
    }
    assert(found_err_catch && "should have XI_ERR_CATCH op");
    xi_func_free(f);
}

TEST(try_catch_defer) {
    XiFunc *f = lower_source("let x = 0\n"
                             "defer { x = 9 }\n"
                             "try {\n"
                             "    x = 1\n"
                             "} catch (e) {\n"
                             "    x = 2\n"
                             "}\n");
    assert(f != NULL);
    /* try + catch + merge = at least 3 blocks */
    assert(f->nblocks >= 3);
    xi_func_free(f);
}

TEST(object_literal) {
    XiFunc *f = lower_source("let obj = {a: 1, b: 2}\n"
                             "print(obj)\n");
    assert(f != NULL);
    int found_alloc = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_JSON_NEW)
            found_alloc = 1;
    }
    assert(found_alloc && "should have JSON_NEW for object literal");
    xi_func_free(f);
}

TEST(nested_function) {
    XiFunc *f = lower_source("fn add(a: int, b: int) -> int {\n"
                             "    return a + b\n"
                             "}\n"
                             "let r = add(1, 2)\n"
                             "print(r)\n");
    assert(f != NULL);
    /* Parent should have a child function */
    assert(f->nchildren == 1);
    XiFunc *child = f->children[0];
    assert(child != NULL);
    assert(child->nparams == 2);
    /* Child should have at least an ADD and a return */
    assert(child->nblocks >= 1);
    assert(child->entry->nvalues >= 1);
    /* Parent should have CLOSURE_NEW and CALL */
    int found_closure = 0, found_call = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_CLOSURE_NEW)
            found_closure = 1;
        if (f->entry->values[i]->op == XI_CALL)
            found_call = 1;
    }
    assert(found_closure && "parent should have CLOSURE_NEW");
    assert(found_call && "parent should have CALL");
    xi_func_free(f);
}

TEST(function_expr) {
    XiFunc *f = lower_source("let double = fn(x: int) -> int { return x * 2 }\n"
                             "let r = double(5)\n"
                             "print(r)\n");
    assert(f != NULL);
    assert(f->nchildren == 1);
    XiFunc *child = f->children[0];
    assert(child != NULL);
    assert(child->nparams == 1);
    xi_func_free(f);
}

TEST(multiple_functions) {
    XiFunc *f = lower_source("fn foo() -> int { return 1 }\n"
                             "fn bar() -> int { return 2 }\n"
                             "print(foo() + bar())\n");
    assert(f != NULL);
    assert(f->nchildren == 2);
    xi_func_free(f);
}

TEST(template_string) {
    XiFunc *f = lower_source("let name = \"world\"\n"
                             "let msg = \"hello ${name}!\"\n"
                             "print(msg)\n");
    assert(f != NULL);
    int found_concat = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_STR_CONCAT)
            found_concat = 1;
    }
    assert(found_concat && "should have STR_CONCAT for template string");
    xi_func_free(f);
}

TEST(go_await) {
    XiFunc *f = lower_source("fn work() -> int { return 42 }\n"
                             "let t = go work()\n"
                             "let r = await t\n"
                             "print(r)\n");
    assert(f != NULL);
    int found_go = 0, found_await = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_GO)
            found_go = 1;
        if (f->entry->values[i]->op == XI_AWAIT) {
            found_await = 1;
            assert((f->entry->values[i]->aux_int & XI_AWAIT_AUX_ONE_SHOT_GO) == 0 &&
                   "visible task await must not be one-shot");
        }
    }
    assert(found_go && "should have GO op");
    assert(found_await && "should have AWAIT op");
    xi_func_free(f);
}

TEST(direct_await_go_one_shot) {
    XiFunc *f = lower_source("fn work() -> int { return 42 }\n"
                             "let r = await go work()\n"
                             "print(r)\n");
    assert(f != NULL);
    int found_await = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_AWAIT) {
            found_await = 1;
            assert((f->entry->values[i]->aux_int & XI_AWAIT_AUX_ONE_SHOT_GO) != 0 &&
                   "direct await-go should be one-shot");
        }
    }
    assert(found_await && "should have AWAIT op");
    xi_func_free(f);
}

TEST(go_arg_transfer_modes) {
    XiFunc *copy_ir = lower_source("fn worker(xs: Array<int>) -> int { return xs.length }\n"
                                   "let xs = [1, 2]\n"
                                   "let task = go worker(copy(xs))\n"
                                   "print(await task)\n");
    assert(copy_ir != NULL);
    XiValue *copy_go = func_tree_find_op(copy_ir, XI_GO);
    assert(copy_go != NULL && "copy case should lower a GO op");
    assert(copy_go->nargs == 2 && "go worker(copy(xs)) should keep callee plus one arg");
    assert(xi_go_arg_transfer_mode(copy_go, 0) == XR_TRANSFER_COPY &&
           "copy(...) at a go boundary must be encoded as COPY transfer");
    assert(!func_tree_has_op(copy_ir, XI_COPY) &&
           "go boundary copy(...) must not lower to a separate copy op before GO");
    xi_func_free(copy_ir);

    XiFunc *move_ir = lower_source("fn worker(xs: Array<int>) -> int { return xs.length }\n"
                                   "shared let xs: Array<int> = [1, 2]\n"
                                   "let task = go worker(move xs)\n"
                                   "print(await task)\n");
    assert(move_ir != NULL);
    XiValue *move_go = func_tree_find_op(move_ir, XI_GO);
    assert(move_go != NULL && "move case should lower a GO op");
    assert(xi_go_arg_transfer_mode(move_go, 0) == XR_TRANSFER_MOVE &&
           "move at a go boundary must be encoded as MOVE transfer");
    assert(func_tree_has_op(move_ir, XI_MOVE) &&
           "move transfer should still consume source ownership");
    xi_func_free(move_ir);

    XiFunc *share_ir = lower_source("fn worker(xs: Array<int>) -> int { return xs.length }\n"
                                    "shared const xs: Array<int> = [1, 2]\n"
                                    "let task = go worker(xs)\n"
                                    "print(await task)\n");
    assert(share_ir != NULL);
    XiValue *share_go = func_tree_find_op(share_ir, XI_GO);
    assert(share_go != NULL && "shared const case should lower a GO op");
    assert(xi_go_arg_transfer_mode(share_go, 0) == XR_TRANSFER_SHARE &&
           "shared const go arguments should be encoded as zero-copy SHARE transfer");
    xi_func_free(share_ir);
}

TEST(defer_stmt) {
    XiFunc *f = lower_source("fn cleanup() { print(0) }\n"
                             "defer cleanup()\n"
                             "print(1)\n");
    assert(f != NULL);
    int found_defer = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_DEFER)
            found_defer = 1;
    }
    assert(found_defer && "should have DEFER op");
    xi_func_free(f);
}

TEST(defer_args_lower_before_defer) {
    XiFunc *f = lower_source("fn cleanup(msg: string, value: int) { print(msg); print(value) }\n"
                             "defer cleanup(\"result\", 42)\n"
                             "print(\"body\")\n");
    assert(f != NULL);
    XiBlock *blk = f->entry;
    XiValue *defer = NULL;
    uint32_t defer_index = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i]->op == XI_DEFER) {
            defer = blk->values[i];
            defer_index = i;
            break;
        }
    }
    assert(defer && "should have DEFER op");
    assert(defer->nargs == 3 && "defer should keep callee plus two args");
    for (uint16_t a = 1; a < defer->nargs; a++) {
        int found_before_defer = 0;
        for (uint32_t i = 0; i < defer_index; i++) {
            if (blk->values[i] == defer->args[a]) {
                found_before_defer = 1;
                break;
            }
        }
        assert(found_before_defer && "defer argument must be lowered before XI_DEFER");
    }
    xi_func_free(f);
}

TEST(set_literal) {
    XiFunc *f = lower_source("let s = #[1, 2, 3]\n"
                             "print(s)\n");
    assert(f != NULL);
    int found_set_new = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_SET_NEW)
            found_set_new = 1;
    }
    assert(found_set_new && "should have SET_NEW op");
    xi_func_free(f);
}

TEST(is_expr) {
    XiFunc *f = lower_source("let x = 42\n"
                             "let ok = x is int\n"
                             "print(ok)\n");
    assert(f != NULL);
    int found_is = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_IS)
            found_is = 1;
    }
    assert(found_is && "should have IS op");
    xi_func_free(f);
}

TEST(slice_expr) {
    XiFunc *f = lower_source("let arr = [1, 2, 3, 4]\n"
                             "let sub = arr[1:3]\n"
                             "print(sub)\n");
    assert(f != NULL);
    int found_slice = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_SLICE)
            found_slice = 1;
    }
    assert(found_slice && "should have SLICE op");
    xi_func_free(f);
}

TEST(range_expr) {
    XiFunc *f = lower_source("let r = 1..10\n"
                             "print(r)\n");
    assert(f != NULL);
    XiValue *range = func_tree_find_op(f, XI_RANGE);
    assert(range && "should have RANGE op");
    assert(range->aux_int == 0 && "half-open range should clear inclusive flag");
    xi_func_free(f);
}

TEST(range_inclusive_expr) {
    XiFunc *f = lower_source("let r = 1..=10\n"
                             "print(r)\n");
    assert(f != NULL);
    XiValue *range = func_tree_find_op(f, XI_RANGE);
    assert(range && "should have RANGE op");
    assert(range->aux_int == 1 && "inclusive range should set inclusive flag");
    xi_func_free(f);
}

TEST(optional_chain) {
    XiFunc *f = lower_source("let obj = {name: \"alice\"}\n"
                             "let n = obj?.name\n"
                             "print(n)\n");
    assert(f != NULL);
    /* Optional chain generates ISNULL + branch + merge */
    int found_isnull = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ISNULL)
                found_isnull = 1;
        }
    }
    assert(found_isnull && "should have ISNULL for optional chain");
    assert(f->nblocks >= 3 && "should have branch blocks for optional chain");
    xi_func_free(f);
}

TEST(optional_call) {
    XiFunc *f = lower_source("type IntFn = (int) -> int\n"
                             "fn bump(x: int) -> int { return x + 1 }\n"
                             "let fnv: IntFn? = bump\n"
                             "let n = fnv?.(41)\n"
                             "print(n)\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_ISNULL) && "optional call should null-check callee");
    assert(func_tree_has_op(f, XI_CALL) && "optional call should call only on non-null path");
    xi_func_free(f);
}

TEST(struct_literal) {
    XiFunc *f = lower_source("struct Point {\n"
                             "    x: float\n"
                             "    y: float\n"
                             "}\n"
                             "let p = Point{x: 1.0, y: 2.0}\n"
                             "print(p)\n");
    assert(f != NULL);
    int found_new = 0, found_set = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_STRUCT_NEW)
                found_new = 1;
            if (blk->values[i]->op == XI_STRUCT_SET)
                found_set = 1;
        }
    }
    assert(found_new && "struct literal should emit STRUCT_NEW");
    assert(found_set && "struct literal should set fields via STRUCT_SET");
    xi_func_free(f);
}

TEST(struct_literal_inside_function) {
    XiFunc *f = lower_source("struct Pair {\n"
                             "    a: int\n"
                             "    b: int\n"
                             "}\n"
                             "fn run() -> int {\n"
                             "    let p = Pair{a: 1, b: 2}\n"
                             "    return p.a + p.b\n"
                             "}\n"
                             "print(run())\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_STRUCT_NEW) &&
           "function-local struct literal should emit STRUCT_NEW");
    assert(func_tree_has_op(f, XI_STRUCT_SET) &&
           "function-local struct literal should set fields via STRUCT_SET");
    assert(func_tree_has_op(f, XI_STRUCT_GET) &&
           "function-local struct field access should emit STRUCT_GET");
    xi_func_free(f);
}

TEST(struct_field_store_narrows_native_width) {
    XiFunc *f = lower_source("struct Sample {\n"
                             "    byte: uint8\n"
                             "}\n"
                             "fn run() -> int {\n"
                             "    let p = Sample{byte: 300}\n"
                             "    p.byte = p.byte + 1\n"
                             "    return p.byte\n"
                             "}\n"
                             "print(run())\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_NARROW_U8) &&
           "uint8 struct field writes should narrow before storage");
    XiValue *narrow = func_tree_find_op(f, XI_NARROW_U8);
    assert(narrow && narrow->type && narrow->type->kind == XR_KIND_INT &&
           narrow->type->native_width == XR_NATIVE_U8 &&
           "NARROW_U8 result type should carry the target native width");
    assert(func_tree_has_op(f, XI_STRUCT_SET) && "struct field writes should use STRUCT_SET");
    xi_func_free(f);
}

TEST(force_unwrap) {
    XiFunc *f = lower_source("let x: int? = 42\n"
                             "let y = x!\n"
                             "print(y)\n");
    assert(f != NULL);
    /* Force unwrap generates ISNULL + branch */
    int found_isnull = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i]->op == XI_ISNULL)
                found_isnull = 1;
        }
    }
    assert(found_isnull && "should have ISNULL for force unwrap");
    assert(f->nblocks >= 3 && "force unwrap should create throw/ok branches");
    xi_func_free(f);
}

TEST(destructure_decl) {
    XiFunc *f = lower_source("let arr = [1, 2, 3]\n"
                             "let [a, b, c] = arr\n"
                             "print(a + b + c)\n");
    assert(f != NULL);
    /* Destructure should create INDEX_GET ops */
    int index_count = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_INDEX_GET)
            index_count++;
    }
    assert(index_count >= 3 && "should have 3 INDEX_GET for destructure");
    xi_func_free(f);
}

TEST(multi_assign) {
    XiFunc *f = lower_source("let a = 1\n"
                             "let b = 2\n"
                             "(a, b) = (b, a)\n"
                             "print(a)\n"
                             "print(b)\n");
    assert(f != NULL);
    xi_func_free(f);
}

TEST(enum_access) {
    XiFunc *f = lower_source("enum Color {\n"
                             "    Red,\n"
                             "    Green,\n"
                             "    Blue\n"
                             "}\n"
                             "let c = Color.Red\n"
                             "print(c)\n");
    assert(f != NULL);
    int found_load = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_LOAD_FIELD)
            found_load = 1;
    }
    assert(found_load && "should have LOAD_FIELD for enum access");
    xi_func_free(f);
}

TEST(import_export_skip) {
    XiFunc *f = lower_source("import \"math\" as math\n"
                             "let x = 42\n"
                             "print(x)\n");
    assert(f != NULL);
    /* Import is compile-time, should not generate any special ops */
    assert(f->entry->nvalues >= 2);
    xi_func_free(f);
}

TEST(class_decl_skip) {
    XiFunc *f = lower_source("class Dog {\n"
                             "    name: string\n"
                             "}\n"
                             "print(1)\n");
    assert(f != NULL);
    /* Class decl is compile-time, lowered body goes nowhere.
     * Main should still have print. */
    int found_print = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_PRINT)
            found_print = 1;
    }
    assert(found_print && "should still have print after class decl");
    xi_func_free(f);
}

TEST(yield_stmt) {
    XiFunc *f = lower_source("yield\n"
                             "print(1)\n");
    assert(f != NULL);
    int found_yield = 0;
    for (uint32_t i = 0; i < f->entry->nvalues; i++) {
        if (f->entry->values[i]->op == XI_YIELD)
            found_yield = 1;
    }
    assert(found_yield && "should have YIELD op");
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
    run_member_access_field_symbols_are_distinct();
    run_bytes_methods_lower_to_semantic_ops();
    run_throw_stmt();
    run_for_in_loop();
    run_nullish_coalesce();
    run_map_literal();
    run_match_expr();
    run_try_catch();
    run_try_catch_defer();
    run_object_literal();
    run_nested_function();
    run_function_expr();
    run_multiple_functions();
    run_template_string();
    run_go_await();
    run_direct_await_go_one_shot();
    run_go_arg_transfer_modes();
    run_defer_stmt();
    run_defer_args_lower_before_defer();
    run_set_literal();
    run_is_expr();
    run_slice_expr();
    run_range_expr();
    run_range_inclusive_expr();
    run_optional_chain();
    run_optional_call();
    run_struct_literal();
    run_struct_literal_inside_function();
    run_struct_field_store_narrows_native_width();
    run_force_unwrap();
    run_destructure_decl();
    run_multi_assign();
    run_enum_access();
    run_import_export_skip();
    run_class_decl_skip();
    run_yield_stmt();

    teardown();

    printf("=== %d/%d Xi Lower tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
