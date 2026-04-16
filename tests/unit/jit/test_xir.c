/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir.c - Unit tests for XIR core data structures
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_printer.h"

/* ========== Test: XirRef encoding ========== */

static void test_ref_encoding(void) {
    fprintf(stderr, "  test_ref_encoding...");

    // VREG
    XirRef v0 = XIR_REF(XIR_REF_VREG, 0);
    assert(XIR_REF_KIND(v0) == XIR_REF_VREG);
    assert(XIR_REF_INDEX(v0) == 0);
    assert(xir_ref_is_vreg(v0));

    XirRef v42 = XIR_REF(XIR_REF_VREG, 42);
    assert(XIR_REF_KIND(v42) == XIR_REF_VREG);
    assert(XIR_REF_INDEX(v42) == 42);

    // CONST
    XirRef c5 = XIR_REF(XIR_REF_CONST, 5);
    assert(XIR_REF_KIND(c5) == XIR_REF_CONST);
    assert(XIR_REF_INDEX(c5) == 5);
    assert(xir_ref_is_const(c5));

    // NONE
    assert(xir_ref_is_none(XIR_NONE));
    assert(XIR_REF_KIND(XIR_NONE) == XIR_REF_NONE);

    // Large index
    XirRef big = XIR_REF(XIR_REF_VREG, (1 << 29) - 1);
    assert(XIR_REF_INDEX(big) == (1u << 29) - 1);

    fprintf(stderr, " PASS\n");
}

/* ========== Test: Arena allocator ========== */

static void test_arena(void) {
    fprintf(stderr, "  test_arena...");

    XirArena arena;
    xir_arena_init(&arena);

    void *p1 = xir_arena_alloc(&arena, 64);
    assert(p1 != NULL);

    void *p2 = xir_arena_alloc(&arena, 128);
    assert(p2 != NULL);
    assert(p2 != p1);

    // Large allocation
    void *p3 = xir_arena_alloc(&arena, XIR_ARENA_PAGE_SIZE * 2);
    assert(p3 != NULL);

    // Zero-init
    int *arr = (int *)xir_arena_calloc(&arena, 10, sizeof(int));
    assert(arr != NULL);
    for (int i = 0; i < 10; i++) {
        assert(arr[i] == 0);
    }

    xir_arena_destroy(&arena);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Function creation ========== */

static void test_func_create(void) {
    fprintf(stderr, "  test_func_create...");

    XirFunc *func = xir_func_new("test");
    assert(func != NULL);
    assert(strcmp(func->name, "test") == 0);
    assert(func->nblk == 0);
    assert(func->nvreg == 0);
    assert(func->nconst == 0);

    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constants ========== */

static void test_constants(void) {
    fprintf(stderr, "  test_constants...");

    XirFunc *func = xir_func_new("const_test");

    XirRef c1 = xir_const_i64(func, 42);
    assert(xir_ref_is_const(c1));
    assert(func->nconst == 1);
    assert(func->consts[0].val.i64 == 42);

    // De-duplication
    XirRef c2 = xir_const_i64(func, 42);
    assert(c1 == c2);
    assert(func->nconst == 1);

    // Different value
    XirRef c3 = xir_const_i64(func, 100);
    assert(c3 != c1);
    assert(func->nconst == 2);

    // Float
    XirRef cf = xir_const_f64(func, 3.14);
    (void)cf;
    assert(func->nconst == 3);
    assert(func->consts[2].val.f64 == 3.14);

    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Basic block and instructions ========== */

static void test_emit_simple(void) {
    fprintf(stderr, "  test_emit_simple...");

    XirFunc *func = xir_func_new("simple");

    XirBlock *entry = xir_func_add_block(func, "entry");
    assert(entry != NULL);
    assert(func->nblk == 1);
    assert(func->entry == entry);

    // v0 = const 10
    XirRef c10 = xir_const_i64(func, 10);
    // v1 = const 20
    XirRef c20 = xir_const_i64(func, 20);

    // v2 = add v0, v1 (but we use const refs directly)
    // First load constants into vregs
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c10);
    XirRef v1 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c20);
    assert(xir_ref_is_vreg(v0));
    assert(xir_ref_is_vreg(v1));
    assert(func->nvreg == 2);

    // v2 = add v0, v1
    XirRef v2 = xir_emit(func, entry, XIR_ADD, XR_REP_I64, v0, v1);
    assert(func->nvreg == 3);
    assert(entry->nins == 3);

    // ret v2
    xir_block_set_ret(entry, v2);
    assert(entry->jmp.type == XIR_JMP_RET);

    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Control flow (branch) ========== */

static void test_control_flow(void) {
    fprintf(stderr, "  test_control_flow...");

    XirFunc *func = xir_func_new("branch");

    XirBlock *entry = xir_func_add_block(func, "entry");
    XirBlock *then_blk = xir_func_add_block(func, "then");
    XirBlock *else_blk = xir_func_add_block(func, "else");
    XirBlock *merge = xir_func_add_block(func, "merge");
    assert(func->nblk == 4);

    // entry: cond = lt(param, 10); br cond, then, else
    XirRef param = xir_new_vreg(func, XR_REP_I64);
    XirRef c10 = xir_const_i64(func, 10);
    XirRef vc10 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c10);
    XirRef cond = xir_emit(func, entry, XIR_LT, XR_REP_I64, param, vc10);
    xir_block_set_br(entry, cond, then_blk, else_blk);

    // then: v = 1; jmp merge
    XirRef c1 = xir_const_i64(func, 1);
    XirRef v_then = xir_emit_unary(func, then_blk, XIR_CONST_I64, XR_REP_I64, c1);
    then_blk->jmp.type = XIR_JMP_JMP;
    then_blk->s1 = merge;

    // else: v = 0; jmp merge
    XirRef c0 = xir_const_i64(func, 0);
    XirRef v_else = xir_emit_unary(func, else_blk, XIR_CONST_I64, XR_REP_I64, c0);
    else_blk->jmp.type = XIR_JMP_JMP;
    else_blk->s1 = merge;

    // Set predecessors for merge
    xir_block_add_pred(merge, then_blk, func->arena);
    xir_block_add_pred(merge, else_blk, func->arena);

    // merge: phi(then:v_then, else:v_else); ret
    XirPhi *phi = xir_add_phi(func, merge, XR_REP_I64);
    assert(phi != NULL);
    assert(phi->narg == 2);
    xir_phi_set_arg(phi, 0, v_then);
    xir_phi_set_arg(phi, 1, v_else);

    xir_block_set_ret(merge, phi->dst);

    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Printer output ========== */

static void test_printer(void) {
    fprintf(stderr, "  test_printer...");

    XirFunc *func = xir_func_new("add_two");

    XirBlock *entry = xir_func_add_block(func, "entry");

    XirRef c10 = xir_const_i64(func, 10);
    XirRef c20 = xir_const_i64(func, 20);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c10);
    XirRef v1 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c20);
    XirRef v2 = xir_emit(func, entry, XIR_ADD, XR_REP_I64, v0, v1);
    xir_block_set_ret(entry, v2);

    // Print to stderr for visual inspection
    fprintf(stderr, "\n");
    xir_print_func(stderr, func);

    xir_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/* ========== Test: Opcode info ========== */

static void test_opcode_info(void) {
    fprintf(stderr, "  test_opcode_info...");

    assert(strcmp(xir_op_name(XIR_ADD), "add") == 0);
    assert(strcmp(xir_op_name(XIR_RET), "ret") == 0);
    assert(strcmp(xir_op_name(XIR_SAFEPOINT), "safepoint") == 0);

    assert(xir_op_is_commutative(XIR_ADD));
    assert(xir_op_is_commutative(XIR_MUL));
    assert(!xir_op_is_commutative(XIR_SUB));
    assert(!xir_op_is_commutative(XIR_DIV));

    assert(xir_op_has_side_effect(XIR_STORE));
    assert(xir_op_has_side_effect(XIR_CALL));
    assert(!xir_op_has_side_effect(XIR_ADD));
    assert(!xir_op_has_side_effect(XIR_LOAD));

    fprintf(stderr, " PASS\n");
}

/* ========== Test: Many vregs (grow) ========== */

static void test_vreg_growth(void) {
    fprintf(stderr, "  test_vreg_growth...");

    XirFunc *func = xir_func_new("growth");
    XirBlock *blk = xir_func_add_block(func, "entry");

    // Emit 200 instructions to test vreg and instruction array growth
    XirRef prev = xir_const_i64(func, 0);
    XirRef vprev = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, prev);
    for (int i = 0; i < 200; i++) {
        XirRef c1 = xir_const_i64(func, 1);
        vprev = xir_emit(func, blk, XIR_ADD, XR_REP_I64, vprev, c1);
    }
    assert(func->nvreg == 201);
    assert(blk->nins == 201);

    xir_block_set_ret(blk, vprev);

    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    fprintf(stderr, "=== test_xir ===\n");

    test_ref_encoding();
    test_arena();
    test_func_create();
    test_constants();
    test_emit_simple();
    test_control_flow();
    test_printer();
    test_opcode_info();
    test_vreg_growth();

    fprintf(stderr, "All tests passed!\n");
    return 0;
}
