/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm.c - Unit tests for Xm core data structures
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_printer.h"
#include "../test_win_compat.h"

/* ========== Test: XmRef encoding ========== */

static void test_ref_encoding(void) {
    fprintf(stderr, "  test_ref_encoding...");

    // VREG
    XmRef v0 = XM_REF(XM_REF_VREG, 0);
    assert(XM_REF_KIND(v0) == XM_REF_VREG);
    assert(XM_REF_INDEX(v0) == 0);
    assert(xm_ref_is_vreg(v0));

    XmRef v42 = XM_REF(XM_REF_VREG, 42);
    assert(XM_REF_KIND(v42) == XM_REF_VREG);
    assert(XM_REF_INDEX(v42) == 42);

    // CONST
    XmRef c5 = XM_REF(XM_REF_CONST, 5);
    assert(XM_REF_KIND(c5) == XM_REF_CONST);
    assert(XM_REF_INDEX(c5) == 5);
    assert(xm_ref_is_const(c5));

    // NONE
    assert(xm_ref_is_none(XM_NONE));
    assert(XM_REF_KIND(XM_NONE) == XM_REF_NONE);

    // Large index
    XmRef big = XM_REF(XM_REF_VREG, (1 << 29) - 1);
    assert(XM_REF_INDEX(big) == (1u << 29) - 1);

    fprintf(stderr, " PASS\n");
}

/* ========== Test: Arena allocator ========== */

static void test_arena(void) {
    fprintf(stderr, "  test_arena...");

    XmArena arena;
    xm_arena_init(&arena);

    void *p1 = xm_arena_alloc(&arena, 64);
    assert(p1 != NULL);

    void *p2 = xm_arena_alloc(&arena, 128);
    assert(p2 != NULL);
    assert(p2 != p1);

    // Large allocation
    void *p3 = xm_arena_alloc(&arena, XM_ARENA_PAGE_SIZE * 2);
    assert(p3 != NULL);

    // Zero-init
    int *arr = (int *)xm_arena_calloc(&arena, 10, sizeof(int));
    assert(arr != NULL);
    for (int i = 0; i < 10; i++) {
        assert(arr[i] == 0);
    }

    xm_arena_destroy(&arena);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Function creation ========== */

static void test_func_create(void) {
    fprintf(stderr, "  test_func_create...");

    XmFunc *func = xm_func_new("test");
    assert(func != NULL);
    assert(strcmp(func->name, "test") == 0);
    assert(func->nblk == 0);
    assert(func->nvreg == 0);
    assert(func->nconst == 0);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Constants ========== */

static void test_constants(void) {
    fprintf(stderr, "  test_constants...");

    XmFunc *func = xm_func_new("const_test");

    XmRef c1 = xm_const_i64(func, 42);
    assert(xm_ref_is_const(c1));
    assert(func->nconst == 1);
    assert(func->consts[0].val.i64 == 42);

    // De-duplication
    XmRef c2 = xm_const_i64(func, 42);
    assert(c1 == c2);
    assert(func->nconst == 1);

    // Different value
    XmRef c3 = xm_const_i64(func, 100);
    assert(c3 != c1);
    assert(func->nconst == 2);

    // Float
    XmRef cf = xm_const_f64(func, 3.14);
    (void)cf;
    assert(func->nconst == 3);
    assert(func->consts[2].val.f64 == 3.14);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Basic block and instructions ========== */

static void test_emit_simple(void) {
    fprintf(stderr, "  test_emit_simple...");

    XmFunc *func = xm_func_new("simple");

    XmBlock *entry = xm_func_add_block(func, "entry");
    assert(entry != NULL);
    assert(func->nblk == 1);
    assert(func->entry == entry);

    // v0 = const 10
    XmRef c10 = xm_const_i64(func, 10);
    // v1 = const 20
    XmRef c20 = xm_const_i64(func, 20);

    // v2 = add v0, v1 (but we use const refs directly)
    // First load constants into vregs
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    assert(xm_ref_is_vreg(v0));
    assert(xm_ref_is_vreg(v1));
    assert(func->nvreg == 2);

    // v2 = add v0, v1
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    assert(func->nvreg == 3);
    assert(entry->nins == 3);

    // ret v2
    xm_block_set_ret(entry, v2);
    assert(entry->jmp.type == XM_JMP_RET);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Control flow (branch) ========== */

static void test_control_flow(void) {
    fprintf(stderr, "  test_control_flow...");

    XmFunc *func = xm_func_new("branch");

    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");
    XmBlock *merge = xm_func_add_block(func, "merge");
    assert(func->nblk == 4);

    // entry: cond = lt(param, 10); br cond, then, else
    XmRef param = xm_new_vreg(func, XR_REP_I64);
    XmRef c10 = xm_const_i64(func, 10);
    XmRef vc10 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef cond = xm_emit(func, entry, XM_LT, XR_REP_I64, param, vc10);
    xm_block_set_br(entry, cond, then_blk, else_blk);

    // then: v = 1; jmp merge
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v_then = xm_emit_unary(func, then_blk, XM_CONST_I64, XR_REP_I64, c1);
    then_blk->jmp.type = XM_JMP_JMP;
    then_blk->s1 = merge;

    // else: v = 0; jmp merge
    XmRef c0 = xm_const_i64(func, 0);
    XmRef v_else = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c0);
    else_blk->jmp.type = XM_JMP_JMP;
    else_blk->s1 = merge;

    // Set predecessors for merge
    xm_block_add_pred(merge, then_blk, func->arena);
    xm_block_add_pred(merge, else_blk, func->arena);

    // merge: phi(then:v_then, else:v_else); ret
    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    assert(phi != NULL);
    assert(phi->narg == 2);
    xm_phi_set_arg(phi, 0, v_then);
    xm_phi_set_arg(phi, 1, v_else);

    xm_block_set_ret(merge, phi->dst);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Test: Printer output ========== */

static void test_printer(void) {
    fprintf(stderr, "  test_printer...");

    XmFunc *func = xm_func_new("add_two");

    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_ret(entry, v2);

    // Print to stderr for visual inspection
    fprintf(stderr, "\n");
    xm_print_func(stderr, func);

    xm_func_destroy(func);
    fprintf(stderr, "  PASS\n");
}

/* ========== Test: Opcode info ========== */

static void test_opcode_info(void) {
    fprintf(stderr, "  test_opcode_info...");

    assert(strcmp(xm_op_name(XM_ADD), "add") == 0);
    assert(strcmp(xm_op_name(XM_RET), "ret") == 0);
    assert(strcmp(xm_op_name(XM_SAFEPOINT), "safepoint") == 0);

    assert(xm_op_is_commutative(XM_ADD));
    assert(xm_op_is_commutative(XM_MUL));
    assert(!xm_op_is_commutative(XM_SUB));
    assert(!xm_op_is_commutative(XM_DIV));

    assert(xm_op_has_side_effect(XM_STORE));
    assert(xm_op_has_side_effect(XM_CALL));
    assert(!xm_op_has_side_effect(XM_ADD));
    assert(!xm_op_has_side_effect(XM_LOAD));

    fprintf(stderr, " PASS\n");
}

/* ========== Test: Many vregs (grow) ========== */

static void test_vreg_growth(void) {
    fprintf(stderr, "  test_vreg_growth...");

    XmFunc *func = xm_func_new("growth");
    XmBlock *blk = xm_func_add_block(func, "entry");

    // Emit 200 instructions to test vreg and instruction array growth
    XmRef prev = xm_const_i64(func, 0);
    XmRef vprev = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, prev);
    for (int i = 0; i < 200; i++) {
        XmRef c1 = xm_const_i64(func, 1);
        vprev = xm_emit(func, blk, XM_ADD, XR_REP_I64, vprev, c1);
    }
    assert(func->nvreg == 201);
    assert(blk->nins == 201);

    xm_block_set_ret(blk, vprev);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm ===\n");

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
