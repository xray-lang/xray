/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_defuse.c - Unit tests for def-use chains
 */

#include <stdio.h>
#include <assert.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_defuse.h"
#include "../test_win_compat.h"

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

    XmFunc *func = xm_func_new("du_single");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_ret(entry, v2);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    uint32_t i1 = XM_REF_INDEX(v1);
    uint32_t i2 = XM_REF_INDEX(v2);

    assert(xm_defuse_nuses(&du, i0) == 1);
    assert(xm_defuse_nuses(&du, i1) == 1);
    assert(xm_defuse_nuses(&du, i2) == 1);

    /* v0 used in ADD as arg0 */
    const XmUse *u0 = xm_defuse_uses(&du, i0);
    assert(u0 != NULL);
    assert(u0->kind == XM_USE_INS_ARG);
    assert(u0->arg_idx == 0);

    /* v1 used in ADD as arg1 */
    const XmUse *u1 = xm_defuse_uses(&du, i1);
    assert(u1 != NULL);
    assert(u1->kind == XM_USE_INS_ARG);
    assert(u1->arg_idx == 1);

    /* v2 used in RET */
    const XmUse *u2 = xm_defuse_uses(&du, i2);
    assert(u2 != NULL);
    assert(u2->kind == XM_USE_JMP_ARG);

    xm_defuse_free(&du);
    xm_func_destroy(func);
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

    XmFunc *func = xm_func_new("du_multi");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c5 = xm_const_i64(func, 5);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c5);
    XmRef v1 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v0);
    xm_block_set_ret(entry, v1);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    assert(xm_defuse_nuses(&du, i0) == 2);

    /* Both uses should be INS_ARG in the same instruction */
    const XmUse *uses = xm_defuse_uses(&du, i0);
    assert(uses[0].kind == XM_USE_INS_ARG);
    assert(uses[1].kind == XM_USE_INS_ARG);
    /* One arg0, one arg1 */
    assert((uses[0].arg_idx == 0 && uses[1].arg_idx == 1) ||
           (uses[0].arg_idx == 1 && uses[1].arg_idx == 0));

    xm_defuse_free(&du);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 3: Dead vreg (no uses).
 *
 *   v0 = CONST_I64(42)
 *   v1 = CONST_I64(99)   ; dead â€?never used
 *   RET v0
 *
 * Expected: v1 has 0 uses, xm_defuse_is_dead returns true
 */
static void test_dead_vreg(void) {
    fprintf(stderr, "  test_dead_vreg...");

    XmFunc *func = xm_func_new("du_dead");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c42 = xm_const_i64(func, 42);
    XmRef c99 = xm_const_i64(func, 99);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c99);
    (void) v1;
    xm_block_set_ret(entry, v0);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i1 = XM_REF_INDEX(v1);
    assert(xm_defuse_is_dead(&du, i1));
    assert(xm_defuse_nuses(&du, i1) == 0);
    assert(xm_defuse_uses(&du, i1) == NULL);

    xm_defuse_free(&du);
    xm_func_destroy(func);
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

    XmFunc *func = xm_func_new("du_phi");
    XmBlock *pred1 = xm_func_add_block(func, "pred1");
    XmBlock *pred2 = xm_func_add_block(func, "pred2");
    XmBlock *merge = xm_func_add_block(func, "merge");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef c2 = xm_const_i64(func, 2);
    XmRef v0 = xm_emit_unary(func, pred1, XM_CONST_I64, XR_REP_I64, c1);
    XmRef v1 = xm_emit_unary(func, pred2, XM_CONST_I64, XR_REP_I64, c2);

    xm_block_set_jmp(pred1, merge);
    xm_block_add_pred(merge, pred1, func->arena);
    xm_block_set_jmp(pred2, merge);
    xm_block_add_pred(merge, pred2, func->arena);

    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    xm_phi_set_arg(phi, 0, v0);
    xm_phi_set_arg(phi, 1, v1);
    XmRef v2 = phi->dst;
    xm_block_set_ret(merge, v2);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    uint32_t i1 = XM_REF_INDEX(v1);
    uint32_t i2 = XM_REF_INDEX(v2);

    assert(xm_defuse_nuses(&du, i0) == 1);
    assert(xm_defuse_nuses(&du, i1) == 1);
    assert(xm_defuse_nuses(&du, i2) == 1);

    /* v0 used as phi arg */
    const XmUse *u0 = xm_defuse_uses(&du, i0);
    assert(u0->kind == XM_USE_PHI_ARG);

    /* v1 used as phi arg */
    const XmUse *u1 = xm_defuse_uses(&du, i1);
    assert(u1->kind == XM_USE_PHI_ARG);

    /* v2 used in RET */
    const XmUse *u2 = xm_defuse_uses(&du, i2);
    assert(u2->kind == XM_USE_JMP_ARG);

    xm_defuse_free(&du);
    xm_func_destroy(func);
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

    XmFunc *func = xm_func_new("du_cross");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *body = xm_func_add_block(func, "body");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_jmp(entry, body);
    xm_block_add_pred(body, entry, func->arena);

    XmRef v1 = xm_emit(func, body, XM_ADD, XR_REP_I64, v0, v0);
    xm_block_set_ret(body, v1);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    uint32_t i1 = XM_REF_INDEX(v1);

    assert(xm_defuse_nuses(&du, i0) == 2);
    assert(xm_defuse_nuses(&du, i1) == 1);

    /* Both uses of v0 should be in block 1 */
    const XmUse *uses = xm_defuse_uses(&du, i0);
    assert(uses[0].blk == 1);
    assert(uses[1].blk == 1);

    xm_defuse_free(&du);
    xm_func_destroy(func);
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

    XmFunc *func = xm_func_new("du_single_use");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c7 = xm_const_i64(func, 7);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c7);
    XmRef v1 = xm_emit_unary(func, entry, XM_NEG, XR_REP_I64, v0);
    xm_block_set_ret(entry, v1);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    uint32_t i1 = XM_REF_INDEX(v1);

    assert(xm_defuse_single_use(&du, i0));
    assert(xm_defuse_single_use(&du, i1));

    xm_defuse_free(&du);
    xm_func_destroy(func);
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

    XmFunc *func = xm_func_new("du_br");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    xm_block_set_ret(then_blk, v0);
    xm_block_set_ret(else_blk, v0);

    XmDefUse du;
    xm_defuse_build(&du, func);

    uint32_t i0 = XM_REF_INDEX(v0);
    assert(xm_defuse_nuses(&du, i0) == 3);

    /* Count use kinds */
    const XmUse *uses = xm_defuse_uses(&du, i0);
    int jmp_count = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (uses[i].kind == XM_USE_JMP_ARG)
            jmp_count++;
    }
    assert(jmp_count == 3); /* BR cond + 2 RETs */

    xm_defuse_free(&du);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm_defuse ===\n");

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
