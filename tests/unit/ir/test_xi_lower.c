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
#include "../../../include/xray_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ========== Test Infrastructure ========== */

static XrVMRuntime *g_iso = NULL;
static int tests_passed = 0;
static int tests_failed = 0;

static void setup(void) {
    if (!g_iso) {
        XrVMConfig p;
        xray_vm_config_init(&p);
        g_iso = xray_vm_new_full(&p);
    }
}

static void teardown(void) {
    if (g_iso) {
        xray_vm_delete(g_iso);
        g_iso = NULL;
    }
}

/* Parse source, run analyzer, lower to Xi, dump and return XiFunc.
 * Caller must call xi_func_free() on the result. */
static XiFunc *lower_source(const char *source) {
    assert(g_iso != NULL);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);

    /* Parse */
    AstNode *program = xr_parse(session, source);
    if (!program) {
        fprintf(stderr, "  PARSE FAILED for: %s\n", source);
        return NULL;
    }

    /* Analyze */
    XaAnalyzer *analyzer = xa_analyzer_new(session);
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
        xr_compiler_session_push_arena(session, program->as.program.arena, "test.xr", &canon_scope);

    /* Canonicalize + Lower */
    xr_canon_program(program, analyzer, session);
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

static XiValue *func_tree_find_method(XiFunc *f, const char *name) {
    if (!f || !name)
        return NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->op == XI_CALL_METHOD && v->aux && strcmp((const char *) v->aux, name) == 0)
                return v;
        }
    }
    for (uint16_t i = 0; i < f->nchildren; i++) {
        XiValue *v = func_tree_find_method(f->children[i], name);
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
                             "    Coro.yield()\n"
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

TEST(bytes_new_low_level_methods_lower_to_semantic_ops) {
    XiFunc *f = lower_source("let src = Bytes(8)\n"
                             "let view: ByteSpan = src\n"
                             "let dst = Bytes.withCapacity(8)\n"
                             "let h = view.loadLE<uint16>(0)\n"
                             "let a = view.loadLE<uint32>(0)\n"
                             "let b = view.loadLE<uint64>(0)\n"
                             "unsafe {\n"
                             "    dst.appendFromUnchecked(view, 0, 2)\n"
                             "    dst.repeatFromUnchecked(2, 4)\n"
                             "}\n"
                             "print(h)\n"
                             "print(a)\n"
                             "print(b)\n");
    assert(f != NULL);
    assert(func_tree_has_op(f, XI_BYTES_LOAD_U16_LE) && "loadLE<uint16> should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_LOAD_U32_LE) && "loadLE<uint32> should lower to Bytes op");
    assert(func_tree_has_op(f, XI_BYTES_LOAD_U64_LE) && "loadLE<uint64> should lower to Bytes op");
    assert(func_tree_find_method(f, "appendFromUnchecked") &&
           "appendFromUnchecked should remain an explicit method call");
    assert(func_tree_find_method(f, "repeatFromUnchecked") &&
           "repeatFromUnchecked should remain an explicit method call");
    assert(!func_tree_has_builtin_name(f, "bytes_load_u16_le") &&
           !func_tree_has_builtin_name(f, "bytes_load_u32_le") &&
           "loadLE should not lower through string builtin");
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

TEST(channel_send_transfer_modes) {
    XiFunc *copy_ir = lower_source("let ch: Channel<Array<int>> = Channel(1)\n"
                                   "let xs = [1, 2]\n"
                                   "ch.send(copy(xs))\n");
    assert(copy_ir != NULL);
    XiValue *copy_send = func_tree_find_op(copy_ir, XI_CHAN_SEND);
    assert(copy_send != NULL && "copy case should lower to CHAN_SEND");
    assert(xi_chan_send_transfer_mode(copy_send) == XR_TRANSFER_COPY &&
           "copy(...) at a channel send boundary must be encoded as COPY transfer");
    assert(!func_tree_has_op(copy_ir, XI_COPY) &&
           "channel boundary copy(...) must not lower to a separate copy op before send");
    xi_func_free(copy_ir);

    XiFunc *move_ir = lower_source("let ch: Channel<Array<int>> = Channel(1)\n"
                                   "let xs = [1, 2]\n"
                                   "ch.send(move xs)\n");
    assert(move_ir != NULL);
    XiValue *move_send = func_tree_find_op(move_ir, XI_CHAN_SEND);
    assert(move_send != NULL && "move case should lower to CHAN_SEND");
    assert(xi_chan_send_transfer_mode(move_send) == XR_TRANSFER_MOVE &&
           "move at a channel send boundary must be encoded as MOVE transfer");
    assert(func_tree_has_op(move_ir, XI_MOVE) &&
           "move transfer should still consume source ownership");
    xi_func_free(move_ir);

    XiFunc *try_ir = lower_source("let ch: Channel<Array<int>> = Channel(1)\n"
                                  "let xs = [1, 2]\n"
                                  "print(ch.trySend(copy(xs)))\n");
    assert(try_ir != NULL);
    XiValue *try_send = func_tree_find_method(try_ir, "trySend");
    assert(try_send != NULL && "copy trySend should lower to CALL_METHOD");
    assert(xi_chan_send_transfer_mode(try_send) == XR_TRANSFER_COPY &&
           "copy(...) at trySend boundary must be encoded as COPY transfer");
    xi_func_free(try_ir);

    XiFunc *timeout_ir = lower_source("let ch: Channel<Array<int>> = Channel(1)\n"
                                      "let xs = [1, 2]\n"
                                      "print(ch.sendTimeout(copy(xs), 0))\n");
    assert(timeout_ir != NULL);
    XiValue *timeout_send = func_tree_find_method(timeout_ir, "sendTimeout");
    assert(timeout_send != NULL && "copy sendTimeout should lower to CALL_METHOD");
    assert(xi_chan_send_transfer_mode(timeout_send) == XR_TRANSFER_COPY &&
           "copy(...) at sendTimeout boundary must be encoded as COPY transfer");
    xi_func_free(timeout_ir);
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
    XiFunc *f = lower_source("Coro.yield()\n"
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

TEST(parallel_for_lowers_to_dedicated_ir) {
#define REQUIRE_PAR_FOR(cond, msg)                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_for_lowers_to_dedicated_ir: %s\n", msg);                     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    let base = 3\n"
                             "    parallel for i in 0..n workers 2 worker wid {\n"
                             "        print(i + wid + base)\n"
                             "    }\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_FOR(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_FOR);
    REQUIRE_PAR_FOR(par != NULL, "PAR_FOR should be present");
    REQUIRE_PAR_FOR(par->nargs >= 4, "PAR_FOR should start with start/end/workers/closure args");
    REQUIRE_PAR_FOR(par->aux_kind == XI_AUX_KIND_PAR_FOR, "PAR_FOR aux kind should be explicit");
    REQUIRE_PAR_FOR(par->args[0] && par->args[1] && par->args[2] && par->args[3],
                    "PAR_FOR args should be populated");
    REQUIRE_PAR_FOR(par->args[3]->op == XI_CLOSURE_NEW, "PAR_FOR body should be a closure");

    XiParallelForData *data = (XiParallelForData *) par->aux;
    REQUIRE_PAR_FOR(data != NULL, "PAR_FOR should carry XiParallelForData");
    REQUIRE_PAR_FOR(data->body_func != NULL, "parallel body func should be recorded");
    REQUIRE_PAR_FOR(par->nargs == 4 + data->body_func->ncaptures,
                    "PAR_FOR should append captured values as hidden lifetime anchors");
    REQUIRE_PAR_FOR(data->item_name && strcmp(data->item_name, "i") == 0,
                    "loop item name should be recorded");
    REQUIRE_PAR_FOR(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                    "worker name should be recorded");
    REQUIRE_PAR_FOR(data->body_func->nparams == 2,
                    "body func should take loop item and worker params");
    REQUIRE_PAR_FOR(data->body_func->params[0] != NULL, "body param should be populated");
    REQUIRE_PAR_FOR(data->body_func->params[0]->op == XI_PARAM, "body param should be PARAM");
    REQUIRE_PAR_FOR(data->body_func->params[1] != NULL, "worker param should be populated");
    REQUIRE_PAR_FOR(data->body_func->params[1]->op == XI_PARAM, "worker param should be PARAM");
    REQUIRE_PAR_FOR(data->body_func->native_callback_kind == XI_NATIVE_CALLBACK_PAR_FOR_I64,
                    "parallel body should use the native runtime callback ABI");
    REQUIRE_PAR_FOR((XiFunc *) par->args[3]->aux == data->body_func,
                    "closure should point at body func");
    REQUIRE_PAR_FOR(data->body_func->ncaptures >= 1, "body should capture outer base");
    xi_func_free(f);

#undef REQUIRE_PAR_FOR
}

TEST(parallel_for_final_lowers_to_range_body_ir) {
#define REQUIRE_PAR_FINAL(cond, msg)                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_for_final_lowers_to_range_body_ir: %s\n", msg);              \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    parallel for i in 0..n workers 2 worker wid\n"
                             "        final { print(wid) } {\n"
                             "        print(i)\n"
                             "    }\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_FINAL(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_FOR);
    REQUIRE_PAR_FINAL(par != NULL, "PAR_FOR should be present");
    XiParallelForData *data = (XiParallelForData *) par->aux;
    REQUIRE_PAR_FINAL(data != NULL, "PAR_FOR should carry XiParallelForData");
    REQUIRE_PAR_FINAL(data->range_body, "parallel final should force range_body lowering");
    REQUIRE_PAR_FINAL(data->body_func != NULL, "parallel final body func should be recorded");
    REQUIRE_PAR_FINAL(data->body_func->nparams == 3,
                      "final body should take begin, end and worker params");
    REQUIRE_PAR_FINAL(data->body_func->native_callback_kind == XI_NATIVE_CALLBACK_PAR_RANGE_I64,
                      "parallel final body should use the native range callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_FINAL
}

TEST(parallel_range_lowers_to_range_body_ir) {
#define REQUIRE_PAR_RANGE(cond, msg)                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_range_lowers_to_range_body_ir: %s\n", msg);                  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    let base = 3\n"
                             "    parallel range begin, end in 0..n workers 2 worker wid {\n"
                             "        print(begin + end + wid + base)\n"
                             "    }\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_RANGE(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_FOR);
    REQUIRE_PAR_RANGE(par != NULL, "PAR_FOR should be present");
    XiParallelForData *data = (XiParallelForData *) par->aux;
    REQUIRE_PAR_RANGE(data != NULL, "PAR_FOR should carry XiParallelForData");
    REQUIRE_PAR_RANGE(data->range_body, "parallel range should mark range_body");
    REQUIRE_PAR_RANGE(data->body_func != NULL, "parallel range body func should be recorded");
    REQUIRE_PAR_RANGE(data->body_func->nparams == 3,
                      "range body should take begin, end and worker params");
    REQUIRE_PAR_RANGE(data->item_name && strcmp(data->item_name, "begin") == 0,
                      "range begin name should be recorded");
    REQUIRE_PAR_RANGE(data->end_name && strcmp(data->end_name, "end") == 0,
                      "range end name should be recorded");
    REQUIRE_PAR_RANGE(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                      "range worker name should be recorded");
    REQUIRE_PAR_RANGE(data->body_func->params[0] && data->body_func->params[0]->op == XI_PARAM,
                      "begin param should be populated");
    REQUIRE_PAR_RANGE(data->body_func->params[1] && data->body_func->params[1]->op == XI_PARAM,
                      "end param should be populated");
    REQUIRE_PAR_RANGE(data->body_func->params[2] && data->body_func->params[2]->op == XI_PARAM,
                      "worker param should be populated");
    REQUIRE_PAR_RANGE(data->body_func->native_callback_kind == XI_NATIVE_CALLBACK_PAR_RANGE_I64,
                      "range body should use the native range callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_RANGE
}

TEST(parallel_reduce_lowers_to_dedicated_ir) {
#define REQUIRE_PAR_REDUCE(cond, msg)                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_reduce_lowers_to_dedicated_ir: %s\n", msg);                  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) -> int {\n"
                             "    let base = 3\n"
                             "    return parallel reduce i in 0..n workers 2 worker wid init 10 "
                             "combine (a: int, b: int) -> a + b {\n"
                             "        i + wid - wid + base\n"
                             "    }\n"
                             "}\n"
                             "print(run(4))\n");
    REQUIRE_PAR_REDUCE(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_REDUCE);
    REQUIRE_PAR_REDUCE(par != NULL, "PAR_REDUCE should be present");
    REQUIRE_PAR_REDUCE(par->nargs >= 6,
                       "PAR_REDUCE should start with start/end/workers/init/body/combine args");
    REQUIRE_PAR_REDUCE(par->aux_kind == XI_AUX_KIND_PAR_REDUCE,
                       "PAR_REDUCE aux kind should be explicit");
    for (uint16_t i = 0; i < 6; i++)
        REQUIRE_PAR_REDUCE(par->args[i] != NULL, "PAR_REDUCE leading args should be populated");
    REQUIRE_PAR_REDUCE(par->args[4]->op == XI_CLOSURE_NEW, "PAR_REDUCE body should be a closure");
    REQUIRE_PAR_REDUCE(par->args[5]->op == XI_CLOSURE_NEW,
                       "PAR_REDUCE combine should be a closure");

    XiParallelReduceData *data = (XiParallelReduceData *) par->aux;
    REQUIRE_PAR_REDUCE(data != NULL, "PAR_REDUCE should carry XiParallelReduceData");
    REQUIRE_PAR_REDUCE(data->accumulator_type && XR_TYPE_IS_INT(data->accumulator_type),
                       "int parallel reduce should record an int accumulator type");
    REQUIRE_PAR_REDUCE(par->type && XR_TYPE_IS_INT(par->type),
                       "int parallel reduce value should keep int result type");
    REQUIRE_PAR_REDUCE(data->body_func != NULL, "parallel reduce body func should be recorded");
    REQUIRE_PAR_REDUCE(data->combine_func != NULL,
                       "parallel reduce combine func should be recorded");
    REQUIRE_PAR_REDUCE((XiFunc *) par->args[4]->aux == data->body_func,
                       "body closure should point at body func");
    REQUIRE_PAR_REDUCE((XiFunc *) par->args[5]->aux == data->combine_func,
                       "combine closure should point at combine func");
    REQUIRE_PAR_REDUCE(par->nargs == 6 + data->body_func->ncaptures + data->combine_func->ncaptures,
                       "PAR_REDUCE should append captured values as hidden lifetime anchors");
    REQUIRE_PAR_REDUCE(data->item_name && strcmp(data->item_name, "i") == 0,
                       "loop item name should be recorded");
    REQUIRE_PAR_REDUCE(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                       "worker name should be recorded");
    REQUIRE_PAR_REDUCE(data->body_func->nparams == 2,
                       "body func should take item and worker params");
    REQUIRE_PAR_REDUCE(data->combine_func->nparams == 2,
                       "combine func should take accumulator and value params");
    REQUIRE_PAR_REDUCE(!data->range_body,
                       "item parallel reduce should not be marked as range body");
    REQUIRE_PAR_REDUCE(data->end_name == NULL,
                       "item parallel reduce should not record a range end name");
    REQUIRE_PAR_REDUCE(data->body_func->params[0] && data->body_func->params[0]->op == XI_PARAM,
                       "body item param should be PARAM");
    REQUIRE_PAR_REDUCE(data->body_func->params[1] && data->body_func->params[1]->op == XI_PARAM,
                       "body worker param should be PARAM");
    REQUIRE_PAR_REDUCE(data->combine_func->params[0] &&
                           data->combine_func->params[0]->op == XI_PARAM,
                       "combine accumulator param should be PARAM");
    REQUIRE_PAR_REDUCE(data->combine_func->params[1] &&
                           data->combine_func->params[1]->op == XI_PARAM,
                       "combine value param should be PARAM");
    REQUIRE_PAR_REDUCE(data->body_func->native_callback_kind ==
                           XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY,
                       "parallel reduce body should use the native i64 callback ABI");
    REQUIRE_PAR_REDUCE(data->combine_func->native_callback_kind ==
                           XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE,
                       "parallel reduce combine should use the native i64 callback ABI");
    REQUIRE_PAR_REDUCE(data->body_func->ncaptures >= 1, "body should capture outer base");
    REQUIRE_PAR_REDUCE(data->combine_func->ncaptures == 0,
                       "uncaptured combine should not manufacture captures");
    xi_func_free(f);

#undef REQUIRE_PAR_REDUCE
}

TEST(parallel_range_reduce_lowers_to_range_body_ir) {
#define REQUIRE_PAR_RANGE_REDUCE(cond, msg)                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_range_reduce_lowers_to_range_body_ir: %s\n", msg);           \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f =
        lower_source("fn run(n: int) -> int {\n"
                     "    let base = 3\n"
                     "    return parallel range reduce begin, end in 0..n workers 2 worker wid "
                     "init 0 combine (a: int, b: int) -> a + b {\n"
                     "        end - begin + wid - wid + base - base\n"
                     "    }\n"
                     "}\n"
                     "print(run(4))\n");
    REQUIRE_PAR_RANGE_REDUCE(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_REDUCE);
    REQUIRE_PAR_RANGE_REDUCE(par != NULL, "PAR_REDUCE should be present");
    REQUIRE_PAR_RANGE_REDUCE(par->aux_kind == XI_AUX_KIND_PAR_REDUCE,
                             "PAR_REDUCE aux kind should be explicit");

    XiParallelReduceData *data = (XiParallelReduceData *) par->aux;
    REQUIRE_PAR_RANGE_REDUCE(data != NULL, "PAR_REDUCE should carry reduce metadata");
    REQUIRE_PAR_RANGE_REDUCE(data->range_body, "range reduce should mark range_body");
    REQUIRE_PAR_RANGE_REDUCE(data->item_name && strcmp(data->item_name, "begin") == 0,
                             "range reduce begin name should be recorded");
    REQUIRE_PAR_RANGE_REDUCE(data->end_name && strcmp(data->end_name, "end") == 0,
                             "range reduce end name should be recorded");
    REQUIRE_PAR_RANGE_REDUCE(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                             "range reduce worker name should be recorded");
    REQUIRE_PAR_RANGE_REDUCE(data->body_func != NULL, "range reduce body func should exist");
    REQUIRE_PAR_RANGE_REDUCE(data->combine_func != NULL, "range reduce combine func should exist");
    REQUIRE_PAR_RANGE_REDUCE(data->body_func->nparams == 3,
                             "range reduce body should take begin, end and worker params");
    REQUIRE_PAR_RANGE_REDUCE(data->combine_func->nparams == 2,
                             "combine should take accumulator and value params");
    for (uint16_t i = 0; i < data->body_func->nparams; i++)
        REQUIRE_PAR_RANGE_REDUCE(data->body_func->params[i] &&
                                     data->body_func->params[i]->op == XI_PARAM,
                                 "range reduce body params should be PARAM values");
    REQUIRE_PAR_RANGE_REDUCE(data->body_func->native_callback_kind ==
                                 XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY,
                             "range reduce body should keep the native i64 callback ABI");
    REQUIRE_PAR_RANGE_REDUCE(data->combine_func->native_callback_kind ==
                                 XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE,
                             "range reduce combine should keep the native i64 callback ABI");
    REQUIRE_PAR_RANGE_REDUCE(par->type && XR_TYPE_IS_INT(par->type),
                             "range reduce value should keep int result type");
    xi_func_free(f);

#undef REQUIRE_PAR_RANGE_REDUCE
}

TEST(parallel_reduce_local_init_lowers_to_range_body_ir) {
#define REQUIRE_PAR_REDUCE_LOCAL(cond, msg)                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_reduce_local_init_lowers_to_range_body_ir: %s\n", msg);      \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) -> int {\n"
                             "    let base = 3\n"
                             "    return parallel reduce i in 0..n workers 2 worker wid\n"
                             "        local acc = base + wid\n"
                             "        init 0\n"
                             "        combine (a: int, b: int) -> a + b {\n"
                             "        acc = acc + i\n"
                             "        acc\n"
                             "    }\n"
                             "}\n"
                             "print(run(4))\n");
    REQUIRE_PAR_REDUCE_LOCAL(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_REDUCE);
    REQUIRE_PAR_REDUCE_LOCAL(par != NULL, "PAR_REDUCE should be present");
    REQUIRE_PAR_REDUCE_LOCAL(par->aux_kind == XI_AUX_KIND_PAR_REDUCE,
                             "PAR_REDUCE aux kind should be explicit");

    XiParallelReduceData *data = (XiParallelReduceData *) par->aux;
    REQUIRE_PAR_REDUCE_LOCAL(data != NULL, "PAR_REDUCE should carry reduce metadata");
    REQUIRE_PAR_REDUCE_LOCAL(data->range_body,
                             "local-init item reduce should lower as a worker range body");
    REQUIRE_PAR_REDUCE_LOCAL(data->item_name && strcmp(data->item_name, "i") == 0,
                             "item name should remain the user item variable");
    REQUIRE_PAR_REDUCE_LOCAL(data->end_name == NULL,
                             "synthetic range end should not expose a user end name");
    REQUIRE_PAR_REDUCE_LOCAL(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                             "worker name should be recorded");
    REQUIRE_PAR_REDUCE_LOCAL(data->body_func != NULL, "reduce body func should exist");
    REQUIRE_PAR_REDUCE_LOCAL(data->combine_func != NULL, "combine func should exist");
    REQUIRE_PAR_REDUCE_LOCAL(data->body_func->nparams == 3,
                             "local-init reduce body should take begin, end and worker params");
    REQUIRE_PAR_REDUCE_LOCAL(data->combine_func->nparams == 2,
                             "combine func should take accumulator and value params");
    for (uint16_t i = 0; i < data->body_func->nparams; i++)
        REQUIRE_PAR_REDUCE_LOCAL(data->body_func->params[i] &&
                                     data->body_func->params[i]->op == XI_PARAM,
                                 "local-init reduce body params should be PARAM values");
    REQUIRE_PAR_REDUCE_LOCAL(data->body_func->native_callback_kind ==
                                 XI_NATIVE_CALLBACK_PAR_REDUCE_I64_BODY,
                             "body should keep the native i64 reduce callback ABI");
    REQUIRE_PAR_REDUCE_LOCAL(data->combine_func->native_callback_kind ==
                                 XI_NATIVE_CALLBACK_PAR_REDUCE_I64_COMBINE,
                             "combine should keep the native i64 reduce callback ABI");
    REQUIRE_PAR_REDUCE_LOCAL(func_tree_find_op(data->body_func, XI_CALL) != NULL,
                             "synthetic range body should call the combine function locally");
    REQUIRE_PAR_REDUCE_LOCAL(par->type && XR_TYPE_IS_INT(par->type),
                             "local-init reduce value should keep int result type");
    xi_func_free(f);

#undef REQUIRE_PAR_REDUCE_LOCAL
}

TEST(parallel_collect_lowers_to_dedicated_ir) {
#define REQUIRE_PAR_COLLECT(cond, msg)                                                             \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_collect_lowers_to_dedicated_ir: %s\n", msg);                 \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f =
        lower_source("fn run(n: int) -> int {\n"
                     "    let base = 3\n"
                     "    let values = parallel for i in 0..n workers 2 worker wid collect {\n"
                     "        i + wid + base\n"
                     "    }\n"
                     "    return values[0] + values[3]\n"
                     "}\n"
                     "print(run(4))\n");
    REQUIRE_PAR_COLLECT(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_COLLECT);
    REQUIRE_PAR_COLLECT(par != NULL, "PAR_COLLECT should be present");
    REQUIRE_PAR_COLLECT(par->nargs >= 4,
                        "PAR_COLLECT should start with start/end/workers/closure args");
    REQUIRE_PAR_COLLECT(par->aux_kind == XI_AUX_KIND_PAR_COLLECT,
                        "PAR_COLLECT aux kind should be explicit");
    for (uint16_t i = 0; i < 4; i++)
        REQUIRE_PAR_COLLECT(par->args[i] != NULL, "PAR_COLLECT leading args should be populated");
    REQUIRE_PAR_COLLECT(par->args[3]->op == XI_CLOSURE_NEW, "PAR_COLLECT body should be a closure");
    REQUIRE_PAR_COLLECT(par->type && XR_TYPE_IS_ARRAY(par->type),
                        "PAR_COLLECT result should be an array");
    REQUIRE_PAR_COLLECT(par->type->container.element_type &&
                            XR_TYPE_IS_INT(par->type->container.element_type),
                        "PAR_COLLECT int body should produce Array<int>");

    XiParallelCollectData *data = (XiParallelCollectData *) par->aux;
    REQUIRE_PAR_COLLECT(data != NULL, "PAR_COLLECT should carry XiParallelCollectData");
    REQUIRE_PAR_COLLECT(data->element_type && XR_TYPE_IS_INT(data->element_type),
                        "int parallel collect should record int element type");
    REQUIRE_PAR_COLLECT(data->body_func != NULL, "parallel collect body func should be recorded");
    REQUIRE_PAR_COLLECT((XiFunc *) par->args[3]->aux == data->body_func,
                        "body closure should point at body func");
    REQUIRE_PAR_COLLECT(par->nargs == 4 + data->body_func->ncaptures,
                        "PAR_COLLECT should append captured values as hidden lifetime anchors");
    REQUIRE_PAR_COLLECT(data->item_name && strcmp(data->item_name, "i") == 0,
                        "loop item name should be recorded");
    REQUIRE_PAR_COLLECT(data->worker_name && strcmp(data->worker_name, "wid") == 0,
                        "worker name should be recorded");
    REQUIRE_PAR_COLLECT(data->body_func->nparams == 2,
                        "body func should take item and worker params");
    REQUIRE_PAR_COLLECT(data->body_func->return_type &&
                            XR_TYPE_IS_INT(data->body_func->return_type),
                        "body func should return int");
    REQUIRE_PAR_COLLECT(data->body_func->native_callback_kind ==
                            XI_NATIVE_CALLBACK_PAR_COLLECT_SCALAR_BODY,
                        "parallel collect body should use the native scalar callback ABI");
    REQUIRE_PAR_COLLECT(data->body_func->ncaptures >= 1, "body should capture outer base");
    xi_func_free(f);

#undef REQUIRE_PAR_COLLECT
}

TEST(parallel_collect_into_lowers_to_dedicated_ir) {
#define REQUIRE_PAR_COLLECT_INTO(cond, msg)                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_collect_into_lowers_to_dedicated_ir: %s\n", msg);            \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    let out: Array<int> = []\n"
                             "    let base = 7\n"
                             "    parallel for i in 0..n workers 2 worker wid collect into out {\n"
                             "        i + wid + base\n"
                             "    }\n"
                             "    print(out.length)\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_COLLECT_INTO(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_COLLECT);
    REQUIRE_PAR_COLLECT_INTO(par != NULL, "PAR_COLLECT should be present");
    REQUIRE_PAR_COLLECT_INTO(par->aux_kind == XI_AUX_KIND_PAR_COLLECT,
                             "PAR_COLLECT aux kind should be explicit");

    XiParallelCollectData *data = (XiParallelCollectData *) par->aux;
    REQUIRE_PAR_COLLECT_INTO(data != NULL, "PAR_COLLECT should carry data");
    REQUIRE_PAR_COLLECT_INTO(data->into_result, "collect into should be marked in aux data");
    REQUIRE_PAR_COLLECT_INTO(data->direct_lane_writes,
                             "scalar collect into should write the result lane in the body");
    REQUIRE_PAR_COLLECT_INTO(par->type && XR_TYPE_IS_UNIT(par->type),
                             "collect into should return unit");
    REQUIRE_PAR_COLLECT_INTO(par->nargs >= 5, "collect into should carry target array arg");
    REQUIRE_PAR_COLLECT_INTO(par->args[4] && par->args[4]->type &&
                                 XR_TYPE_IS_ARRAY(par->args[4]->type),
                             "arg 4 should be the result buffer array");
    REQUIRE_PAR_COLLECT_INTO(data->element_type && XR_TYPE_IS_INT(data->element_type),
                             "collect into should record element type from target array");
    REQUIRE_PAR_COLLECT_INTO(data->body_func != NULL, "body func should be recorded");
    REQUIRE_PAR_COLLECT_INTO(par->nargs == 5 + data->body_func->ncaptures,
                             "captures should start after the explicit result buffer arg");
    REQUIRE_PAR_COLLECT_INTO(data->body_func->return_type &&
                                 XR_TYPE_IS_UNIT(data->body_func->return_type),
                             "direct collect into body should return unit");
    REQUIRE_PAR_COLLECT_INTO(data->body_func->native_callback_kind ==
                                 XI_NATIVE_CALLBACK_PAR_FOR_I64,
                             "direct collect into body should use the native range callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_COLLECT_INTO
}

TEST(parallel_collect_final_lowers_to_range_body_ir) {
#define REQUIRE_PAR_COLLECT_FINAL(cond, msg)                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_collect_final_lowers_to_range_body_ir: %s\n", msg);          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    let values = parallel for i in 0..n workers 2 worker wid\n"
                             "        final { print(wid) }\n"
                             "        collect {\n"
                             "        i + wid\n"
                             "    }\n"
                             "    print(values.length)\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_COLLECT_FINAL(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_COLLECT);
    REQUIRE_PAR_COLLECT_FINAL(par != NULL, "PAR_COLLECT should be present");
    REQUIRE_PAR_COLLECT_FINAL(par->aux_kind == XI_AUX_KIND_PAR_COLLECT,
                              "PAR_COLLECT aux kind should be explicit");

    XiParallelCollectData *data = (XiParallelCollectData *) par->aux;
    REQUIRE_PAR_COLLECT_FINAL(data != NULL, "PAR_COLLECT should carry data");
    REQUIRE_PAR_COLLECT_FINAL(!data->into_result, "returning collect should produce an array");
    REQUIRE_PAR_COLLECT_FINAL(data->direct_lane_writes,
                              "collect final should use direct result lane writes");
    REQUIRE_PAR_COLLECT_FINAL(data->body_func != NULL, "body func should be recorded");
    REQUIRE_PAR_COLLECT_FINAL(data->body_func->nparams == 3,
                              "collect final body should take begin, end and worker params");
    REQUIRE_PAR_COLLECT_FINAL(data->body_func->native_callback_kind ==
                                  XI_NATIVE_CALLBACK_PAR_RANGE_I64,
                              "collect final body should use the native range callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_COLLECT_FINAL
}

TEST(parallel_collect_into_reference_lane_uses_direct_ir) {
#define REQUIRE_PAR_COLLECT_REF(cond, msg)                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_collect_into_reference_lane_uses_direct_ir: %s\n", msg);     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("fn run(n: int) {\n"
                             "    let out: Array<string> = []\n"
                             "    let label = \"chunk\"\n"
                             "    parallel for i in 0..n workers 2 collect into out {\n"
                             "        label\n"
                             "    }\n"
                             "    print(out.length)\n"
                             "}\n"
                             "run(4)\n");
    REQUIRE_PAR_COLLECT_REF(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_COLLECT);
    REQUIRE_PAR_COLLECT_REF(par != NULL, "PAR_COLLECT should be present");
    REQUIRE_PAR_COLLECT_REF(par->aux_kind == XI_AUX_KIND_PAR_COLLECT,
                            "PAR_COLLECT aux kind should be explicit");

    XiParallelCollectData *data = (XiParallelCollectData *) par->aux;
    REQUIRE_PAR_COLLECT_REF(data != NULL, "PAR_COLLECT should carry data");
    REQUIRE_PAR_COLLECT_REF(data->into_result, "collect into should be marked in aux data");
    REQUIRE_PAR_COLLECT_REF(data->direct_lane_writes,
                            "reference collect into should write the result lane in the body");
    REQUIRE_PAR_COLLECT_REF(par->type && XR_TYPE_IS_UNIT(par->type),
                            "collect into should return unit");
    REQUIRE_PAR_COLLECT_REF(par->nargs >= 5, "collect into should carry target array arg");
    REQUIRE_PAR_COLLECT_REF(data->element_type && XR_TYPE_IS_STRING(data->element_type),
                            "collect into should record string element type from target array");
    REQUIRE_PAR_COLLECT_REF(data->body_func != NULL, "body func should be recorded");
    REQUIRE_PAR_COLLECT_REF(data->body_func->return_type &&
                                XR_TYPE_IS_UNIT(data->body_func->return_type),
                            "direct reference collect into body should return unit");
    REQUIRE_PAR_COLLECT_REF(data->body_func->native_callback_kind == XI_NATIVE_CALLBACK_PAR_FOR_I64,
                            "direct reference collect into body should use range callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_COLLECT_REF
}

TEST(parallel_reduce_struct_accumulator_keeps_typed_ir) {
#define REQUIRE_PAR_REDUCE_STRUCT(cond, msg)                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "parallel_reduce_struct_accumulator_keeps_typed_ir: %s\n", msg);       \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

    XiFunc *f = lower_source("struct Totals {\n"
                             "    bytes: int\n"
                             "    checksum: int\n"
                             "}\n"
                             "fn run(n: int) -> int {\n"
                             "    let totals = parallel reduce i in 0..n workers 2 worker wid "
                             "init Totals{bytes: 0, checksum: 0} "
                             "combine (a: Totals, b: Totals) -> Totals{"
                             "bytes: a.bytes + b.bytes, "
                             "checksum: a.checksum + b.checksum} {\n"
                             "        Totals{bytes: i, checksum: wid}\n"
                             "    }\n"
                             "    return totals.bytes + totals.checksum\n"
                             "}\n"
                             "print(run(4))\n");
    REQUIRE_PAR_REDUCE_STRUCT(f != NULL, "source should lower");
    XiValue *par = func_tree_find_op(f, XI_PAR_REDUCE);
    REQUIRE_PAR_REDUCE_STRUCT(par != NULL, "PAR_REDUCE should be present");
    XiParallelReduceData *data = (XiParallelReduceData *) par->aux;
    REQUIRE_PAR_REDUCE_STRUCT(data != NULL, "PAR_REDUCE should carry reduce metadata");
    REQUIRE_PAR_REDUCE_STRUCT(data->accumulator_type != NULL,
                              "struct reduce should record accumulator type");
    REQUIRE_PAR_REDUCE_STRUCT(!XR_TYPE_IS_INT(data->accumulator_type),
                              "struct reduce accumulator must not collapse to int");
    REQUIRE_PAR_REDUCE_STRUCT(par->type == data->accumulator_type ||
                                  xr_type_equals(par->type, data->accumulator_type),
                              "PAR_REDUCE result should use the accumulator type");
    REQUIRE_PAR_REDUCE_STRUCT(data->body_func && data->combine_func,
                              "struct reduce should still build body/combine funcs");
    REQUIRE_PAR_REDUCE_STRUCT(
        data->body_func->return_type &&
            xr_type_assignable(data->accumulator_type, data->body_func->return_type),
        "body func should return the accumulator type");
    REQUIRE_PAR_REDUCE_STRUCT(
        data->combine_func->return_type &&
            xr_type_assignable(data->accumulator_type, data->combine_func->return_type),
        "combine func should return the accumulator type");
    REQUIRE_PAR_REDUCE_STRUCT(
        data->combine_func->params[0] &&
            xr_type_assignable(data->accumulator_type, data->combine_func->params[0]->type),
        "combine accumulator param should use accumulator type");
    REQUIRE_PAR_REDUCE_STRUCT(data->body_func->native_callback_kind ==
                                  XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_BODY,
                              "struct reduce body should use the native aggregate callback ABI");
    REQUIRE_PAR_REDUCE_STRUCT(data->combine_func->native_callback_kind ==
                                  XI_NATIVE_CALLBACK_PAR_REDUCE_AGG_COMBINE,
                              "struct combine should use the native aggregate callback ABI");
    xi_func_free(f);

#undef REQUIRE_PAR_REDUCE_STRUCT
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
    run_bytes_new_low_level_methods_lower_to_semantic_ops();
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
    run_channel_send_transfer_modes();
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
    run_parallel_for_lowers_to_dedicated_ir();
    run_parallel_for_final_lowers_to_range_body_ir();
    run_parallel_range_lowers_to_range_body_ir();
    run_parallel_reduce_lowers_to_dedicated_ir();
    run_parallel_range_reduce_lowers_to_range_body_ir();
    run_parallel_reduce_local_init_lowers_to_range_body_ir();
    run_parallel_collect_lowers_to_dedicated_ir();
    run_parallel_collect_into_lowers_to_dedicated_ir();
    run_parallel_collect_final_lowers_to_range_body_ir();
    run_parallel_collect_into_reference_lane_uses_direct_ir();
    run_parallel_reduce_struct_accumulator_keeps_typed_ir();

    teardown();

    printf("=== %d/%d Xi Lower tests passed ===\n", tests_passed, tests_passed + tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
