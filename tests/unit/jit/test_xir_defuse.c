/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_defuse.c - Unit tests for def-use chains
 */

#include <stdio.h>
#include <assert.h>
#include "../../../src/jit/xir.h"
#include "../../../src/jit/xir_defuse.h"

/*
 * Test 1: Single block, each vreg used once.
 *
 *   v0 = CONST_I64(10)
 *   v1 = CONST_I64(20)
 *   v2 = ADD v0, v1
 *   RET v2
 *
 * Expected: v0 has 1 use (ADD arg0), v1 has 1 use (ADD arg1),
 *           v2 has 1 use (RET), const vregs have 0 extra uses
 */
static void test_single_block_uses(void) {
    fprintf(stderr, "  test_single_block_uses...");

    XirFunc *func = xir_func_new("du_single");
    XirBlock *entry = xir_func_add_block(func, "entry");

    XirRef c10 = xir_const_i64(func, 10);
    XirRef c20 = xir_const_i64(func, 20);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c10);
    XirRef v1 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c20);
    XirRef v2 = xir_emit(func, entry, XIR_ADD, XR_REP_I64, v0, v1);
    xir_block_set_ret(entry, v2);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    uint32_t i1 = XIR_REF_INDEX(v1);
    uint32_t i2 = XIR_REF_INDEX(v2);

    assert(xir_defuse_nuses(&du, i0) == 1);
    assert(xir_defuse_nuses(&du, i1) == 1);
    assert(xir_defuse_nuses(&du, i2) == 1);

    /* v0 used in ADD as arg0 */
    const XirUse *u0 = xir_defuse_uses(&du, i0);
    assert(u0 != NULL);
    assert(u0->kind == XIR_USE_INS_ARG);
    assert(u0->arg_idx == 0);

    /* v1 used in ADD as arg1 */
    const XirUse *u1 = xir_defuse_uses(&du, i1);
    assert(u1 != NULL);
    assert(u1->kind == XIR_USE_INS_ARG);
    assert(u1->arg_idx == 1);

    /* v2 used in RET */
    const XirUse *u2 = xir_defuse_uses(&du, i2);
    assert(u2 != NULL);
    assert(u2->kind == XIR_USE_JMP_ARG);

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 2: Vreg used multiple times.
 *
 *   v0 = CONST_I64(5)
 *   v1 = ADD v0, v0    ; v0 used twice
 *   RET v1
 *
 * Expected: v0 has 2 uses
 */
static void test_multi_use(void) {
    fprintf(stderr, "  test_multi_use...");

    XirFunc *func = xir_func_new("du_multi");
    XirBlock *entry = xir_func_add_block(func, "entry");

    XirRef c5 = xir_const_i64(func, 5);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c5);
    XirRef v1 = xir_emit(func, entry, XIR_ADD, XR_REP_I64, v0, v0);
    xir_block_set_ret(entry, v1);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    assert(xir_defuse_nuses(&du, i0) == 2);

    /* Both uses should be INS_ARG in the same instruction */
    const XirUse *uses = xir_defuse_uses(&du, i0);
    assert(uses[0].kind == XIR_USE_INS_ARG);
    assert(uses[1].kind == XIR_USE_INS_ARG);
    /* One arg0, one arg1 */
    assert((uses[0].arg_idx == 0 && uses[1].arg_idx == 1) ||
           (uses[0].arg_idx == 1 && uses[1].arg_idx == 0));

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 3: Dead vreg (no uses).
 *
 *   v0 = CONST_I64(42)
 *   v1 = CONST_I64(99)   ; dead — never used
 *   RET v0
 *
 * Expected: v1 has 0 uses, xir_defuse_is_dead returns true
 */
static void test_dead_vreg(void) {
    fprintf(stderr, "  test_dead_vreg...");

    XirFunc *func = xir_func_new("du_dead");
    XirBlock *entry = xir_func_add_block(func, "entry");

    XirRef c42 = xir_const_i64(func, 42);
    XirRef c99 = xir_const_i64(func, 99);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c42);
    XirRef v1 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c99);
    (void)v1;
    xir_block_set_ret(entry, v0);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i1 = XIR_REF_INDEX(v1);
    assert(xir_defuse_is_dead(&du, i1));
    assert(xir_defuse_nuses(&du, i1) == 0);
    assert(xir_defuse_uses(&du, i1) == NULL);

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 4: Phi node uses.
 *
 *   pred1: v0 = CONST_I64(1), JMP merge
 *   pred2: v1 = CONST_I64(2), JMP merge
 *   merge: v2 = phi(v0, v1), RET v2
 *
 * Expected: v0 has 1 use (phi arg), v1 has 1 use (phi arg),
 *           v2 has 1 use (RET)
 */
static void test_phi_uses(void) {
    fprintf(stderr, "  test_phi_uses...");

    XirFunc *func = xir_func_new("du_phi");
    XirBlock *pred1 = xir_func_add_block(func, "pred1");
    XirBlock *pred2 = xir_func_add_block(func, "pred2");
    XirBlock *merge = xir_func_add_block(func, "merge");

    XirRef c1 = xir_const_i64(func, 1);
    XirRef c2 = xir_const_i64(func, 2);
    XirRef v0 = xir_emit_unary(func, pred1, XIR_CONST_I64, XR_REP_I64, c1);
    XirRef v1 = xir_emit_unary(func, pred2, XIR_CONST_I64, XR_REP_I64, c2);

    xir_block_set_jmp(pred1, merge);
    xir_block_add_pred(merge, pred1, func->arena);
    xir_block_set_jmp(pred2, merge);
    xir_block_add_pred(merge, pred2, func->arena);

    XirPhi *phi = xir_add_phi(func, merge, XR_REP_I64);
    xir_phi_set_arg(phi, 0, v0);
    xir_phi_set_arg(phi, 1, v1);
    XirRef v2 = phi->dst;
    xir_block_set_ret(merge, v2);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    uint32_t i1 = XIR_REF_INDEX(v1);
    uint32_t i2 = XIR_REF_INDEX(v2);

    assert(xir_defuse_nuses(&du, i0) == 1);
    assert(xir_defuse_nuses(&du, i1) == 1);
    assert(xir_defuse_nuses(&du, i2) == 1);

    /* v0 used as phi arg */
    const XirUse *u0 = xir_defuse_uses(&du, i0);
    assert(u0->kind == XIR_USE_PHI_ARG);

    /* v1 used as phi arg */
    const XirUse *u1 = xir_defuse_uses(&du, i1);
    assert(u1->kind == XIR_USE_PHI_ARG);

    /* v2 used in RET */
    const XirUse *u2 = xir_defuse_uses(&du, i2);
    assert(u2->kind == XIR_USE_JMP_ARG);

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 5: Cross-block uses.
 *
 *   entry: v0 = CONST_I64(10), JMP body
 *   body:  v1 = ADD v0, v0, RET v1
 *
 * Expected: v0 has 2 uses in block 1 (body), v1 has 1 use (RET)
 */
static void test_cross_block(void) {
    fprintf(stderr, "  test_cross_block...");

    XirFunc *func = xir_func_new("du_cross");
    XirBlock *entry = xir_func_add_block(func, "entry");
    XirBlock *body = xir_func_add_block(func, "body");

    XirRef c10 = xir_const_i64(func, 10);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c10);
    xir_block_set_jmp(entry, body);
    xir_block_add_pred(body, entry, func->arena);

    XirRef v1 = xir_emit(func, body, XIR_ADD, XR_REP_I64, v0, v0);
    xir_block_set_ret(body, v1);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    uint32_t i1 = XIR_REF_INDEX(v1);

    assert(xir_defuse_nuses(&du, i0) == 2);
    assert(xir_defuse_nuses(&du, i1) == 1);

    /* Both uses of v0 should be in block 1 */
    const XirUse *uses = xir_defuse_uses(&du, i0);
    assert(uses[0].blk == 1);
    assert(uses[1].blk == 1);

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 6: single_use helper.
 *
 *   v0 = CONST_I64(7)
 *   v1 = NEG v0
 *   RET v1
 *
 * Expected: v0 single_use = true, v1 single_use = true
 */
static void test_single_use(void) {
    fprintf(stderr, "  test_single_use...");

    XirFunc *func = xir_func_new("du_single_use");
    XirBlock *entry = xir_func_add_block(func, "entry");

    XirRef c7 = xir_const_i64(func, 7);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c7);
    XirRef v1 = xir_emit_unary(func, entry, XIR_NEG, XR_REP_I64, v0);
    xir_block_set_ret(entry, v1);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    uint32_t i1 = XIR_REF_INDEX(v1);

    assert(xir_defuse_single_use(&du, i0));
    assert(xir_defuse_single_use(&du, i1));

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 7: BR terminator condition is counted as a use.
 *
 *   entry: v0 = CONST_I64(1), BR v0 then else
 *   then:  RET v0
 *   else:  RET v0
 *
 * Expected: v0 has 3 uses: BR cond + RET(then) + RET(else)
 */
static void test_br_cond_use(void) {
    fprintf(stderr, "  test_br_cond_use...");

    XirFunc *func = xir_func_new("du_br");
    XirBlock *entry = xir_func_add_block(func, "entry");
    XirBlock *then_blk = xir_func_add_block(func, "then");
    XirBlock *else_blk = xir_func_add_block(func, "else");

    XirRef c1 = xir_const_i64(func, 1);
    XirRef v0 = xir_emit_unary(func, entry, XIR_CONST_I64, XR_REP_I64, c1);
    xir_block_set_br(entry, v0, then_blk, else_blk);
    xir_block_add_pred(then_blk, entry, func->arena);
    xir_block_add_pred(else_blk, entry, func->arena);

    xir_block_set_ret(then_blk, v0);
    xir_block_set_ret(else_blk, v0);

    XirDefUse du;
    xir_defuse_build(&du, func);

    uint32_t i0 = XIR_REF_INDEX(v0);
    assert(xir_defuse_nuses(&du, i0) == 3);

    /* Count use kinds */
    const XirUse *uses = xir_defuse_uses(&du, i0);
    int jmp_count = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (uses[i].kind == XIR_USE_JMP_ARG) jmp_count++;
    }
    assert(jmp_count == 3); /* BR cond + 2 RETs */

    xir_defuse_free(&du);
    xir_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    fprintf(stderr, "=== test_xir_defuse ===\n");

    test_single_block_uses();
    test_multi_use();
    test_dead_vreg();
    test_phi_uses();
    test_cross_block();
    test_single_use();
    test_br_cond_use();

    fprintf(stderr, "All 7 tests passed!\n");
    return 0;
}
