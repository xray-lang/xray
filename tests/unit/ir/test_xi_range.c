/*
 * Unit tests for Xi range analysis (xi_range.h / xi_range.c).
 * Covers lattice operations, arithmetic transfer functions, queries,
 * and the analysis pass on synthetic IR.
 */

#include "../../../src/ir/xi_range.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_opt_sccp.h"
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

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__);                            \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ========== Lattice Constructor Tests ========== */

TEST(range_top) {
    XiRange r = xi_range_top();
    ASSERT(r.is_top);
    ASSERT(!r.is_bot);
}

TEST(range_bot) {
    XiRange r = xi_range_bot();
    ASSERT(!r.is_top);
    ASSERT(r.is_bot);
}

TEST(range_const) {
    XiRange r = xi_range_const(42);
    ASSERT(!r.is_top && !r.is_bot);
    ASSERT(r.lo == 42 && r.hi == 42);
    ASSERT(xi_range_is_const(r));
}

TEST(range_make) {
    XiRange r = xi_range_make(-10, 20);
    ASSERT(!r.is_top && !r.is_bot);
    ASSERT(r.lo == -10 && r.hi == 20);
    ASSERT(!xi_range_is_const(r));
}

/* ========== Lattice Operation Tests ========== */

TEST(union_bot_identity) {
    XiRange a = xi_range_const(5);
    XiRange b = xi_range_bot();
    XiRange u = xi_range_union(a, b);
    ASSERT(!u.is_top && !u.is_bot);
    ASSERT(u.lo == 5 && u.hi == 5);
}

TEST(union_top_absorbs) {
    XiRange a = xi_range_const(5);
    XiRange b = xi_range_top();
    XiRange u = xi_range_union(a, b);
    ASSERT(u.is_top);
}

TEST(union_widens) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 20);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 20);
}

TEST(intersect_top_identity) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_top();
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 0 && i.hi == 10);
}

TEST(intersect_overlap) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 20);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 5 && i.hi == 10);
}

TEST(intersect_disjoint_is_bot) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(10, 20);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.is_bot);
}

TEST(intersect_bot_absorbs) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_bot();
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.is_bot);
}

/* ========== Query Tests ========== */

TEST(known_nonneg) {
    ASSERT(xi_range_known_nonneg(xi_range_make(0, 100)));
    ASSERT(xi_range_known_nonneg(xi_range_const(0)));
    ASSERT(!xi_range_known_nonneg(xi_range_make(-1, 100)));
    ASSERT(!xi_range_known_nonneg(xi_range_top()));
}

TEST(known_positive) {
    ASSERT(xi_range_known_positive(xi_range_make(1, 100)));
    ASSERT(!xi_range_known_positive(xi_range_make(0, 100)));
    ASSERT(!xi_range_known_positive(xi_range_top()));
}

TEST(known_less_than) {
    ASSERT(xi_range_known_less_than(xi_range_make(0, 9), 10));
    ASSERT(!xi_range_known_less_than(xi_range_make(0, 10), 10));
    ASSERT(!xi_range_known_less_than(xi_range_top(), 10));
}

TEST(known_ge) {
    ASSERT(xi_range_known_ge(xi_range_make(5, 10), 5));
    ASSERT(!xi_range_known_ge(xi_range_make(4, 10), 5));
}

TEST(contains) {
    ASSERT(xi_range_contains(xi_range_make(0, 100), xi_range_make(10, 50)));
    ASSERT(xi_range_contains(xi_range_make(0, 100), xi_range_const(50)));
    ASSERT(!xi_range_contains(xi_range_make(10, 50), xi_range_make(0, 100)));
    ASSERT(xi_range_contains(xi_range_top(), xi_range_make(0, 100)));
    ASSERT(xi_range_contains(xi_range_make(0, 100), xi_range_bot()));
}

/* ========== Arithmetic Transfer Function Tests ========== */

TEST(add_ranges) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_make(10, 20);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == 11 && r.hi == 25);
}

TEST(add_overflow_to_top) {
    XiRange a = xi_range_make(INT64_MAX - 1, INT64_MAX);
    XiRange b = xi_range_make(1, 2);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

TEST(sub_ranges) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_make(1, 5);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == 5 && r.hi == 19);
}

TEST(mul_ranges) {
    XiRange a = xi_range_make(2, 3);
    XiRange b = xi_range_make(4, 5);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 8 && r.hi == 15);
}

TEST(mul_negative) {
    XiRange a = xi_range_make(-3, -1);
    XiRange b = xi_range_make(2, 4);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == -12 && r.hi == -2);
}

TEST(mul_overflow_to_top) {
    XiRange a = xi_range_make(INT64_MAX / 2, INT64_MAX);
    XiRange b = xi_range_make(2, 3);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_top);
}

TEST(neg_range) {
    XiRange a = xi_range_make(-5, 10);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == -10 && r.hi == 5);
}

TEST(neg_int64_min_to_top) {
    XiRange a = xi_range_make(INT64_MIN, 0);
    XiRange r = xi_range_neg(a);
    ASSERT(r.is_top);
}

/* ========== Analysis Pass Tests ========== */

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_range", &stub_int);
    XiBlock *entry = xi_block_new(f);
    f->entry = entry;
    return f;
}

TEST(const_range_query) {
    XiFunc *f = make_func();
    XiValue *c = xi_const_int(f, f->entry, 42, &stub_int);

    /* xi_range_of for constants should return [42, 42] directly. */
    XiRange r = xi_range_of(c);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == 42 && r.hi == 42);

    xi_func_free(f);
}

TEST(non_int_is_top) {
    XiFunc *f = make_func();
    XiValue *s = xi_const_str(f, f->entry, "hello", &stub_str);

    XiRange r = xi_range_of(s);
    ASSERT(r.is_top);

    xi_func_free(f);
}

TEST(analyze_const_add) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 20, &stub_int);

    XiValue *add = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add->args[0] = c1;
    add->args[1] = c2;

    xi_range_analyze(f);

    XiRange r = xi_range_of(add);
    ASSERT(!r.is_top && !r.is_bot);
    ASSERT(r.lo == 30 && r.hi == 30);

    xi_func_free(f);
}

TEST(analyze_sets_invariant) {
    XiFunc *f = make_func();
    xi_const_int(f, f->entry, 1, &stub_int);

    xi_range_analyze(f);

    ASSERT(f->invariant_mask & XI_INV_RANGE_ANNOTATED);

    xi_func_free(f);
}

/* ========== SCCP Branch Elimination Tests ========== */

/* Build a diamond CFG: entry -> {then, else} -> merge
 * entry has IF(cmp), cmp compares two phi-merged values.
 * If SCCP can prove the comparison from ranges, it should eliminate a branch. */
TEST(sccp_eliminates_dead_branch_lt) {
    /* Build: entry has two consts 3 and 7, compare LT.
     * Since 3 < 7 is always true, SCCP should fold to PLAIN -> then. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *c7 = xi_const_int(f, entry, 7, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_LT, &stub_int, 2);
    cmp->args[0] = c3;
    cmp->args[1] = c7;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c3;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c7;

    xi_opt_sccp(f);

    /* Entry should become PLAIN -> then_blk (since 3 < 7 is true). */
    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_eliminates_dead_branch_ge_false) {
    /* 2 >= 10 is always false -> SCCP folds to else branch. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c2 = xi_const_int(f, entry, 2, &stub_int);
    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GE, &stub_int, 2);
    cmp->args[0] = c2;
    cmp->args[1] = c10;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c2;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c10;

    xi_opt_sccp(f);

    /* 2 >= 10 is false -> entry becomes PLAIN -> else_blk. */
    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == else_blk);

    xi_func_free(f);
}

TEST(sccp_removes_unreachable_block) {
    /* Dead branch should result in block removal. */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c1 = xi_const_int(f, entry, 1, &stub_int);
    XiValue *c0 = xi_const_int(f, entry, 0, &stub_int);
    /* 1 == 0 is always false */
    XiValue *cmp = xi_value_new(f, entry, XI_EQ, &stub_int, 2);
    cmp->args[0] = c1;
    cmp->args[1] = c0;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c1;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c0;

    uint32_t orig_nblocks = f->nblocks;
    xi_opt_sccp(f);

    /* then_blk is unreachable (1==0 is false), should be removed. */
    ASSERT(f->nblocks < orig_nblocks);

    xi_func_free(f);
}

/* ========== Additional Range Lattice Tests ========== */

TEST(union_symmetric) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(3, 10);
    XiRange u1 = xi_range_union(a, b);
    XiRange u2 = xi_range_union(b, a);
    ASSERT(u1.lo == u2.lo && u1.hi == u2.hi);
}

TEST(intersect_symmetric) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 15);
    XiRange i1 = xi_range_intersect(a, b);
    XiRange i2 = xi_range_intersect(b, a);
    ASSERT(i1.lo == i2.lo && i1.hi == i2.hi);
}

TEST(union_same_is_identity) {
    XiRange a = xi_range_make(3, 7);
    XiRange u = xi_range_union(a, a);
    ASSERT(u.lo == 3 && u.hi == 7);
}

TEST(intersect_same_is_identity) {
    XiRange a = xi_range_make(3, 7);
    XiRange i = xi_range_intersect(a, a);
    ASSERT(i.lo == 3 && i.hi == 7);
}

TEST(union_adjacent) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(6, 10);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 10);
}

TEST(intersect_adjacent_is_bot) {
    XiRange a = xi_range_make(0, 4);
    XiRange b = xi_range_make(5, 10);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.is_bot);
}

TEST(intersect_touching) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(5, 10);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 5 && i.hi == 5);
}

TEST(add_const_ranges) {
    XiRange a = xi_range_const(3);
    XiRange b = xi_range_const(4);
    XiRange r = xi_range_add(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == 7);
}

TEST(sub_const_ranges) {
    XiRange a = xi_range_const(10);
    XiRange b = xi_range_const(3);
    XiRange r = xi_range_sub(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == 7);
}

TEST(mul_by_zero) {
    XiRange a = xi_range_make(-100, 100);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_mul(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == 0);
}

TEST(neg_const) {
    XiRange a = xi_range_const(5);
    XiRange r = xi_range_neg(a);
    ASSERT(xi_range_is_const(r) && r.lo == -5);
}

TEST(add_negative_ranges) {
    XiRange a = xi_range_make(-10, -5);
    XiRange b = xi_range_make(-3, -1);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == -13 && r.hi == -6);
}

TEST(sub_negative_result) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(10, 20);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == -20 && r.hi == -5);
}

TEST(mul_mixed_sign) {
    XiRange a = xi_range_make(-2, 3);
    XiRange b = xi_range_make(1, 4);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == -8 && r.hi == 12);
}

TEST(add_top_propagates) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_top();
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

TEST(sub_bot_propagates) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_bot();
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_bot);
}

TEST(mul_top_propagates) {
    XiRange a = xi_range_top();
    XiRange b = xi_range_const(5);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_top);
}

TEST(neg_bot_is_bot) {
    XiRange r = xi_range_neg(xi_range_bot());
    ASSERT(r.is_bot);
}

TEST(neg_top_is_top) {
    XiRange r = xi_range_neg(xi_range_top());
    ASSERT(r.is_top);
}

TEST(known_nonneg_negative_lo) {
    ASSERT(!xi_range_known_nonneg(xi_range_make(-1, INT64_MAX)));
}

TEST(known_positive_at_boundary) {
    ASSERT(xi_range_known_positive(xi_range_const(1)));
    ASSERT(!xi_range_known_positive(xi_range_const(0)));
}

TEST(known_lt_at_boundary) {
    ASSERT(xi_range_known_less_than(xi_range_const(9), 10));
    ASSERT(!xi_range_known_less_than(xi_range_const(10), 10));
}

TEST(known_ge_at_boundary) {
    ASSERT(xi_range_known_ge(xi_range_const(5), 5));
    ASSERT(!xi_range_known_ge(xi_range_const(4), 5));
}

TEST(contains_top_contains_all) {
    ASSERT(xi_range_contains(xi_range_top(), xi_range_make(INT64_MIN, INT64_MAX)));
}

TEST(contains_bot_contained_by_all) {
    ASSERT(xi_range_contains(xi_range_const(0), xi_range_bot()));
}

TEST(sub_overflow_to_top) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(add_max_boundary) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_add(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MAX);
}

TEST(neg_symmetric) {
    XiRange a = xi_range_make(-5, 5);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == -5 && r.hi == 5);
}

TEST(union_bot_bot_is_bot) {
    XiRange r = xi_range_union(xi_range_bot(), xi_range_bot());
    ASSERT(r.is_bot);
}

TEST(intersect_top_top_is_top) {
    XiRange r = xi_range_intersect(xi_range_top(), xi_range_top());
    ASSERT(r.is_top);
}

TEST(union_top_top_is_top) {
    XiRange r = xi_range_union(xi_range_top(), xi_range_top());
    ASSERT(r.is_top);
}

/* ========== Runner ========== */

int main(void) {
    printf("=== Xi Range Analysis Tests ===\n\n");

    /* Lattice constructors */
    run_range_top();
    run_range_bot();
    run_range_const();
    run_range_make();

    /* Lattice operations */
    run_union_bot_identity();
    run_union_top_absorbs();
    run_union_widens();
    run_intersect_top_identity();
    run_intersect_overlap();
    run_intersect_disjoint_is_bot();
    run_intersect_bot_absorbs();

    /* Queries */
    run_known_nonneg();
    run_known_positive();
    run_known_less_than();
    run_known_ge();
    run_contains();

    /* Arithmetic */
    run_add_ranges();
    run_add_overflow_to_top();
    run_sub_ranges();
    run_mul_ranges();
    run_mul_negative();
    run_mul_overflow_to_top();
    run_neg_range();
    run_neg_int64_min_to_top();

    /* Analysis pass */
    run_const_range_query();
    run_non_int_is_top();
    run_analyze_const_add();
    run_analyze_sets_invariant();

    /* SCCP branch elimination */
    run_sccp_eliminates_dead_branch_lt();
    run_sccp_eliminates_dead_branch_ge_false();
    run_sccp_removes_unreachable_block();

    /* Additional lattice tests */
    run_union_symmetric();
    run_intersect_symmetric();
    run_union_same_is_identity();
    run_intersect_same_is_identity();
    run_union_adjacent();
    run_intersect_adjacent_is_bot();
    run_intersect_touching();
    run_add_const_ranges();
    run_sub_const_ranges();
    run_mul_by_zero();
    run_neg_const();
    run_add_negative_ranges();
    run_sub_negative_result();
    run_mul_mixed_sign();
    run_add_top_propagates();
    run_sub_bot_propagates();
    run_mul_top_propagates();
    run_neg_bot_is_bot();
    run_neg_top_is_top();
    run_known_nonneg_negative_lo();
    run_known_positive_at_boundary();
    run_known_lt_at_boundary();
    run_known_ge_at_boundary();
    run_contains_top_contains_all();
    run_contains_bot_contained_by_all();
    run_sub_overflow_to_top();
    run_add_max_boundary();
    run_neg_symmetric();
    run_union_bot_bot_is_bot();
    run_intersect_top_top_is_top();
    run_union_top_top_is_top();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
