/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_pass.c - Unit tests for Xm optimization passes
 *   (copy_prop, sccp, phi_simp)
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_pass.h"
#include "../../../src/jit/xm_pass_sccp.h"
#include "../../../src/jit/xm_looptree.h"
#include "../test_win_compat.h"

/* ========== Copy Propagation Tests ========== */

/*
 * Test: MOV chain is propagated through.
 *
 *   v0 = CONST_I64(42)
 *   v1 = MOV v0
 *   v2 = MOV v1
 *   v3 = ADD v2, v2
 *   RET v3
 *
 * After copy_prop: v3 = ADD v0, v0 (v1, v2 usage eliminated)
 */
static void test_copy_prop_chain(void) {
    fprintf(stderr, "  test_copy_prop_chain...");

    XmFunc *func = xm_func_new("cp_chain");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    XmRef v1 = xm_emit_unary(func, entry, XM_MOV, XR_REP_I64, v0);
    XmRef v2 = xm_emit_unary(func, entry, XM_MOV, XR_REP_I64, v1);
    XmRef v3 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v2, v2);
    xm_block_set_ret(entry, v3);

    xm_pass_copy_prop(func);

    /* After copy_prop, the ADD's args should both be v0 (the root) */
    XmIns *add_ins = NULL;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XM_ADD) {
            add_ins = &entry->ins[i];
            break;
        }
    }
    assert(add_ins != NULL);
    assert(add_ins->args[0] == v0);
    assert(add_ins->args[1] == v0);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: copy_prop does not touch non-MOV definitions.
 *
 *   v0 = CONST_I64(1)
 *   v1 = CONST_I64(2)
 *   v2 = ADD v0, v1
 *   RET v2
 *
 * After copy_prop: no changes (no MOV instructions).
 */
static void test_copy_prop_no_mov(void) {
    fprintf(stderr, "  test_copy_prop_no_mov...");

    XmFunc *func = xm_func_new("cp_nomov");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef c2 = xm_const_i64(func, 2);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c2);
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_ret(entry, v2);

    uint32_t nins_before = entry->nins;
    xm_pass_copy_prop(func);
    assert(entry->nins == nins_before);

    /* ADD still uses v0, v1 */
    XmIns *add_ins = &entry->ins[2];
    assert(add_ins->op == XM_ADD);
    assert(add_ins->args[0] == v0);
    assert(add_ins->args[1] == v1);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Branch Simplification Tests ========== */

/*
 * Test: BR with constant true condition â†?JMP to true branch.
 *
 *   entry:
 *     v0 = CONST_I64(1)   ; true
 *     BR v0, then, else
 *
 * After branch_simp: JMP then
 */
static void test_branch_simp_const_true(void) {
    fprintf(stderr, "  test_branch_simp_const_true...");

    XmFunc *func = xm_func_new("bs_true");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);

    /* Add simple returns to then/else */
    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef vt = xm_emit_unary(func, then_blk, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_ret(then_blk, vt);
    XmRef ve = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c20);
    xm_block_set_ret(else_blk, ve);

    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    xm_pass_sccp(func);

    assert(entry->jmp.type == XM_JMP_JMP);
    assert(entry->s1 == then_blk);
    assert(entry->s2 == NULL);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: BR with constant false condition â†?JMP to false branch.
 */
static void test_branch_simp_const_false(void) {
    fprintf(stderr, "  test_branch_simp_const_false...");

    XmFunc *func = xm_func_new("bs_false");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef c0 = xm_const_i64(func, 0);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef vt = xm_emit_unary(func, then_blk, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_ret(then_blk, vt);
    XmRef ve = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c20);
    xm_block_set_ret(else_blk, ve);

    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    xm_pass_sccp(func);

    assert(entry->jmp.type == XM_JMP_JMP);
    assert(entry->s1 == else_blk);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: BR with both targets the same â†?JMP.
 */
static void test_branch_simp_same_target(void) {
    fprintf(stderr, "  test_branch_simp_same_target...");

    XmFunc *func = xm_func_new("bs_same");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *merge = xm_func_add_block(func, "merge");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);

    XmRef c42 = xm_const_i64(func, 42);
    XmRef vm = xm_emit_unary(func, merge, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(merge, vm);

    /* BR where s1 == s2 */
    xm_block_set_br(entry, v0, merge, merge);
    xm_block_add_pred(merge, entry, func->arena);

    xm_pass_sccp(func);

    assert(entry->jmp.type == XM_JMP_JMP);
    assert(entry->s1 == merge);
    assert(entry->s2 == NULL);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Unreachable Block Elimination Tests ========== */

/*
 * Test: After branch_simp removes a branch, remove_unreachable
 * deletes the dead block.
 *
 *   entry: BR(const 1) then else
 *   â†?entry: JMP then   (else becomes unreachable)
 *   â†?remove_unreachable: nblk decreases
 */
static void test_remove_unreachable_basic(void) {
    fprintf(stderr, "  test_remove_unreachable_basic...");

    XmFunc *func = xm_func_new("ru_basic");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef vt = xm_emit_unary(func, then_blk, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_ret(then_blk, vt);
    XmRef ve = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c20);
    xm_block_set_ret(else_blk, ve);

    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    assert(func->nblk == 3);

    /* SCCP folds BR(1) â†?JMP then and removes unreachable else_blk */
    xm_pass_sccp(func);
    assert(entry->jmp.type == XM_JMP_JMP);
    assert(func->nblk == 2);

    /* Verify block IDs are compacted */
    assert(func->blocks[0]->id == 0);
    assert(func->blocks[1]->id == 1);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Phi Simplification Tests ========== */

/*
 * Test: Trivial phi where all args are the same â†?MOV.
 *
 *   header:
 *     phi(v0, v0) = v1    ; all args = v0
 *     ...
 *
 * After phi_simp: phi removed, MOV v1 = v0 inserted at block start.
 */
static void test_phi_simp_trivial(void) {
    fprintf(stderr, "  test_phi_simp_trivial...");

    XmFunc *func = xm_func_new("ps_trivial");
    XmBlock *pred1 = xm_func_add_block(func, "pred1");
    XmBlock *pred2 = xm_func_add_block(func, "pred2");
    XmBlock *merge = xm_func_add_block(func, "merge");

    /* Create a vreg that both preds will pass to the phi */
    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, pred1, XM_CONST_I64, XR_REP_I64, c42);

    /* Wire preds â†?merge */
    xm_block_set_jmp(pred1, pred2);
    xm_block_add_pred(pred2, pred1, func->arena);
    xm_block_set_jmp(pred2, merge);
    xm_block_add_pred(merge, pred1, func->arena);
    xm_block_add_pred(merge, pred2, func->arena);

    /* Create phi with both args = v0 */
    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    assert(phi != NULL);
    xm_phi_set_arg(phi, 0, v0);
    xm_phi_set_arg(phi, 1, v0);
    XmRef phi_dst = phi->dst;

    xm_block_set_ret(merge, phi_dst);

    assert(merge->phis != NULL);
    uint32_t nins_before = merge->nins;

    xm_pass_phi_simp(func);

    /* Phi should be removed, MOV inserted */
    assert(merge->phis == NULL);
    assert(merge->nins == nins_before + 1);
    assert(merge->ins[0].op == XM_MOV);
    assert(merge->ins[0].dst == phi_dst);
    assert(merge->ins[0].args[0] == v0);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: Non-trivial phi (different args) is NOT simplified.
 */
static void test_phi_simp_nontrivial(void) {
    fprintf(stderr, "  test_phi_simp_nontrivial...");

    XmFunc *func = xm_func_new("ps_nontrivial");
    XmBlock *pred1 = xm_func_add_block(func, "pred1");
    XmBlock *pred2 = xm_func_add_block(func, "pred2");
    XmBlock *merge = xm_func_add_block(func, "merge");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef c2 = xm_const_i64(func, 2);
    XmRef v0 = xm_emit_unary(func, pred1, XM_CONST_I64, XR_REP_I64, c1);
    XmRef v1 = xm_emit_unary(func, pred2, XM_CONST_I64, XR_REP_I64, c2);

    xm_block_set_jmp(pred1, pred2);
    xm_block_add_pred(pred2, pred1, func->arena);
    xm_block_set_jmp(pred2, merge);
    xm_block_add_pred(merge, pred1, func->arena);
    xm_block_add_pred(merge, pred2, func->arena);

    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    xm_phi_set_arg(phi, 0, v0);
    xm_phi_set_arg(phi, 1, v1); /* different from v0! */

    xm_block_set_ret(merge, phi->dst);

    xm_pass_phi_simp(func);

    /* Phi should NOT be removed (args differ) */
    assert(merge->phis != NULL);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Block Merging Tests ========== */

/*
 * Test: Two blocks Aâ†’B where B has single pred â†?merged into one block.
 *
 *   A: v0 = CONST_I64(1)
 *      JMP B
 *   B: v1 = CONST_I64(2)
 *      v2 = ADD v0, v1
 *      RET v2
 *
 * After merge_blocks: A contains all instructions, B removed.
 */
static void test_merge_blocks_basic(void) {
    fprintf(stderr, "  test_merge_blocks_basic...");

    XmFunc *func = xm_func_new("mb_basic");
    XmBlock *a = xm_func_add_block(func, "a");
    XmBlock *b = xm_func_add_block(func, "b");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef c2 = xm_const_i64(func, 2);
    XmRef v0 = xm_emit_unary(func, a, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(a, b);
    xm_block_add_pred(b, a, func->arena);

    XmRef v1 = xm_emit_unary(func, b, XM_CONST_I64, XR_REP_I64, c2);
    XmRef v2 = xm_emit(func, b, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_ret(b, v2);

    assert(func->nblk == 2);
    uint32_t total_ins = a->nins + b->nins;

    xm_pass_merge_blocks(func);

    /* Should merge into 1 block */
    assert(func->nblk == 1);
    assert(func->blocks[0]->nins == total_ins);
    assert(func->blocks[0]->jmp.type == XM_JMP_RET);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: Block with multiple predecessors is NOT merged.
 */
static void test_merge_blocks_multi_pred(void) {
    fprintf(stderr, "  test_merge_blocks_multi_pred...");

    XmFunc *func = xm_func_new("mb_multi");
    XmBlock *a = xm_func_add_block(func, "a");
    XmBlock *b = xm_func_add_block(func, "b");
    XmBlock *c = xm_func_add_block(func, "c");

    XmRef c1 = xm_const_i64(func, 1);
    xm_emit_unary(func, a, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(a, c);
    xm_block_add_pred(c, a, func->arena);

    xm_emit_unary(func, b, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_jmp(b, c);
    xm_block_add_pred(c, b, func->arena);

    XmRef c42 = xm_const_i64(func, 42);
    XmRef vc = xm_emit_unary(func, c, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(c, vc);

    assert(func->nblk == 3);
    assert(c->npred == 2);

    xm_pass_merge_blocks(func);

    /* c has 2 predecessors, so no merge should happen */
    assert(func->nblk == 3);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Combined Pipeline Test ========== */

/*
 * Test: const_prop + branch_simp + remove_unreachable + copy_prop + DCE
 * work together to eliminate dead code.
 *
 *   entry:
 *     v0 = CONST_I64(1)
 *     BR v0, then, else
 *   then:
 *     v1 = CONST_I64(42)
 *     RET v1
 *   else:
 *     v2 = CONST_I64(99)
 *     RET v2
 *
 * After pipeline: entry â†?JMP then, else block removed.
 */
static void test_combined_branch_elim(void) {
    fprintf(stderr, "  test_combined_branch_elim...");

    XmFunc *func = xm_func_new("combined");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);

    XmRef c42 = xm_const_i64(func, 42);
    XmRef c99 = xm_const_i64(func, 99);
    XmRef vt = xm_emit_unary(func, then_blk, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_ret(then_blk, vt);
    XmRef ve = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c99);
    xm_block_set_ret(else_blk, ve);

    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    assert(func->nblk == 3);

    /* Run the combined passes */
    xm_pass_sccp(func);
    xm_pass_copy_prop(func);
    xm_pass_dce(func);

    /* else block should be removed */
    assert(func->nblk == 2);
    /* entry should JMP to then */
    assert(func->blocks[0]->jmp.type == XM_JMP_JMP);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== GVN Normalization Tests ========== */

/*
 * Test: commutative normalization makes ADD(v1,v0) match ADD(v0,v1).
 *
 *   Block 0 (entry â†?JMP b1):
 *     v0 = CONST_I64(10)
 *     v1 = CONST_I64(20)
 *     v2 = ADD v0, v1      // canonical order
 *
 *   Block 1:
 *     v3 = ADD v1, v0      // reversed order â€?should be CSE'd by GVN
 *     v4 = ADD v2, v3
 *     RET v4
 *
 * After GVN: v3 should become MOV v2 (CSE hit via normalization).
 */
static void test_gvn_normalize_commutative(void) {
    fprintf(stderr, "  test_gvn_normalize_commutative...");

    XmFunc *func = xm_func_new("gvn_norm");
    XmBlock *b0 = xm_func_add_block(func, "entry");
    XmBlock *b1 = xm_func_add_block(func, "body");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v0 = xm_emit_unary(func, b0, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, b0, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, b0, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_jmp(b0, b1);
    xm_block_add_pred(b1, b0, func->arena);

    /* Reversed arg order â€?should normalize to same hash */
    XmRef v3 = xm_emit(func, b1, XM_ADD, XR_REP_I64, v1, v0);
    XmRef v4 = xm_emit(func, b1, XM_ADD, XR_REP_I64, v2, v3);
    xm_block_set_ret(b1, v4);

    xm_pass_gvn(func);

    /* v3's instruction should be MOV v2 (CSE'd) */
    XmIns *v3_ins = NULL;
    for (uint32_t i = 0; i < b1->nins; i++) {
        if (b1->ins[i].dst == v3) {
            v3_ins = &b1->ins[i];
            break;
        }
    }
    assert(v3_ins != NULL);
    assert(v3_ins->op == XM_MOV);
    assert(v3_ins->args[0] == v2);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: non-commutative SUB is NOT normalized (SUB(a,b) != SUB(b,a)).
 *
 *   Block 0 â†?Block 1:
 *     v0 = CONST_I64(10)
 *     v1 = CONST_I64(20)
 *     v2 = SUB v0, v1      // 10 - 20
 *
 *   Block 1:
 *     v3 = SUB v1, v0      // 20 - 10 (different!)
 *     v4 = ADD v2, v3
 *     RET v4
 *
 * After GVN: v3 should NOT be CSE'd (SUB not commutative).
 */
static void test_gvn_no_normalize_sub(void) {
    fprintf(stderr, "  test_gvn_no_normalize_sub...");

    XmFunc *func = xm_func_new("gvn_nosub");
    XmBlock *b0 = xm_func_add_block(func, "entry");
    XmBlock *b1 = xm_func_add_block(func, "body");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v0 = xm_emit_unary(func, b0, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, b0, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, b0, XM_SUB, XR_REP_I64, v0, v1);
    xm_block_set_jmp(b0, b1);
    xm_block_add_pred(b1, b0, func->arena);

    XmRef v3 = xm_emit(func, b1, XM_SUB, XR_REP_I64, v1, v0);
    XmRef v4 = xm_emit(func, b1, XM_ADD, XR_REP_I64, v2, v3);
    xm_block_set_ret(b1, v4);

    xm_pass_gvn(func);

    /* v3 should still be SUB (not CSE'd) */
    XmIns *v3_ins = NULL;
    for (uint32_t i = 0; i < b1->nins; i++) {
        if (b1->ins[i].dst == v3) {
            v3_ins = &b1->ins[i];
            break;
        }
    }
    assert(v3_ins != NULL);
    assert(v3_ins->op == XM_SUB);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Store-to-Load Forwarding Tests ========== */

/*
 * Test: STORE_FIELD then LOAD_FIELD to same (obj, offset) is forwarded.
 *
 *   v0 = CONST_PTR(fake_obj)
 *   v1 = CONST_I64(42)
 *   STORE_FIELD offset=16, obj=v0, val=v1
 *   v2 = LOAD_FIELD obj=v0, offset=16     // should become MOV v1
 *   RET v2
 */
static void test_s2l_basic_forward(void) {
    fprintf(stderr, "  test_s2l_basic_forward...");

    XmFunc *func = xm_func_new("s2l_basic");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef cptr = xm_const_ptr(func, (void *) 0xDEAD);
    XmRef c42 = xm_const_i64(func, 42);
    XmRef c16 = xm_const_i64(func, 16);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_PTR, XR_REP_PTR, cptr);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);

    /* STORE_FIELD: dst=const(offset), args[0]=obj, args[1]=value */
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v1);

    /* LOAD_FIELD: dst=vreg, args[0]=obj, args[1]=const(offset) */
    XmRef v2 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, v0, c16);
    xm_block_set_ret(entry, v2);

    xm_pass_store_to_load(func);

    /* v2 should now be MOV v1 (forwarded from store) */
    XmIns *v2_ins = NULL;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].dst == v2) {
            v2_ins = &entry->ins[i];
            break;
        }
    }
    assert(v2_ins != NULL);
    assert(v2_ins->op == XM_MOV);
    assert(v2_ins->args[0] == v1);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: STORE_FIELD to different offset does NOT forward to LOAD_FIELD.
 *
 *   STORE_FIELD offset=16, obj=v0, val=v1
 *   v2 = LOAD_FIELD obj=v0, offset=24     // different offset, no forward
 */
static void test_s2l_different_offset(void) {
    fprintf(stderr, "  test_s2l_different_offset...");

    XmFunc *func = xm_func_new("s2l_diff");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef cptr = xm_const_ptr(func, (void *) 0xDEAD);
    XmRef c42 = xm_const_i64(func, 42);
    XmRef c16 = xm_const_i64(func, 16);
    XmRef c24 = xm_const_i64(func, 24);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_PTR, XR_REP_PTR, cptr);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);

    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v1);
    XmRef v2 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, v0, c24);
    xm_block_set_ret(entry, v2);

    xm_pass_store_to_load(func);

    /* v2 should still be LOAD_FIELD (no forwarding) */
    XmIns *v2_ins = NULL;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].dst == v2) {
            v2_ins = &entry->ins[i];
            break;
        }
    }
    assert(v2_ins != NULL);
    assert(v2_ins->op == XM_LOAD_FIELD);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: CALL between STORE and LOAD kills forwarding.
 *
 *   STORE_FIELD offset=16, obj=v0, val=v1
 *   CALL ...
 *   v2 = LOAD_FIELD obj=v0, offset=16     // killed by CALL
 */
static void test_s2l_kill_by_call(void) {
    fprintf(stderr, "  test_s2l_kill_by_call...");

    XmFunc *func = xm_func_new("s2l_call");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef cptr = xm_const_ptr(func, (void *) 0xDEAD);
    XmRef c42 = xm_const_i64(func, 42);
    XmRef c16 = xm_const_i64(func, 16);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_PTR, XR_REP_PTR, cptr);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);

    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v1);

    /* Insert a CALL (side effect kills all tracked state) */
    xm_emit_raw(func, entry, XM_CALL, XR_REP_I64, XM_NONE, v0, XM_NONE);

    XmRef v2 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, v0, c16);
    xm_block_set_ret(entry, v2);

    xm_pass_store_to_load(func);

    /* v2 should still be LOAD_FIELD (call killed the tracked store) */
    XmIns *v2_ins = NULL;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].dst == v2) {
            v2_ins = &entry->ins[i];
            break;
        }
    }
    assert(v2_ins != NULL);
    assert(v2_ins->op == XM_LOAD_FIELD);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Dead Store Elimination Tests ========== */

/*
 * Test: Two consecutive STORE_FIELD to same (obj, offset) â€?first is dead.
 *
 *   STORE_FIELD offset=16, obj=v0, val=v1   // dead (overwritten by next)
 *   STORE_FIELD offset=16, obj=v0, val=v2   // live
 *   RET v2
 */
static void test_dse_consecutive_stores(void) {
    fprintf(stderr, "  test_dse_consecutive_stores...");

    XmFunc *func = xm_func_new("dse_basic");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef cptr = xm_const_ptr(func, (void *) 0xDEAD);
    XmRef c42 = xm_const_i64(func, 42);
    XmRef c99 = xm_const_i64(func, 99);
    XmRef c16 = xm_const_i64(func, 16);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_PTR, XR_REP_PTR, cptr);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c99);

    /* First store (will be killed by DSE) */
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v1);
    uint32_t first_store_idx = entry->nins - 1;

    /* Second store to same (obj, offset) */
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v2);
    xm_block_set_ret(entry, v2);

    xm_pass_store_to_load(func);

    /* First store should be converted to NOP (MOV NONE, NONE) */
    XmIns *dead = &entry->ins[first_store_idx];
    assert(dead->op == XM_MOV);
    assert(xm_ref_is_none(dead->dst));
    assert(xm_ref_is_none(dead->args[0]));

    /* Second store should still be STORE_FIELD */
    bool found_live_store = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XM_STORE_FIELD) {
            found_live_store = true;
            break;
        }
    }
    assert(found_live_store);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: STORE, LOAD, STORE â€?first store is NOT dead (it was read).
 *
 *   STORE_FIELD offset=16, obj=v0, val=v1   // live (read by LOAD)
 *   v3 = LOAD_FIELD obj=v0, offset=16       // reads the store
 *   STORE_FIELD offset=16, obj=v0, val=v2   // overwrites
 */
static void test_dse_store_load_store(void) {
    fprintf(stderr, "  test_dse_store_load_store...");

    XmFunc *func = xm_func_new("dse_sls");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef cptr = xm_const_ptr(func, (void *) 0xDEAD);
    XmRef c42 = xm_const_i64(func, 42);
    XmRef c99 = xm_const_i64(func, 99);
    XmRef c16 = xm_const_i64(func, 16);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_PTR, XR_REP_PTR, cptr);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    XmRef v2 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c99);

    /* First store */
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v1);
    uint32_t first_store_idx = entry->nins - 1;

    /* Load from same field (reads the first store) */
    XmRef v3 = xm_emit(func, entry, XM_LOAD_FIELD, XR_REP_I64, v0, c16);

    /* Second store to same (obj, offset) */
    xm_emit_raw(func, entry, XM_STORE_FIELD, XR_REP_I64, c16, v0, v2);
    xm_block_set_ret(entry, v3);

    xm_pass_store_to_load(func);

    /* First store should still be STORE_FIELD (not dead â€?was read by LOAD) */
    XmIns *first = &entry->ins[first_store_idx];
    assert(first->op == XM_STORE_FIELD);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== LICM Tests ========== */

/*
 * Test: Pure instruction with loop-invariant operands is hoisted.
 *
 * CFG: entry(0) â†?header(1) â†?body(2) â†?header(1)  [back-edge]
 *                  header(1) â†?exit(3)
 *
 *   entry:  v0 = CONST_I64(10)
 *           v1 = CONST_I64(20)
 *           JMP header
 *   header: BR(cond) body, exit
 *   body:   v2 = ADD v0, v1      // loop-invariant, should be hoisted
 *           JMP header
 *   exit:   RET v0
 */
static void test_licm_basic_hoist(void) {
    fprintf(stderr, "  test_licm_basic_hoist...");

    XmFunc *func = xm_func_new("licm_basic");
    XmBlock *entry = xm_func_add_block(func, "entry");    // bi=0
    XmBlock *header = xm_func_add_block(func, "header");  // bi=1
    XmBlock *body = xm_func_add_block(func, "body");      // bi=2
    XmBlock *exit_b = xm_func_add_block(func, "exit");    // bi=3

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    xm_block_set_jmp(entry, header);
    xm_block_add_pred(header, entry, func->arena);

    /* header: BR(cond) â†?body or exit */
    XmRef cond = xm_emit_unary(func, header, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_br(header, cond, body, exit_b);
    xm_block_add_pred(body, header, func->arena);
    xm_block_add_pred(exit_b, header, func->arena);

    /* body: v2 = ADD v0, v1 (invariant); JMP header (back-edge) */
    XmRef v2 = xm_emit(func, body, XM_ADD, XR_REP_I64, v0, v1);
    (void) v2;
    xm_block_set_jmp(body, header);
    xm_block_add_pred(header, body, func->arena);  // back-edge pred

    /* exit: RET v0 */
    xm_block_set_ret(exit_b, v0);

    /* Record original body instruction count */
    uint32_t body_nins_before = body->nins;
    uint32_t entry_nins_before = entry->nins;

    xm_pass_licm(func);

    /* v2 = ADD should have been hoisted from body to entry (preheader) */
    assert(body->nins < body_nins_before);
    assert(entry->nins > entry_nins_before);

    /* Verify the hoisted instruction is ADD with correct operands */
    bool found_add = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XM_ADD && entry->ins[i].args[0] == v0 &&
            entry->ins[i].args[1] == v1) {
            found_add = true;
            break;
        }
    }
    assert(found_add);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test: loop_depth is computed correctly for nested loops.
 *
 * CFG: entry(0) â†?outer_hdr(1) â†?inner_hdr(2) â†?inner_body(3) â†?inner_hdr(2)
 *                                  inner_hdr(2) â†?outer_body(4) â†?outer_hdr(1)
 *                  outer_hdr(1) â†?exit(5)
 *
 * Expected loop_depth:
 *   entry=0, outer_hdr=1, inner_hdr=2, inner_body=2, outer_body=1, exit=0
 */
static void test_licm_loop_depth(void) {
    fprintf(stderr, "  test_licm_loop_depth...");

    XmFunc *func = xm_func_new("licm_depth");
    XmBlock *entry = xm_func_add_block(func, "entry");            // 0
    XmBlock *outer_hdr = xm_func_add_block(func, "outer_hdr");    // 1
    XmBlock *inner_hdr = xm_func_add_block(func, "inner_hdr");    // 2
    XmBlock *inner_body = xm_func_add_block(func, "inner_body");  // 3
    XmBlock *outer_body = xm_func_add_block(func, "outer_body");  // 4
    XmBlock *exit_b = xm_func_add_block(func, "exit");            // 5

    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);

    /* entry â†?outer_hdr */
    xm_block_set_jmp(entry, outer_hdr);
    xm_block_add_pred(outer_hdr, entry, func->arena);

    /* outer_hdr: BR â†?inner_hdr or exit */
    XmRef vc1 = xm_emit_unary(func, outer_hdr, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_br(outer_hdr, vc1, inner_hdr, exit_b);
    xm_block_add_pred(inner_hdr, outer_hdr, func->arena);
    xm_block_add_pred(exit_b, outer_hdr, func->arena);

    /* inner_hdr: BR â†?inner_body or outer_body */
    XmRef vc2 = xm_emit_unary(func, inner_hdr, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_br(inner_hdr, vc2, inner_body, outer_body);
    xm_block_add_pred(inner_body, inner_hdr, func->arena);
    xm_block_add_pred(outer_body, inner_hdr, func->arena);

    /* inner_body â†?inner_hdr (inner back-edge) */
    XmRef vc3 = xm_emit_unary(func, inner_body, XM_CONST_I64, XR_REP_I64, c1);
    (void) vc3;
    xm_block_set_jmp(inner_body, inner_hdr);
    xm_block_add_pred(inner_hdr, inner_body, func->arena);

    /* outer_body â†?outer_hdr (outer back-edge) */
    XmRef vc4 = xm_emit_unary(func, outer_body, XM_CONST_I64, XR_REP_I64, c1);
    (void) vc4;
    xm_block_set_jmp(outer_body, outer_hdr);
    xm_block_add_pred(outer_hdr, outer_body, func->arena);

    /* exit: RET */
    xm_block_set_ret(exit_b, v0);

    xm_pass_licm(func);

    /* Check loop_depth via XmLoopInfo (loop_depth moved off XmBlock) */
    assert(xm_block_loop_depth(func, entry->id) == 0);
    assert(xm_block_loop_depth(func, outer_hdr->id) == 1);
    assert(xm_block_loop_depth(func, inner_hdr->id) == 2);
    assert(xm_block_loop_depth(func, inner_body->id) == 2);
    assert(xm_block_loop_depth(func, outer_body->id) == 1);
    assert(xm_block_loop_depth(func, exit_b->id) == 0);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== If-Conversion Tests ========== */

/*
 * Test: Diamond pattern is converted to SELECT.
 *
 * CFG: entry(0) -> BR(cond) â†?then(1), else(2)
 *       then(1) -> v_then = CONST_I64(10); JMP merge(3)
 *       else(2) -> v_else = CONST_I64(20); JMP merge(3)
 *       merge(3) -> v_phi = PHI(v_then, v_else); RET v_phi
 *
 * After if-conversion:
 *   entry: [original] + [then ins] + [else ins]
 *          + SELECT_COND(cond) + SELECT(v_then, v_else)
 *          JMP merge
 *   merge: no PHI, RET v_phi
 */
static void test_ifconv_diamond(void) {
    fprintf(stderr, "  test_ifconv_diamond...");

    XmFunc *func = xm_func_new("ifconv");
    XmBlock *entry = xm_func_add_block(func, "entry");  // 0
    XmBlock *thenb = xm_func_add_block(func, "then");   // 1
    XmBlock *elseb = xm_func_add_block(func, "else");   // 2
    XmBlock *merge = xm_func_add_block(func, "merge");  // 3

    /* entry: BR(cond) â†?then, else */
    XmRef c1 = xm_const_i64(func, 1);
    XmRef cond = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    xm_block_set_br(entry, cond, thenb, elseb);
    xm_block_add_pred(thenb, entry, func->arena);
    xm_block_add_pred(elseb, entry, func->arena);

    /* then: v_then = CONST_I64(10); JMP merge */
    XmRef c10 = xm_const_i64(func, 10);
    XmRef v_then = xm_emit_unary(func, thenb, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_jmp(thenb, merge);
    xm_block_add_pred(merge, thenb, func->arena);

    /* else: v_else = CONST_I64(20); JMP merge */
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v_else = xm_emit_unary(func, elseb, XM_CONST_I64, XR_REP_I64, c20);
    xm_block_set_jmp(elseb, merge);
    xm_block_add_pred(merge, elseb, func->arena);

    /* merge: PHI(v_then from then, v_else from else); RET */
    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    xm_phi_set_arg(phi, 0, v_then);  // pred[0] = then
    xm_phi_set_arg(phi, 1, v_else);  // pred[1] = else
    xm_block_set_ret(merge, phi->dst);

    assert(merge->phis != NULL);
    assert(entry->jmp.type == XM_JMP_BR);

    xm_pass_ifconvert(func);

    /* entry should now be JMP (not BR) */
    assert(entry->jmp.type == XM_JMP_JMP);
    assert(entry->s1 == merge);
    assert(entry->s2 == NULL);

    /* merge should have no PHIs */
    assert(merge->phis == NULL);

    /* entry should contain SELECT_COND and SELECT instructions */
    bool found_sel_cond = false;
    bool found_sel = false;
    for (uint32_t i = 0; i < entry->nins; i++) {
        if (entry->ins[i].op == XM_SELECT_COND)
            found_sel_cond = true;
        if (entry->ins[i].op == XM_SELECT) {
            found_sel = true;
            assert(entry->ins[i].args[0] == v_then);
            assert(entry->ins[i].args[1] == v_else);
        }
    }
    assert(found_sel_cond);
    assert(found_sel);

    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm_pass ===\n");

    /* Copy Propagation */
    test_copy_prop_chain();
    test_copy_prop_no_mov();

    /* Branch Simplification */
    test_branch_simp_const_true();
    test_branch_simp_const_false();
    test_branch_simp_same_target();

    /* Unreachable Block Elimination */
    test_remove_unreachable_basic();

    /* Phi Simplification */
    test_phi_simp_trivial();
    test_phi_simp_nontrivial();

    /* Block Merging */
    test_merge_blocks_basic();
    test_merge_blocks_multi_pred();

    /* Combined */
    test_combined_branch_elim();

    /* GVN Normalization */
    test_gvn_normalize_commutative();
    test_gvn_no_normalize_sub();

    /* Store-to-Load Forwarding */
    test_s2l_basic_forward();
    test_s2l_different_offset();
    test_s2l_kill_by_call();

    /* Dead Store Elimination */
    test_dse_consecutive_stores();
    test_dse_store_load_store();

    /* LICM */
    test_licm_basic_hoist();
    test_licm_loop_depth();

    /* If-Conversion */
    test_ifconv_diamond();

    fprintf(stderr, "All 21 tests passed!\n");
    return 0;
}
