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
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 5, .frozen = true};

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

/* ========== Runner ========== */

int main(void) {
    printf("=== Xi BCE Tests ===\n\n");

    /* Range proof */
    run_eliminate_const_idx_const_len();
    run_eliminate_zero_idx();
    run_keep_negative_idx();
    run_keep_idx_equals_len();
    run_keep_idx_exceeds_len();
    run_eliminate_boundary_idx();

    /* Dominator dedup */
    run_dedup_same_block();
    run_dedup_dominating_block();

    /* Edge cases */
    run_no_elimination_without_range();
    run_empty_func_no_crash();

    /* Multiple checks */
    run_multiple_safe_checks();
    run_mixed_safe_unsafe();
    run_side_effect_flag_cleared();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
