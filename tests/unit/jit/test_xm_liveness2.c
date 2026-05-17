/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xm_liveness2.c - Unit tests for dataflow-based liveness analysis
 */

#include <stdio.h>
#include <assert.h>
#include "../../../src/jit/xm.h"
#include "../../../src/jit/xm_liveness2.h"
#include "../test_win_compat.h"

/*
 * Test 1: Single block, no liveness across boundaries.
 *
 *   entry:
 *     v0 = CONST_I64(10)
 *     v1 = CONST_I64(20)
 *     v2 = ADD v0, v1
 *     RET v2
 *
 * Expected: live_in[entry] = {} (all defined locally)
 *           live_out[entry] = {} (no successors)
 */
static void test_single_block(void) {
    fprintf(stderr, "  test_single_block...");

    XmFunc *func = xm_func_new("single");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c10 = xm_const_i64(func, 10);
    XmRef c20 = xm_const_i64(func, 20);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c20);
    XmRef v2 = xm_emit(func, entry, XM_ADD, XR_REP_I64, v0, v1);
    xm_block_set_ret(entry, v2);

    XmLive live;
    xm_live_compute(&live, func);

    assert(live.nblk == 1);
    assert(xm_bset_empty(&live.blocks[0].live_in));
    assert(xm_bset_empty(&live.blocks[0].live_out));

    xm_live_free(&live);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 2: Diamond CFG (if-then-else).
 *
 *   entry:
 *     v0 = CONST_I64(1)     ; condition
 *     v1 = CONST_I64(10)    ; used in then
 *     BR v0, then, else
 *
 *   then:
 *     v2 = ADD v1, v1       ; uses v1
 *     JMP merge
 *
 *   else:
 *     v3 = CONST_I64(99)
 *     JMP merge
 *
 *   merge:
 *     phi(v2, v3)
 *     RET phi
 *
 * Expected: v1 is live-out of entry (used in then block)
 *           v1 is live-in of then block
 */
static void test_diamond(void) {
    fprintf(stderr, "  test_diamond...");

    XmFunc *func = xm_func_new("diamond");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *then_blk = xm_func_add_block(func, "then");
    XmBlock *else_blk = xm_func_add_block(func, "else");
    XmBlock *merge = xm_func_add_block(func, "merge");

    /* entry */
    XmRef c1 = xm_const_i64(func, 1);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c1);
    XmRef c10 = xm_const_i64(func, 10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_br(entry, v0, then_blk, else_blk);
    xm_block_add_pred(then_blk, entry, func->arena);
    xm_block_add_pred(else_blk, entry, func->arena);

    /* then: v2 = v1 + v1 */
    XmRef v2 = xm_emit(func, then_blk, XM_ADD, XR_REP_I64, v1, v1);
    xm_block_set_jmp(then_blk, merge);
    xm_block_add_pred(merge, then_blk, func->arena);

    /* else: v3 = 99 */
    XmRef c99 = xm_const_i64(func, 99);
    XmRef v3 = xm_emit_unary(func, else_blk, XM_CONST_I64, XR_REP_I64, c99);
    xm_block_set_jmp(else_blk, merge);
    xm_block_add_pred(merge, else_blk, func->arena);

    /* merge: phi(v2, v3), ret */
    XmPhi *phi = xm_add_phi(func, merge, XR_REP_I64);
    xm_phi_set_arg(phi, 0, v2); /* from then */
    xm_phi_set_arg(phi, 1, v3); /* from else */
    xm_block_set_ret(merge, phi->dst);

    XmLive live;
    xm_live_compute(&live, func);

    assert(live.nblk == 4);

    /* v1 should be live-out of entry (used in then) */
    uint32_t v1_idx = XM_REF_INDEX(v1);
    assert(xm_live_at_exit(&live, 0, v1_idx));

    /* v1 should be live-in of then block */
    assert(xm_live_at_entry(&live, 1, v1_idx));

    /* v1 should NOT be live-in of else block (not used there) */
    assert(!xm_live_at_entry(&live, 2, v1_idx));

    /* v2 should be live-out of then (phi arg in merge) */
    uint32_t v2_idx = XM_REF_INDEX(v2);
    assert(xm_live_at_exit(&live, 1, v2_idx));

    /* v3 should be live-out of else (phi arg in merge) */
    uint32_t v3_idx = XM_REF_INDEX(v3);
    assert(xm_live_at_exit(&live, 2, v3_idx));

    /* merge live_in should have phi->dst (from phi def) but
     * NOT v2 or v3 (those come via phi, not direct use) */
    assert(!xm_live_at_entry(&live, 3, v2_idx));
    assert(!xm_live_at_entry(&live, 3, v3_idx));

    xm_live_free(&live);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 3: Simple loop.
 *
 *   entry:
 *     v0 = CONST_I64(0)    ; i = 0
 *     v1 = CONST_I64(10)   ; limit
 *     JMP header
 *
 *   header:
 *     phi_i(v0, v3)        ; i from entry or back-edge
 *     v2 = LT phi_i, v1    ; i < 10
 *     BR v2, body, exit
 *
 *   body:
 *     c1 = CONST_I64(1)
 *     v3 = ADD phi_i, c1   ; i + 1
 *     JMP header
 *
 *   exit:
 *     RET phi_i
 *
 * Expected: v1 is live across the loop (live-in of header)
 *           phi_i is live-out of header (used in body and exit)
 */
static void test_loop(void) {
    fprintf(stderr, "  test_loop...");

    XmFunc *func = xm_func_new("loop");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *header = xm_func_add_block(func, "header");
    XmBlock *body = xm_func_add_block(func, "body");
    XmBlock *exit_blk = xm_func_add_block(func, "exit");

    /* Build CFG edges first so phi gets correct narg */
    xm_block_add_pred(header, entry, func->arena); /* pred 0 */
    xm_block_add_pred(header, body, func->arena);  /* pred 1 (back-edge) */
    xm_block_add_pred(body, header, func->arena);
    xm_block_add_pred(exit_blk, header, func->arena);

    /* entry */
    XmRef c0 = xm_const_i64(func, 0);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c0);
    XmRef c10 = xm_const_i64(func, 10);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c10);
    xm_block_set_jmp(entry, header);

    /* header: phi(entry:v0, body:v3), compare, branch */
    XmPhi *phi_i = xm_add_phi(func, header, XR_REP_I64);
    xm_phi_set_arg(phi_i, 0, v0);

    XmRef v2 = xm_emit(func, header, XM_LT, XR_REP_I64, phi_i->dst, v1);
    xm_block_set_br(header, v2, body, exit_blk);

    /* body: i + 1, jump back */
    XmRef c1 = xm_const_i64(func, 1);
    XmRef vc1 = xm_emit_unary(func, body, XM_CONST_I64, XR_REP_I64, c1);
    XmRef v3 = xm_emit(func, body, XM_ADD, XR_REP_I64, phi_i->dst, vc1);
    xm_block_set_jmp(body, header);
    xm_phi_set_arg(phi_i, 1, v3); /* back-edge */

    /* exit: return phi_i */
    xm_block_set_ret(exit_blk, phi_i->dst);

    XmLive live;
    xm_live_compute(&live, func);

    uint32_t v1_idx = XM_REF_INDEX(v1);
    uint32_t phi_idx = XM_REF_INDEX(phi_i->dst);

    /* v1 (limit) should be live-out of entry (used in header) */
    assert(xm_live_at_exit(&live, 0, v1_idx));

    /* v1 should be live-in of header */
    assert(xm_live_at_entry(&live, 1, v1_idx));

    /* v1 should be live across the loop: live-out of body
     * (because header uses it, and body jumps back to header) */
    assert(xm_live_at_exit(&live, 2, v1_idx));

    /* phi_i->dst should be live-out of header (used in body and exit) */
    assert(xm_live_at_exit(&live, 1, phi_idx));

    /* phi_i->dst should be live-in of body (used in ADD) */
    assert(xm_live_at_entry(&live, 2, phi_idx));

    /* phi_i->dst should be live-in of exit (used in RET) */
    assert(xm_live_at_entry(&live, 3, phi_idx));

    xm_live_free(&live);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 4: Dead variable (defined but never used).
 *
 *   entry:
 *     v0 = CONST_I64(42)    ; dead
 *     v1 = CONST_I64(99)
 *     RET v1
 *
 * Expected: v0 not live anywhere
 */
static void test_dead_var(void) {
    fprintf(stderr, "  test_dead_var...");

    XmFunc *func = xm_func_new("dead");
    XmBlock *entry = xm_func_add_block(func, "entry");

    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    XmRef c99 = xm_const_i64(func, 99);
    XmRef v1 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c99);
    (void) v0;
    xm_block_set_ret(entry, v1);

    XmLive live;
    xm_live_compute(&live, func);

    uint32_t v0_idx = XM_REF_INDEX(v0);
    assert(!xm_live_at_entry(&live, 0, v0_idx));
    assert(!xm_live_at_exit(&live, 0, v0_idx));

    xm_live_free(&live);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

/*
 * Test 5: Chain of blocks â€?value passes through middle block.
 *
 *   entry:
 *     v0 = CONST_I64(42)
 *     JMP mid
 *
 *   mid:
 *     v1 = CONST_I64(0)     ; local, not relevant
 *     JMP tail
 *
 *   tail:
 *     RET v0                ; uses v0 from entry
 *
 * Expected: v0 is live-through mid (live-in AND live-out of mid)
 */
static void test_passthrough(void) {
    fprintf(stderr, "  test_passthrough...");

    XmFunc *func = xm_func_new("passthru");
    XmBlock *entry = xm_func_add_block(func, "entry");
    XmBlock *mid = xm_func_add_block(func, "mid");
    XmBlock *tail = xm_func_add_block(func, "tail");

    XmRef c42 = xm_const_i64(func, 42);
    XmRef v0 = xm_emit_unary(func, entry, XM_CONST_I64, XR_REP_I64, c42);
    xm_block_set_jmp(entry, mid);
    xm_block_add_pred(mid, entry, func->arena);

    XmRef c0 = xm_const_i64(func, 0);
    XmRef v1 = xm_emit_unary(func, mid, XM_CONST_I64, XR_REP_I64, c0);
    (void) v1;
    xm_block_set_jmp(mid, tail);
    xm_block_add_pred(tail, mid, func->arena);

    xm_block_set_ret(tail, v0);

    XmLive live;
    xm_live_compute(&live, func);

    uint32_t v0_idx = XM_REF_INDEX(v0);

    /* v0 should be live-out of entry */
    assert(xm_live_at_exit(&live, 0, v0_idx));

    /* v0 should pass through mid */
    assert(xm_live_at_entry(&live, 1, v0_idx));
    assert(xm_live_at_exit(&live, 1, v0_idx));

    /* v0 should be live-in of tail (used in RET) */
    assert(xm_live_at_entry(&live, 2, v0_idx));

    xm_live_free(&live);
    xm_func_destroy(func);
    fprintf(stderr, " PASS\n");
}

int main(void) {
    xr_test_suppress_dialogs();
    fprintf(stderr, "=== test_xm_liveness2 ===\n");

    test_single_block();
    test_diamond();
    test_loop();
    test_dead_var();
    test_passthrough();

    fprintf(stderr, "All 5 tests passed!\n");
    return 0;
}
