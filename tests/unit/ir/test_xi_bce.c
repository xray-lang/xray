/*
 * Unit tests for Xi bounds check elimination (xi_opt_bce).
 * Covers range-based proof, dominator dedup, and non-elimination cases.
 */

#include "../../../src/ir/xi_opt_bce.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_range.h"
#include "../../../src/ir/xi_op_name.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <limits.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_bce", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* Helper: create a BOUNDS_CHECK value. */
static XiValue *make_bounds_check(XiFunc *f, XiBlock *blk, XiValue *idx, XiValue *len) {
    XiValue *bc = xi_value_new(f, blk, XI_BOUNDS_CHECK, &stub_int, 2);
    bc->args[0] = idx;
    bc->args[1] = len;
    bc->flags |= XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW;
    return bc;
}

/* ========== Range Proof Tests ========== */

TEST(eliminate_const_idx_const_len) {
    /* idx=3, len=10 -> range(3) = [3,3], range(10).lo = 10 -> 3 < 10 -> eliminate */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 3, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    ASSERT(bc->nargs == 1);
    ASSERT(bc->args[0] == idx);

    xi_func_free(f);
}

TEST(eliminate_zero_idx) {
    /* idx=0, len=1 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *len = xi_const_int(f, blk, 1, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

TEST(keep_negative_idx) {
    /* idx=-1, len=10 -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, -1, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(keep_idx_equals_len) {
    /* idx=5, len=5 -> idx >= len -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 5, &stub_int);
    XiValue *len = xi_const_int(f, blk, 5, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(keep_idx_exceeds_len) {
    /* idx=10, len=5 -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 10, &stub_int);
    XiValue *len = xi_const_int(f, blk, 5, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(eliminate_boundary_idx) {
    /* idx=9, len=10 -> range(9)=[9,9], 9 < 10 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 9, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

/* ========== Dominator Dedup Tests ========== */

TEST(dedup_same_block) {
    /* Two identical checks in same block -> second eliminated. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 100, &stub_int);
    XiValue *len = xi_const_int(f, blk, 50, &stub_int);
    /* idx=100, len=50 -> can NOT range-prove (100 >= 50).
     * But second check with same args should be deduped. */
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    /* First stays, second deduped. */
    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);

    xi_func_free(f);
}

TEST(dedup_dominating_block) {
    /* Check in entry dominates check in successor. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    entry->succs[0] = succ;
    xi_block_add_pred(succ, entry);
    succ->idom = entry;

    /* Use large idx so range proof fails. */
    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);

    /* Same check in successor block. */
    XiValue *bc2 = make_bounds_check(f, succ, idx, len);

    succ->kind = XI_BLOCK_RETURN;
    succ->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);

    xi_func_free(f);
}

/* ========== No Range Info Tests ========== */

TEST(no_elimination_without_range) {
    /* If range analysis hasn't run, BCE should be a no-op. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    /* Do NOT run xi_range_analyze. */
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(empty_func_no_crash) {
    XiFunc *f = make_func();
    f->invariant_mask |= XI_INV_RANGE_ANNOTATED;
    xi_opt_bce(f);
    xi_func_free(f);
}

/* ========== Multiple Eliminations ========== */

TEST(multiple_safe_checks) {
    /* Several safe checks in one block -> all eliminated. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc[5];
    for (int i = 0; i < 5; i++) {
        XiValue *idx = xi_const_int(f, blk, i * 10, &stub_int);
        bc[i] = make_bounds_check(f, blk, idx, len);
    }

    xi_range_analyze(f);
    xi_opt_bce(f);

    for (int i = 0; i < 5; i++) {
        ASSERT(bc[i]->op == XI_COPY);
    }

    xi_func_free(f);
}

TEST(mixed_safe_unsafe) {
    /* Some safe, some unsafe checks. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *idx_safe = xi_const_int(f, blk, 5, &stub_int);
    XiValue *idx_unsafe = xi_const_int(f, blk, 15, &stub_int);

    XiValue *bc_safe = make_bounds_check(f, blk, idx_safe, len);
    XiValue *bc_unsafe = make_bounds_check(f, blk, idx_unsafe, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_safe->op == XI_COPY);
    ASSERT(bc_unsafe->op == XI_BOUNDS_CHECK);

    xi_func_free(f);
}

TEST(side_effect_flag_cleared) {
    /* Eliminated check should have side-effect flag cleared. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 2, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    ASSERT(!(bc->flags & XI_FLAG_SIDE_EFFECT));
    ASSERT(!(bc->flags & XI_FLAG_MAY_THROW));

    xi_func_free(f);
}

/* Helper: wire CFG edge from->succs[slot] to 'to', adding pred. */
static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

/* ========== Range Proof — Boundary Constants ========== */

TEST(keep_zero_idx_zero_len) {
    /* idx=0, len=0 -> 0 < 0 is false -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *len = xi_const_int(f, blk, 0, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(keep_idx_one_len_one) {
    /* idx=1, len=1 -> 1 < 1 false -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 1, &stub_int);
    XiValue *len = xi_const_int(f, blk, 1, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(eliminate_idx_one_len_two) {
    /* idx=1, len=2 -> 1 < 2 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 1, &stub_int);
    XiValue *len = xi_const_int(f, blk, 2, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

TEST(keep_very_large_negative_idx) {
    /* idx=-1000000, len=10 -> negative -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, -1000000, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(eliminate_large_constants) {
    /* idx=99999, len=100000 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 99999, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100000, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

TEST(keep_max_int_idx) {
    /* idx=INT64_MAX, len=INT64_MAX -> equal -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, INT64_MAX, &stub_int);
    XiValue *len = xi_const_int(f, blk, INT64_MAX, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(keep_huge_idx_small_len) {
    /* idx=INT64_MAX, len=1 -> vastly out of range */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, INT64_MAX, &stub_int);
    XiValue *len = xi_const_int(f, blk, 1, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

/* ========== Range Proof — Derived Ranges (ADD / SUB) ========== */

TEST(eliminate_add_derived_range) {
    /* c = 3 + 4 = 7, len=10 -> range(c) = [7,7], 7 < 10 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *a = xi_const_int(f, blk, 3, &stub_int);
    XiValue *b = xi_const_int(f, blk, 4, &stub_int);
    XiValue *c = xi_binary(f, blk, XI_ADD, &stub_int, a, b);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, c, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    ASSERT(bc->args[0] == c);
    xi_func_free(f);
}

TEST(keep_add_derived_oob) {
    /* c = 5 + 5 = 10, len=10 -> 10 < 10 false -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *a = xi_const_int(f, blk, 5, &stub_int);
    XiValue *b = xi_const_int(f, blk, 5, &stub_int);
    XiValue *c = xi_binary(f, blk, XI_ADD, &stub_int, a, b);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, c, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(eliminate_sub_derived_range) {
    /* c = 10 - 3 = 7, len=10 -> 7 < 10 -> safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *a = xi_const_int(f, blk, 10, &stub_int);
    XiValue *b = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c = xi_binary(f, blk, XI_SUB, &stub_int, a, b);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, c, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

TEST(keep_sub_negative_result) {
    /* c = 3 - 10 = -7, len=10 -> negative -> not safe */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *a = xi_const_int(f, blk, 3, &stub_int);
    XiValue *b = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c = xi_binary(f, blk, XI_SUB, &stub_int, a, b);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, c, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

/* ========== Same-Block Dedup Variations ========== */

TEST(dedup_three_identical_same_block) {
    /* Three identical checks; first kept, 2nd and 3rd deduped. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);
    XiValue *bc3 = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    ASSERT(bc3->op == XI_COPY);
    xi_func_free(f);
}

TEST(no_dedup_different_idx_same_len) {
    /* Same length, different indices -> no dedup (different keys). */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx1 = xi_const_int(f, blk, 200, &stub_int);
    XiValue *idx2 = xi_const_int(f, blk, 201, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx1, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx2, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(no_dedup_same_idx_different_len) {
    /* Same index, different lengths -> no dedup (different keys). */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len1 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *len2 = xi_const_int(f, blk, 150, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len1);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len2);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(dedup_two_pairs_same_block) {
    /* Two distinct (idx, len) pairs, each appearing twice.
     * Each pair's second occurrence should be deduped independently. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx_a = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len_a = xi_const_int(f, blk, 100, &stub_int);
    XiValue *idx_b = xi_const_int(f, blk, 300, &stub_int);
    XiValue *len_b = xi_const_int(f, blk, 150, &stub_int);

    XiValue *bc_a1 = make_bounds_check(f, blk, idx_a, len_a);
    XiValue *bc_b1 = make_bounds_check(f, blk, idx_b, len_b);
    XiValue *bc_a2 = make_bounds_check(f, blk, idx_a, len_a);
    XiValue *bc_b2 = make_bounds_check(f, blk, idx_b, len_b);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_a1->op == XI_BOUNDS_CHECK);
    ASSERT(bc_b1->op == XI_BOUNDS_CHECK);
    ASSERT(bc_a2->op == XI_COPY);
    ASSERT(bc_b2->op == XI_COPY);
    xi_func_free(f);
}

/* ========== Cross-Block Dominator Dedup ========== */

TEST(dedup_chain_three_blocks) {
    /* entry -> mid -> tail; check in entry dominates tail. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *mid = xi_block_new(f);
    XiBlock *tail = xi_block_new(f);
    mid->sealed = true;
    tail->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, mid, 0);
    mid->idom = entry;

    mid->kind = XI_BLOCK_PLAIN;
    wire(mid, tail, 0);
    tail->idom = mid;

    tail->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);
    XiValue *bc2 = make_bounds_check(f, tail, idx, len);
    tail->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

TEST(dedup_diamond_both_branches) {
    /*        entry (check here)
     *        /   \
     *     then   else  (check in both)
     *        \   /
     *        merge
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;
    merge->sealed = true;

    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    wire(entry, then_blk, 0);
    wire(entry, else_blk, 1);
    then_blk->idom = entry;
    else_blk->idom = entry;

    then_blk->kind = XI_BLOCK_PLAIN;
    wire(then_blk, merge, 0);
    else_blk->kind = XI_BLOCK_PLAIN;
    wire(else_blk, merge, 0);
    merge->idom = entry;
    merge->kind = XI_BLOCK_RETURN;
    merge->control = cond;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc_entry = make_bounds_check(f, entry, idx, len);
    XiValue *bc_then = make_bounds_check(f, then_blk, idx, len);
    XiValue *bc_else = make_bounds_check(f, else_blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_entry->op == XI_BOUNDS_CHECK);
    ASSERT(bc_then->op == XI_COPY);
    ASSERT(bc_else->op == XI_COPY);
    xi_func_free(f);
}

TEST(no_dedup_sibling_branches) {
    /* Check in then-branch, same check in else-branch.
     * Neither dominates the other -> no dedup. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    then_blk->sealed = true;
    else_blk->sealed = true;

    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    wire(entry, then_blk, 0);
    wire(entry, else_blk, 1);
    then_blk->idom = entry;
    else_blk->idom = entry;

    then_blk->kind = XI_BLOCK_RETURN;
    else_blk->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc_then = make_bounds_check(f, then_blk, idx, len);
    XiValue *bc_else = make_bounds_check(f, else_blk, idx, len);
    then_blk->control = idx;
    else_blk->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_then->op == XI_BOUNDS_CHECK);
    ASSERT(bc_else->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(dedup_deep_chain_four_blocks) {
    /* entry -> b1 -> b2 -> b3; check in entry dominates b3. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);
    b1->sealed = true;
    b2->sealed = true;
    b3->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, b1, 0);
    b1->idom = entry;

    b1->kind = XI_BLOCK_PLAIN;
    wire(b1, b2, 0);
    b2->idom = b1;

    b2->kind = XI_BLOCK_PLAIN;
    wire(b2, b3, 0);
    b3->idom = b2;

    b3->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);
    XiValue *bc3 = make_bounds_check(f, b3, idx, len);
    b3->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc3->op == XI_COPY);
    xi_func_free(f);
}

TEST(no_dedup_without_idom_set) {
    /* Successor exists but idom is NULL -> dominator walk fails. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    /* Intentionally do NOT set succ->idom */

    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);
    XiValue *bc2 = make_bounds_check(f, succ, idx, len);
    succ->control = idx;

    xi_range_analyze(f);
    /* Range analysis computes dominators as a loop-forest prerequisite and
     * fills succ->idom; clear it again so the dedup dominator walk really
     * runs without idom information. */
    succ->idom = NULL;
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

/* ========== Mixed Strategy Tests ========== */

TEST(range_safe_not_recorded_for_dedup) {
    /* If first check is range-safe and eliminated, it's NOT recorded
     * in the seen table, so a second identical check would need its
     * own range proof.  Both are range-safe here. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 3, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_COPY);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

TEST(range_and_dedup_mixed) {
    /* bc1: range-safe (idx=3, len=10) -> eliminated by range.
     * bc2: unsafe (idx=200, len=100) -> kept, recorded.
     * bc3: same as bc2 -> eliminated by dedup. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx_safe = xi_const_int(f, blk, 3, &stub_int);
    XiValue *len1 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx_safe, len1);

    XiValue *idx_unsafe = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len2 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc2 = make_bounds_check(f, blk, idx_unsafe, len2);
    XiValue *bc3 = make_bounds_check(f, blk, idx_unsafe, len2);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_COPY);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    ASSERT(bc3->op == XI_COPY);
    xi_func_free(f);
}

TEST(unsafe_first_dedup_second) {
    /* First check is unsafe (kept, recorded); second identical is deduped. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    succ->idom = entry;
    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);
    XiValue *bc2 = make_bounds_check(f, succ, idx, len);
    succ->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

/* ========== Parameter (Unknown Range) Tests ========== */

TEST(keep_param_idx_const_len) {
    /* Param index has TOP range -> range proof fails -> kept. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_param(f, blk, 0, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(dedup_param_idx_same_block) {
    /* Same param check twice in same block -> second deduped. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_param(f, blk, 0, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

TEST(dedup_param_across_blocks) {
    /* Param check in entry, same in successor -> deduped. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    succ->idom = entry;
    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_param(f, entry, 0, &stub_int);
    XiValue *len = xi_const_int(f, entry, 10, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);
    XiValue *bc2 = make_bounds_check(f, succ, idx, len);
    succ->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

TEST(keep_param_different_lens) {
    /* Param checked against different lengths -> no dedup. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_param(f, blk, 0, &stub_int);
    XiValue *len1 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *len2 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len1);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len2);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

/* ========== Independent Arrays ========== */

TEST(independent_arrays_both_safe) {
    /* Two different (idx, len) pairs, both range-safe. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx_a = xi_const_int(f, blk, 2, &stub_int);
    XiValue *len_a = xi_const_int(f, blk, 10, &stub_int);
    XiValue *idx_b = xi_const_int(f, blk, 7, &stub_int);
    XiValue *len_b = xi_const_int(f, blk, 20, &stub_int);

    XiValue *bc_a = make_bounds_check(f, blk, idx_a, len_a);
    XiValue *bc_b = make_bounds_check(f, blk, idx_b, len_b);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_a->op == XI_COPY);
    ASSERT(bc_b->op == XI_COPY);
    xi_func_free(f);
}

TEST(independent_arrays_one_safe_one_unsafe) {
    /* Array A is safe, Array B is not. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx_a = xi_const_int(f, blk, 2, &stub_int);
    XiValue *len_a = xi_const_int(f, blk, 10, &stub_int);
    XiValue *idx_b = xi_const_int(f, blk, 15, &stub_int);
    XiValue *len_b = xi_const_int(f, blk, 10, &stub_int);

    XiValue *bc_a = make_bounds_check(f, blk, idx_a, len_a);
    XiValue *bc_b = make_bounds_check(f, blk, idx_b, len_b);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_a->op == XI_COPY);
    ASSERT(bc_b->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(independent_arrays_no_cross_dedup) {
    /* Same index value but different length values -> dedup
     * requires exact (idx.id, len.id) match, so no cross-dedup. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len_a = xi_const_int(f, blk, 100, &stub_int);
    XiValue *len_b = xi_const_int(f, blk, 150, &stub_int);

    XiValue *bc_a = make_bounds_check(f, blk, idx, len_a);
    XiValue *bc_b = make_bounds_check(f, blk, idx, len_b);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc_a->op == XI_BOUNDS_CHECK);
    ASSERT(bc_b->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

/* ========== Structural / No-Op Edge Cases ========== */

TEST(no_bounds_checks_in_func) {
    /* Function has values but no BOUNDS_CHECK -> no change. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *a = xi_const_int(f, blk, 42, &stub_int);
    XiValue *b = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c = xi_binary(f, blk, XI_ADD, &stub_int, a, b);
    (void) c;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(a->op == XI_CONST);
    ASSERT(b->op == XI_CONST);
    xi_func_free(f);
}

TEST(single_check_safe) {
    /* Exactly one check, and it's range-safe. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 4, &stub_int);
    XiValue *len = xi_const_int(f, blk, 5, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

TEST(single_check_unsafe) {
    /* Exactly one check, and it's NOT safe. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 5, &stub_int);
    XiValue *len = xi_const_int(f, blk, 5, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(multiple_empty_blocks_no_crash) {
    /* Several empty blocks -> no crash, no change. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *b1 = xi_block_new(f);
    XiBlock *b2 = xi_block_new(f);
    XiBlock *b3 = xi_block_new(f);
    b1->sealed = true;
    b2->sealed = true;
    b3->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, b1, 0);
    b1->kind = XI_BLOCK_PLAIN;
    wire(b1, b2, 0);
    b2->kind = XI_BLOCK_PLAIN;
    wire(b2, b3, 0);
    b3->kind = XI_BLOCK_RETURN;

    f->invariant_mask |= XI_INV_RANGE_ANNOTATED;
    xi_opt_bce(f);

    xi_func_free(f);
}

TEST(check_in_non_entry_block_only) {
    /* Check only in successor, no prior check -> must stand alone. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    succ->idom = entry;
    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, succ, 200, &stub_int);
    XiValue *len = xi_const_int(f, succ, 100, &stub_int);
    XiValue *bc = make_bounds_check(f, succ, idx, len);
    succ->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    xi_func_free(f);
}

TEST(check_in_non_entry_range_safe) {
    /* Check only in successor, but range-safe. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    succ->idom = entry;
    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, succ, 3, &stub_int);
    XiValue *len = xi_const_int(f, succ, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, succ, idx, len);
    succ->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    xi_func_free(f);
}

/* ========== Flag / Correctness Verification ========== */

TEST(flags_preserved_on_kept_check) {
    /* Kept check must retain SIDE_EFFECT and MAY_THROW. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 15, &stub_int);
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_BOUNDS_CHECK);
    ASSERT(bc->flags & XI_FLAG_SIDE_EFFECT);
    ASSERT(bc->flags & XI_FLAG_MAY_THROW);
    xi_func_free(f);
}

TEST(eliminated_copy_result_is_idx) {
    /* After elimination, COPY's single arg must be the original index. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 7, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc->op == XI_COPY);
    ASSERT(bc->nargs == 1);
    ASSERT(bc->args[0] == idx);
    xi_func_free(f);
}

TEST(multiple_eliminations_all_flags_cleared) {
    /* Batch of eliminated checks: all have flags cleared. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);

    XiValue *bc[10];
    for (int i = 0; i < 10; i++) {
        XiValue *idx = xi_const_int(f, blk, i * 5, &stub_int);
        bc[i] = make_bounds_check(f, blk, idx, len);
    }

    xi_range_analyze(f);
    xi_opt_bce(f);

    for (int i = 0; i < 10; i++) {
        ASSERT(bc[i]->op == XI_COPY);
        ASSERT(!(bc[i]->flags & XI_FLAG_SIDE_EFFECT));
        ASSERT(!(bc[i]->flags & XI_FLAG_MAY_THROW));
    }
    xi_func_free(f);
}

TEST(dedup_clears_flags) {
    /* Dedup-eliminated check also has flags cleared. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc1->flags & XI_FLAG_SIDE_EFFECT);
    ASSERT(bc2->op == XI_COPY);
    ASSERT(!(bc2->flags & XI_FLAG_SIDE_EFFECT));
    ASSERT(!(bc2->flags & XI_FLAG_MAY_THROW));
    xi_func_free(f);
}

TEST(kept_check_args_unchanged) {
    /* The first (kept) check's args must remain untouched. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *idx = xi_const_int(f, blk, 200, &stub_int);
    XiValue *len = xi_const_int(f, blk, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, blk, idx, len);
    XiValue *bc2 = make_bounds_check(f, blk, idx, len);
    (void) bc2;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc1->nargs == 2);
    ASSERT(bc1->args[0] == idx);
    ASSERT(bc1->args[1] == len);
    xi_func_free(f);
}

/* ========== Stress / Scale Tests ========== */

TEST(twenty_checks_all_safe) {
    /* 20 checks with indices 0..19, len=20 -> all eliminated. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;
    XiValue *len = xi_const_int(f, blk, 20, &stub_int);

    XiValue *bc[20];
    for (int i = 0; i < 20; i++) {
        XiValue *idx = xi_const_int(f, blk, i, &stub_int);
        bc[i] = make_bounds_check(f, blk, idx, len);
    }

    xi_range_analyze(f);
    xi_opt_bce(f);

    for (int i = 0; i < 20; i++) {
        ASSERT(bc[i]->op == XI_COPY);
    }
    xi_func_free(f);
}

TEST(twenty_checks_all_unsafe) {
    /* 20 checks with indices 100..119, len=50 -> none eliminated
     * (different idx values prevent dedup). */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;
    XiValue *len = xi_const_int(f, blk, 50, &stub_int);

    XiValue *bc[20];
    for (int i = 0; i < 20; i++) {
        XiValue *idx = xi_const_int(f, blk, 100 + i, &stub_int);
        bc[i] = make_bounds_check(f, blk, idx, len);
    }

    xi_range_analyze(f);
    xi_opt_bce(f);

    for (int i = 0; i < 20; i++) {
        ASSERT(bc[i]->op == XI_BOUNDS_CHECK);
    }
    xi_func_free(f);
}

TEST(interleaved_safe_unsafe) {
    /* Alternating safe and unsafe checks: even=safe, odd=unsafe. */
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;
    XiValue *len = xi_const_int(f, blk, 10, &stub_int);

    XiValue *bc[10];
    for (int i = 0; i < 10; i++) {
        int v = (i % 2 == 0) ? i / 2 : 10 + i;
        XiValue *idx = xi_const_int(f, blk, v, &stub_int);
        bc[i] = make_bounds_check(f, blk, idx, len);
    }

    xi_range_analyze(f);
    xi_opt_bce(f);

    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            ASSERT(bc[i]->op == XI_COPY);
        } else {
            ASSERT(bc[i]->op == XI_BOUNDS_CHECK);
        }
    }
    xi_func_free(f);
}

/* ========== Dedup Across Complex CFG ========== */

TEST(dedup_dominator_then_safe_in_successor) {
    /* entry: unsafe check (kept).
     * succ: safe check with DIFFERENT args (eliminated by range, not dedup).
     * Verifies the two strategies are independent. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *succ = xi_block_new(f);
    succ->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, succ, 0);
    succ->idom = entry;
    succ->kind = XI_BLOCK_RETURN;

    XiValue *idx1 = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len1 = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx1, len1);

    XiValue *idx2 = xi_const_int(f, succ, 5, &stub_int);
    XiValue *len2 = xi_const_int(f, succ, 20, &stub_int);
    XiValue *bc2 = make_bounds_check(f, succ, idx2, len2);
    succ->control = idx2;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_COPY);
    xi_func_free(f);
}

TEST(dedup_chain_with_intermediate_check) {
    /* entry -> mid -> tail
     * entry: check(idx, len) -- kept
     * mid:   check(idx2, len2) -- different, kept
     * tail:  check(idx, len) -- same as entry, deduped */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *mid = xi_block_new(f);
    XiBlock *tail = xi_block_new(f);
    mid->sealed = true;
    tail->sealed = true;

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, mid, 0);
    mid->idom = entry;
    mid->kind = XI_BLOCK_PLAIN;
    wire(mid, tail, 0);
    tail->idom = mid;
    tail->kind = XI_BLOCK_RETURN;

    XiValue *idx = xi_const_int(f, entry, 200, &stub_int);
    XiValue *len = xi_const_int(f, entry, 100, &stub_int);
    XiValue *bc1 = make_bounds_check(f, entry, idx, len);

    XiValue *idx2 = xi_const_int(f, mid, 300, &stub_int);
    XiValue *len2 = xi_const_int(f, mid, 150, &stub_int);
    XiValue *bc2 = make_bounds_check(f, mid, idx2, len2);

    XiValue *bc3 = make_bounds_check(f, tail, idx, len);
    tail->control = idx;

    xi_range_analyze(f);
    xi_opt_bce(f);

    ASSERT(bc1->op == XI_BOUNDS_CHECK);
    ASSERT(bc2->op == XI_BOUNDS_CHECK);
    ASSERT(bc3->op == XI_COPY);
    xi_func_free(f);
}

/* ========== Runner ========== */

int main(void) {
    printf("=== Xi BCE Tests ===\n\n");

    /* Range proof — original */
    run_eliminate_const_idx_const_len();
    run_eliminate_zero_idx();
    run_keep_negative_idx();
    run_keep_idx_equals_len();
    run_keep_idx_exceeds_len();
    run_eliminate_boundary_idx();

    /* Range proof — boundary constants */
    run_keep_zero_idx_zero_len();
    run_keep_idx_one_len_one();
    run_eliminate_idx_one_len_two();
    run_keep_very_large_negative_idx();
    run_eliminate_large_constants();
    run_keep_max_int_idx();
    run_keep_huge_idx_small_len();

    /* Range proof — derived ranges (ADD / SUB) */
    run_eliminate_add_derived_range();
    run_keep_add_derived_oob();
    run_eliminate_sub_derived_range();
    run_keep_sub_negative_result();

    /* Dominator dedup — original */
    run_dedup_same_block();
    run_dedup_dominating_block();

    /* Same-block dedup variations */
    run_dedup_three_identical_same_block();
    run_no_dedup_different_idx_same_len();
    run_no_dedup_same_idx_different_len();
    run_dedup_two_pairs_same_block();

    /* Cross-block dominator dedup */
    run_dedup_chain_three_blocks();
    run_dedup_diamond_both_branches();
    run_no_dedup_sibling_branches();
    run_dedup_deep_chain_four_blocks();
    run_no_dedup_without_idom_set();

    /* Mixed strategy */
    run_range_safe_not_recorded_for_dedup();
    run_range_and_dedup_mixed();
    run_unsafe_first_dedup_second();

    /* Parameter (unknown range) */
    run_keep_param_idx_const_len();
    run_dedup_param_idx_same_block();
    run_dedup_param_across_blocks();
    run_keep_param_different_lens();

    /* Independent arrays */
    run_independent_arrays_both_safe();
    run_independent_arrays_one_safe_one_unsafe();
    run_independent_arrays_no_cross_dedup();

    /* Edge cases — original */
    run_no_elimination_without_range();
    run_empty_func_no_crash();

    /* Structural / no-op edge cases */
    run_no_bounds_checks_in_func();
    run_single_check_safe();
    run_single_check_unsafe();
    run_multiple_empty_blocks_no_crash();
    run_check_in_non_entry_block_only();
    run_check_in_non_entry_range_safe();

    /* Multiple checks — original */
    run_multiple_safe_checks();
    run_mixed_safe_unsafe();
    run_side_effect_flag_cleared();

    /* Flag / correctness verification */
    run_flags_preserved_on_kept_check();
    run_eliminated_copy_result_is_idx();
    run_multiple_eliminations_all_flags_cleared();
    run_dedup_clears_flags();
    run_kept_check_args_unchanged();

    /* Stress / scale */
    run_twenty_checks_all_safe();
    run_twenty_checks_all_unsafe();
    run_interleaved_safe_unsafe();

    /* Complex CFG */
    run_dedup_dominator_then_safe_in_successor();
    run_dedup_chain_with_intermediate_check();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
