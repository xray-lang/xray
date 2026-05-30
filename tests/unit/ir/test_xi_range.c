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

/* ========== Constructor Edge Case Tests ========== */

TEST(range_const_zero) {
    XiRange r = xi_range_const(0);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == 0 && r.hi == 0);
}

TEST(range_const_negative) {
    XiRange r = xi_range_const(-42);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == -42 && r.hi == -42);
}

TEST(range_const_int64_min) {
    XiRange r = xi_range_const(INT64_MIN);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == INT64_MIN && r.hi == INT64_MIN);
}

TEST(range_const_int64_max) {
    XiRange r = xi_range_const(INT64_MAX);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == INT64_MAX && r.hi == INT64_MAX);
}

TEST(range_make_full) {
    XiRange r = xi_range_make(INT64_MIN, INT64_MAX);
    ASSERT(!r.is_top && !r.is_bot);
    ASSERT(r.lo == INT64_MIN && r.hi == INT64_MAX);
}

TEST(range_make_single_element) {
    XiRange r = xi_range_make(7, 7);
    ASSERT(xi_range_is_const(r));
    ASSERT(r.lo == 7 && r.hi == 7);
}

TEST(range_make_two_elements) {
    XiRange r = xi_range_make(3, 4);
    ASSERT(!xi_range_is_const(r));
    ASSERT(r.lo == 3 && r.hi == 4);
}

TEST(range_make_negative_span) {
    XiRange r = xi_range_make(-100, -1);
    ASSERT(r.lo == -100 && r.hi == -1);
}

/* ========== Additional Lattice Union Tests ========== */

TEST(union_const_with_range) {
    XiRange a = xi_range_const(5);
    XiRange b = xi_range_make(0, 10);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 10);
}

TEST(union_negative_ranges) {
    XiRange a = xi_range_make(-20, -10);
    XiRange b = xi_range_make(-15, -5);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == -20 && u.hi == -5);
}

TEST(union_disjoint_far) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(1000, 2000);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 2000);
}

TEST(union_inner_contained) {
    XiRange a = xi_range_make(0, 100);
    XiRange b = xi_range_make(20, 50);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 100);
}

TEST(union_bot_with_top) {
    XiRange u = xi_range_union(xi_range_bot(), xi_range_top());
    ASSERT(u.is_top);
}

TEST(union_top_with_bot) {
    XiRange u = xi_range_union(xi_range_top(), xi_range_bot());
    ASSERT(u.is_top);
}

TEST(union_negative_to_positive) {
    XiRange a = xi_range_make(-50, -1);
    XiRange b = xi_range_make(1, 50);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == -50 && u.hi == 50);
}

TEST(union_wide_covers_narrow) {
    XiRange a = xi_range_make(-1000, 1000);
    XiRange b = xi_range_make(-1, 1);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == -1000 && u.hi == 1000);
}

TEST(union_const_const_same) {
    XiRange a = xi_range_const(42);
    XiRange b = xi_range_const(42);
    XiRange u = xi_range_union(a, b);
    ASSERT(xi_range_is_const(u) && u.lo == 42);
}

TEST(union_const_const_different) {
    XiRange a = xi_range_const(3);
    XiRange b = xi_range_const(7);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 3 && u.hi == 7);
}

TEST(union_extends_left) {
    XiRange a = xi_range_make(5, 10);
    XiRange b = xi_range_make(0, 6);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 10);
}

TEST(union_extends_right) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(3, 20);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 0 && u.hi == 20);
}

TEST(union_int64_extremes) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(INT64_MAX);
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == INT64_MIN && u.hi == INT64_MAX);
}

/* ========== Additional Lattice Intersect Tests ========== */

TEST(intersect_subset) {
    XiRange a = xi_range_make(0, 100);
    XiRange b = xi_range_make(20, 50);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 20 && i.hi == 50);
}

TEST(intersect_superset) {
    XiRange a = xi_range_make(20, 50);
    XiRange b = xi_range_make(0, 100);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 20 && i.hi == 50);
}

TEST(intersect_negative_ranges) {
    XiRange a = xi_range_make(-20, -5);
    XiRange b = xi_range_make(-15, -10);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == -15 && i.hi == -10);
}

TEST(intersect_left_edge_only) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(0, 5);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 0 && i.hi == 5);
}

TEST(intersect_right_edge_only) {
    XiRange a = xi_range_make(5, 10);
    XiRange b = xi_range_make(0, 10);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 5 && i.hi == 10);
}

TEST(intersect_const_inside_range) {
    XiRange a = xi_range_make(0, 100);
    XiRange b = xi_range_const(50);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(xi_range_is_const(i) && i.lo == 50);
}

TEST(intersect_const_outside_range) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_const(20);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.is_bot);
}

TEST(intersect_const_at_lo_edge) {
    XiRange a = xi_range_make(5, 10);
    XiRange b = xi_range_const(5);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(xi_range_is_const(i) && i.lo == 5);
}

TEST(intersect_const_at_hi_edge) {
    XiRange a = xi_range_make(5, 10);
    XiRange b = xi_range_const(10);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(xi_range_is_const(i) && i.lo == 10);
}

TEST(intersect_two_consts_same) {
    XiRange i = xi_range_intersect(xi_range_const(7), xi_range_const(7));
    ASSERT(xi_range_is_const(i) && i.lo == 7);
}

TEST(intersect_two_consts_different) {
    XiRange i = xi_range_intersect(xi_range_const(3), xi_range_const(7));
    ASSERT(i.is_bot);
}

TEST(intersect_bot_bot_is_bot) {
    XiRange i = xi_range_intersect(xi_range_bot(), xi_range_bot());
    ASSERT(i.is_bot);
}

TEST(intersect_top_with_bot) {
    XiRange i = xi_range_intersect(xi_range_top(), xi_range_bot());
    ASSERT(i.is_bot);
}

TEST(intersect_spanning_zero) {
    XiRange a = xi_range_make(-10, 10);
    XiRange b = xi_range_make(-5, 5);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == -5 && i.hi == 5);
}

TEST(intersect_int64_extremes) {
    XiRange a = xi_range_make(INT64_MIN, 0);
    XiRange b = xi_range_make(0, INT64_MAX);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(xi_range_is_const(i) && i.lo == 0);
}

/* ========== Additional Arithmetic: Add ========== */

TEST(add_zero_identity) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == 10 && r.hi == 20);
}

TEST(add_bot_propagates) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_bot();
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_bot);
}

TEST(add_symmetric) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_make(10, 20);
    XiRange r1 = xi_range_add(a, b);
    XiRange r2 = xi_range_add(b, a);
    ASSERT(r1.lo == r2.lo && r1.hi == r2.hi);
}

TEST(add_spanning_zero) {
    XiRange a = xi_range_make(-5, 5);
    XiRange b = xi_range_make(-3, 3);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == -8 && r.hi == 8);
}

TEST(add_same_range) {
    XiRange a = xi_range_make(1, 10);
    XiRange r = xi_range_add(a, a);
    ASSERT(r.lo == 2 && r.hi == 20);
}

TEST(add_const_to_range) {
    XiRange a = xi_range_make(0, 100);
    XiRange b = xi_range_const(5);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == 5 && r.hi == 105);
}

TEST(add_large_no_overflow) {
    XiRange a = xi_range_const(INT64_MAX / 2);
    XiRange b = xi_range_const(INT64_MAX / 2);
    XiRange r = xi_range_add(a, b);
    ASSERT(!r.is_top);
    ASSERT(r.lo == (INT64_MAX / 2) * 2);
}

TEST(add_min_plus_zero) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_add(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MIN);
}

TEST(add_min_plus_max) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(INT64_MAX);
    XiRange r = xi_range_add(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == -1);
}

TEST(add_both_negative) {
    XiRange a = xi_range_make(-100, -50);
    XiRange b = xi_range_make(-30, -10);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.lo == -130 && r.hi == -60);
}

TEST(add_negative_overflow_to_top) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

/* ========== Additional Arithmetic: Sub ========== */

TEST(sub_zero_identity) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == 10 && r.hi == 20);
}

TEST(sub_self_const_is_zero) {
    XiRange a = xi_range_const(42);
    XiRange r = xi_range_sub(a, a);
    ASSERT(xi_range_is_const(r) && r.lo == 0);
}

TEST(sub_top_propagates) {
    XiRange a = xi_range_make(1, 5);
    XiRange b = xi_range_top();
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(sub_spanning_zero) {
    XiRange a = xi_range_make(-5, 5);
    XiRange b = xi_range_make(-3, 3);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == -8 && r.hi == 8);
}

TEST(sub_const_from_range) {
    XiRange a = xi_range_make(10, 50);
    XiRange b = xi_range_const(5);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == 5 && r.hi == 45);
}

TEST(sub_range_from_const) {
    XiRange a = xi_range_const(100);
    XiRange b = xi_range_make(10, 30);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == 70 && r.hi == 90);
}

TEST(sub_negative_from_negative) {
    XiRange a = xi_range_make(-10, -5);
    XiRange b = xi_range_make(-3, -1);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == -9 && r.hi == -2);
}

TEST(sub_max_minus_zero) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(0);
    XiRange r = xi_range_sub(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MAX);
}

TEST(sub_zero_minus_max) {
    XiRange a = xi_range_const(0);
    XiRange b = xi_range_const(INT64_MAX);
    XiRange r = xi_range_sub(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == -INT64_MAX);
}

TEST(sub_positive_overflow_to_top) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(sub_bot_left_propagates) {
    XiRange a = xi_range_bot();
    XiRange b = xi_range_make(1, 5);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_bot);
}

TEST(sub_self_range_centered) {
    XiRange a = xi_range_make(0, 10);
    XiRange r = xi_range_sub(a, a);
    ASSERT(r.lo == -10 && r.hi == 10);
}

/* ========== Additional Arithmetic: Mul ========== */

TEST(mul_by_one_identity) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 10 && r.hi == 20);
}

TEST(mul_by_neg_one) {
    XiRange a = xi_range_make(3, 7);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == -7 && r.hi == -3);
}

TEST(mul_const_const) {
    XiRange a = xi_range_const(6);
    XiRange b = xi_range_const(7);
    XiRange r = xi_range_mul(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == 42);
}

TEST(mul_zero_range_result) {
    XiRange a = xi_range_const(0);
    XiRange b = xi_range_make(INT64_MIN, INT64_MAX);
    XiRange r = xi_range_mul(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == 0);
}

TEST(mul_both_positive) {
    XiRange a = xi_range_make(2, 5);
    XiRange b = xi_range_make(3, 7);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 6 && r.hi == 35);
}

TEST(mul_both_negative_ranges) {
    XiRange a = xi_range_make(-5, -2);
    XiRange b = xi_range_make(-7, -3);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 6 && r.hi == 35);
}

TEST(mul_symmetric) {
    XiRange a = xi_range_make(2, 3);
    XiRange b = xi_range_make(4, 5);
    XiRange r1 = xi_range_mul(a, b);
    XiRange r2 = xi_range_mul(b, a);
    ASSERT(r1.lo == r2.lo && r1.hi == r2.hi);
}

TEST(mul_bot_left_propagates) {
    XiRange a = xi_range_bot();
    XiRange b = xi_range_const(5);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_bot);
}

TEST(mul_bot_right_propagates) {
    XiRange a = xi_range_const(5);
    XiRange b = xi_range_bot();
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_bot);
}

TEST(mul_spanning_zero) {
    XiRange a = xi_range_make(-2, 3);
    XiRange b = xi_range_make(-4, 5);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == -12 && r.hi == 15);
}

TEST(mul_by_two) {
    XiRange a = xi_range_make(5, 10);
    XiRange b = xi_range_const(2);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 10 && r.hi == 20);
}

TEST(mul_neg_by_neg_const) {
    XiRange a = xi_range_make(-10, -5);
    XiRange b = xi_range_const(-2);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.lo == 10 && r.hi == 20);
}

/* ========== Additional Arithmetic: Neg ========== */

TEST(neg_positive_to_negative) {
    XiRange a = xi_range_make(1, 100);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == -100 && r.hi == -1);
}

TEST(neg_negative_to_positive) {
    XiRange a = xi_range_make(-100, -1);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == 1 && r.hi == 100);
}

TEST(neg_zero_is_zero) {
    XiRange r = xi_range_neg(xi_range_const(0));
    ASSERT(xi_range_is_const(r) && r.lo == 0);
}

TEST(neg_int64_max) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == -INT64_MAX);
}

TEST(neg_wide_range) {
    XiRange a = xi_range_make(-50, 100);
    XiRange r = xi_range_neg(a);
    ASSERT(r.lo == -100 && r.hi == 50);
}

TEST(neg_single_negative) {
    XiRange a = xi_range_const(-7);
    XiRange r = xi_range_neg(a);
    ASSERT(xi_range_is_const(r) && r.lo == 7);
}

TEST(neg_double_is_identity) {
    XiRange a = xi_range_make(3, 17);
    XiRange r = xi_range_neg(xi_range_neg(a));
    ASSERT(r.lo == 3 && r.hi == 17);
}

/* ========== Additional Query: is_const ========== */

TEST(is_const_top_false) {
    ASSERT(!xi_range_is_const(xi_range_top()));
}

TEST(is_const_bot_false) {
    ASSERT(!xi_range_is_const(xi_range_bot()));
}

TEST(is_const_wide_range_false) {
    ASSERT(!xi_range_is_const(xi_range_make(0, 1)));
}

TEST(is_const_negative_value) {
    ASSERT(xi_range_is_const(xi_range_const(-999)));
}

/* ========== Additional Query: known_nonneg ========== */

TEST(known_nonneg_const_zero) {
    ASSERT(xi_range_known_nonneg(xi_range_const(0)));
}

TEST(known_nonneg_single_positive) {
    ASSERT(xi_range_known_nonneg(xi_range_const(42)));
}

TEST(known_nonneg_single_negative) {
    ASSERT(!xi_range_known_nonneg(xi_range_const(-1)));
}

TEST(known_nonneg_max_range) {
    ASSERT(xi_range_known_nonneg(xi_range_make(0, INT64_MAX)));
}

TEST(known_nonneg_bot) {
    ASSERT(!xi_range_known_nonneg(xi_range_bot()));
}

TEST(known_nonneg_full_range) {
    ASSERT(!xi_range_known_nonneg(xi_range_make(INT64_MIN, INT64_MAX)));
}

/* ========== Additional Query: known_positive ========== */

TEST(known_positive_large) {
    ASSERT(xi_range_known_positive(xi_range_make(1000, INT64_MAX)));
}

TEST(known_positive_negative_range) {
    ASSERT(!xi_range_known_positive(xi_range_make(-100, -1)));
}

TEST(known_positive_bot) {
    ASSERT(!xi_range_known_positive(xi_range_bot()));
}

TEST(known_positive_spanning) {
    ASSERT(!xi_range_known_positive(xi_range_make(-1, 100)));
}

/* ========== Additional Query: known_less_than ========== */

TEST(known_lt_negative_bound) {
    ASSERT(xi_range_known_less_than(xi_range_make(-100, -50), -49));
    ASSERT(!xi_range_known_less_than(xi_range_make(-100, -50), -50));
}

TEST(known_lt_zero_bound) {
    ASSERT(xi_range_known_less_than(xi_range_make(-10, -1), 0));
    ASSERT(!xi_range_known_less_than(xi_range_make(-10, 0), 0));
}

TEST(known_lt_single_value_pass) {
    ASSERT(xi_range_known_less_than(xi_range_const(5), 6));
}

TEST(known_lt_single_value_fail) {
    ASSERT(!xi_range_known_less_than(xi_range_const(5), 5));
}

TEST(known_lt_bot) {
    ASSERT(!xi_range_known_less_than(xi_range_bot(), 100));
}

/* ========== Additional Query: known_ge ========== */

TEST(known_ge_zero) {
    ASSERT(xi_range_known_ge(xi_range_make(0, 100), 0));
}

TEST(known_ge_negative) {
    ASSERT(xi_range_known_ge(xi_range_make(-5, 10), -5));
    ASSERT(!xi_range_known_ge(xi_range_make(-5, 10), -4));
}

TEST(known_ge_large) {
    ASSERT(xi_range_known_ge(xi_range_make(1000, INT64_MAX), 1000));
}

TEST(known_ge_bot) {
    ASSERT(!xi_range_known_ge(xi_range_bot(), 0));
}

TEST(known_ge_int64_min) {
    ASSERT(xi_range_known_ge(xi_range_make(INT64_MIN, 0), INT64_MIN));
}

/* ========== Additional Query: contains ========== */

TEST(contains_self) {
    XiRange r = xi_range_make(0, 100);
    ASSERT(xi_range_contains(r, r));
}

TEST(contains_const_in_range) {
    ASSERT(xi_range_contains(xi_range_make(0, 100), xi_range_const(50)));
}

TEST(contains_range_not_in_single) {
    ASSERT(!xi_range_contains(xi_range_const(50), xi_range_make(0, 100)));
}

TEST(contains_disjoint_no) {
    ASSERT(!xi_range_contains(xi_range_make(0, 10), xi_range_make(20, 30)));
}

TEST(contains_partial_overlap_no) {
    ASSERT(!xi_range_contains(xi_range_make(0, 10), xi_range_make(5, 15)));
}

TEST(contains_bot_in_anything) {
    ASSERT(xi_range_contains(xi_range_make(-1, 1), xi_range_bot()));
}

TEST(contains_nothing_in_bot) {
    ASSERT(!xi_range_contains(xi_range_bot(), xi_range_make(0, 1)));
}

TEST(contains_bot_in_bot) {
    ASSERT(xi_range_contains(xi_range_bot(), xi_range_bot()));
}

TEST(contains_top_in_top) {
    ASSERT(xi_range_contains(xi_range_top(), xi_range_top()));
}

TEST(contains_at_boundary) {
    ASSERT(xi_range_contains(xi_range_make(5, 10), xi_range_make(5, 10)));
    ASSERT(!xi_range_contains(xi_range_make(5, 10), xi_range_make(4, 10)));
    ASSERT(!xi_range_contains(xi_range_make(5, 10), xi_range_make(5, 11)));
}

/* ========== Arithmetic + Lattice Combined ========== */

TEST(add_then_sub_roundtrip_const) {
    XiRange a = xi_range_const(10);
    XiRange b = xi_range_const(5);
    XiRange sum = xi_range_add(a, b);
    XiRange diff = xi_range_sub(sum, b);
    ASSERT(xi_range_is_const(diff) && diff.lo == 10);
}

TEST(sub_then_add_roundtrip_const) {
    XiRange a = xi_range_const(20);
    XiRange b = xi_range_const(7);
    XiRange diff = xi_range_sub(a, b);
    XiRange sum = xi_range_add(diff, b);
    ASSERT(xi_range_is_const(sum) && sum.lo == 20);
}

TEST(add_preserves_width_const) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_const(100);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.hi - r.lo == 10);
}

TEST(sub_preserves_width_const) {
    XiRange a = xi_range_make(100, 200);
    XiRange b = xi_range_const(50);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.lo == 50 && r.hi == 150);
    ASSERT(r.hi - r.lo == 100);
}

TEST(mul_by_one_preserves_range) {
    XiRange a = xi_range_make(-10, 10);
    XiRange r = xi_range_mul(a, xi_range_const(1));
    ASSERT(r.lo == -10 && r.hi == 10);
}

TEST(neg_add_eq_sub) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_make(3, 7);
    XiRange neg_b = xi_range_neg(b);
    XiRange add_neg = xi_range_add(a, neg_b);
    XiRange sub_direct = xi_range_sub(a, b);
    ASSERT(add_neg.lo == sub_direct.lo && add_neg.hi == sub_direct.hi);
}

TEST(union_of_add_results) {
    XiRange a = xi_range_add(xi_range_const(1), xi_range_const(2));
    XiRange b = xi_range_add(xi_range_const(10), xi_range_const(20));
    XiRange u = xi_range_union(a, b);
    ASSERT(u.lo == 3 && u.hi == 30);
}

TEST(intersect_of_computed_ranges) {
    XiRange a = xi_range_add(xi_range_make(0, 10), xi_range_const(5));
    XiRange b = xi_range_make(8, 20);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(i.lo == 8 && i.hi == 15);
}

/* ========== Analysis Pass: More Tests ========== */

TEST(analyze_const_sub) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 50, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *sub = xi_value_new(f, blk, XI_SUB, &stub_int, 2);
    sub->args[0] = c1;
    sub->args[1] = c2;

    xi_range_analyze(f);

    XiRange r = xi_range_of(sub);
    ASSERT(!r.is_top && !r.is_bot);
    ASSERT(r.lo == 30 && r.hi == 30);

    xi_func_free(f);
}

TEST(analyze_const_mul) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 6, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 7, &stub_int);
    XiValue *mul = xi_value_new(f, blk, XI_MUL, &stub_int, 2);
    mul->args[0] = c1;
    mul->args[1] = c2;

    xi_range_analyze(f);

    XiRange r = xi_range_of(mul);
    ASSERT(xi_range_is_const(r) && r.lo == 42);

    xi_func_free(f);
}

TEST(analyze_const_neg) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 10, &stub_int);
    XiValue *neg = xi_value_new(f, blk, XI_NEG, &stub_int, 1);
    neg->args[0] = c;

    xi_range_analyze(f);

    XiRange r = xi_range_of(neg);
    ASSERT(xi_range_is_const(r) && r.lo == -10);

    xi_func_free(f);
}

TEST(analyze_chain_add_add) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 30, &stub_int);

    XiValue *add1 = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add1->args[0] = c1;
    add1->args[1] = c2;

    XiValue *add2 = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add2->args[0] = add1;
    add2->args[1] = c3;

    xi_range_analyze(f);

    XiRange r = xi_range_of(add2);
    ASSERT(xi_range_is_const(r) && r.lo == 60);

    xi_func_free(f);
}

TEST(analyze_chain_add_sub) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 100, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 30, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 25, &stub_int);

    XiValue *add = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add->args[0] = c1;
    add->args[1] = c2;

    XiValue *sub = xi_value_new(f, blk, XI_SUB, &stub_int, 2);
    sub->args[0] = add;
    sub->args[1] = c3;

    xi_range_analyze(f);

    XiRange r = xi_range_of(sub);
    ASSERT(xi_range_is_const(r) && r.lo == 105);

    xi_func_free(f);
}

TEST(analyze_chain_mul_add) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);
    XiValue *c4 = xi_const_int(f, blk, 4, &stub_int);

    XiValue *mul = xi_value_new(f, blk, XI_MUL, &stub_int, 2);
    mul->args[0] = c2;
    mul->args[1] = c3;

    XiValue *add = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add->args[0] = mul;
    add->args[1] = c4;

    xi_range_analyze(f);

    XiRange r = xi_range_of(add);
    ASSERT(xi_range_is_const(r) && r.lo == 10);

    xi_func_free(f);
}

TEST(analyze_const_zero) {
    XiFunc *f = make_func();
    XiValue *c = xi_const_int(f, f->entry, 0, &stub_int);

    xi_range_analyze(f);

    XiRange r = xi_range_of(c);
    ASSERT(xi_range_is_const(r) && r.lo == 0);

    xi_func_free(f);
}

TEST(analyze_const_negative) {
    XiFunc *f = make_func();
    XiValue *c = xi_const_int(f, f->entry, -99, &stub_int);

    xi_range_analyze(f);

    XiRange r = xi_range_of(c);
    ASSERT(xi_range_is_const(r) && r.lo == -99);

    xi_func_free(f);
}

TEST(analyze_const_int64_max) {
    XiFunc *f = make_func();
    XiValue *c = xi_const_int(f, f->entry, INT64_MAX, &stub_int);

    xi_range_analyze(f);

    XiRange r = xi_range_of(c);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MAX);

    xi_func_free(f);
}

TEST(analyze_const_int64_min) {
    XiFunc *f = make_func();
    XiValue *c = xi_const_int(f, f->entry, INT64_MIN, &stub_int);

    xi_range_analyze(f);

    XiRange r = xi_range_of(c);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MIN);

    xi_func_free(f);
}

TEST(analyze_mul_by_zero) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c0 = xi_const_int(f, blk, 0, &stub_int);
    XiValue *c99 = xi_const_int(f, blk, 99, &stub_int);
    XiValue *mul = xi_value_new(f, blk, XI_MUL, &stub_int, 2);
    mul->args[0] = c99;
    mul->args[1] = c0;

    xi_range_analyze(f);

    XiRange r = xi_range_of(mul);
    ASSERT(xi_range_is_const(r) && r.lo == 0);

    xi_func_free(f);
}

TEST(analyze_neg_neg_is_identity) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 42, &stub_int);
    XiValue *neg1 = xi_value_new(f, blk, XI_NEG, &stub_int, 1);
    neg1->args[0] = c;
    XiValue *neg2 = xi_value_new(f, blk, XI_NEG, &stub_int, 1);
    neg2->args[0] = neg1;

    xi_range_analyze(f);

    XiRange r = xi_range_of(neg2);
    ASSERT(xi_range_is_const(r) && r.lo == 42);

    xi_func_free(f);
}

TEST(analyze_sub_self_is_zero) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 77, &stub_int);
    XiValue *sub = xi_value_new(f, blk, XI_SUB, &stub_int, 2);
    sub->args[0] = c;
    sub->args[1] = c;

    xi_range_analyze(f);

    XiRange r = xi_range_of(sub);
    ASSERT(xi_range_is_const(r) && r.lo == 0);

    xi_func_free(f);
}

TEST(analyze_reanalyze_stable) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 5, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *add = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add->args[0] = c1;
    add->args[1] = c2;

    xi_range_analyze(f);
    XiRange r1 = xi_range_of(add);

    f->invariant_mask &= ~XI_INV_RANGE_ANNOTATED;
    xi_range_analyze(f);
    XiRange r2 = xi_range_of(add);

    ASSERT(r1.lo == r2.lo && r1.hi == r2.hi);
    ASSERT(r2.lo == 15);

    xi_func_free(f);
}

TEST(analyze_multiple_independent_ops) {
    XiFunc *f = make_func();
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 10, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 20, &stub_int);
    XiValue *c3 = xi_const_int(f, blk, 3, &stub_int);

    XiValue *add = xi_value_new(f, blk, XI_ADD, &stub_int, 2);
    add->args[0] = c1;
    add->args[1] = c2;

    XiValue *mul = xi_value_new(f, blk, XI_MUL, &stub_int, 2);
    mul->args[0] = c1;
    mul->args[1] = c3;

    xi_range_analyze(f);

    XiRange ra = xi_range_of(add);
    XiRange rm = xi_range_of(mul);
    ASSERT(xi_range_is_const(ra) && ra.lo == 30);
    ASSERT(xi_range_is_const(rm) && rm.lo == 30);

    xi_func_free(f);
}

/* ========== SCCP: More Branch Folding Tests ========== */

TEST(sccp_eq_same_const_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_EQ, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_ne_same_const_false) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_NE, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == else_blk);

    xi_func_free(f);
}

TEST(sccp_le_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *c7 = xi_const_int(f, entry, 7, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_LE, &stub_int, 2);
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

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_le_equal_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_LE, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_gt_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GT, &stub_int, 2);
    cmp->args[0] = c10;
    cmp->args[1] = c3;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c10;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c3;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_gt_false) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GT, &stub_int, 2);
    cmp->args[0] = c3;
    cmp->args[1] = c10;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c3;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c10;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == else_blk);

    xi_func_free(f);
}

TEST(sccp_ne_different_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c1 = xi_const_int(f, entry, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, entry, 2, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_NE, &stub_int, 2);
    cmp->args[0] = c1;
    cmp->args[1] = c2;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c1;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c2;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_ge_equal_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GE, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_ge_greater_true) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GE, &stub_int, 2);
    cmp->args[0] = c10;
    cmp->args[1] = c3;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c10;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c3;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_lt_false_when_equal) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_LT, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == else_blk);

    xi_func_free(f);
}

TEST(sccp_gt_false_when_equal) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c5a = xi_const_int(f, entry, 5, &stub_int);
    XiValue *c5b = xi_const_int(f, entry, 5, &stub_int);
    XiValue *cmp = xi_value_new(f, entry, XI_GT, &stub_int, 2);
    cmp->args[0] = c5a;
    cmp->args[1] = c5b;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = c5a;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c5b;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == else_blk);

    xi_func_free(f);
}

TEST(sccp_with_add_result_lt) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c2 = xi_const_int(f, entry, 2, &stub_int);
    XiValue *c3 = xi_const_int(f, entry, 3, &stub_int);
    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *add = xi_value_new(f, entry, XI_ADD, &stub_int, 2);
    add->args[0] = c2;
    add->args[1] = c3;

    XiValue *cmp = xi_value_new(f, entry, XI_LT, &stub_int, 2);
    cmp->args[0] = add;
    cmp->args[1] = c10;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = add;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c10;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_with_mul_result_eq) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c6 = xi_const_int(f, entry, 6, &stub_int);
    XiValue *c7 = xi_const_int(f, entry, 7, &stub_int);
    XiValue *c42 = xi_const_int(f, entry, 42, &stub_int);
    XiValue *mul = xi_value_new(f, entry, XI_MUL, &stub_int, 2);
    mul->args[0] = c6;
    mul->args[1] = c7;

    XiValue *cmp = xi_value_new(f, entry, XI_EQ, &stub_int, 2);
    cmp->args[0] = mul;
    cmp->args[1] = c42;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = mul;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c42;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

TEST(sccp_with_neg_comparison) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    entry->sealed = true;
    XiBlock *then_blk = xi_block_new(f);
    then_blk->sealed = true;
    XiBlock *else_blk = xi_block_new(f);
    else_blk->sealed = true;

    XiValue *c10 = xi_const_int(f, entry, 10, &stub_int);
    XiValue *neg = xi_value_new(f, entry, XI_NEG, &stub_int, 1);
    neg->args[0] = c10;
    XiValue *c0 = xi_const_int(f, entry, 0, &stub_int);

    XiValue *cmp = xi_value_new(f, entry, XI_LT, &stub_int, 2);
    cmp->args[0] = neg;
    cmp->args[1] = c0;

    entry->kind = XI_BLOCK_IF;
    entry->control = cmp;
    entry->succs[0] = then_blk;
    entry->succs[1] = else_blk;
    xi_block_add_pred(then_blk, entry);
    xi_block_add_pred(else_blk, entry);

    then_blk->kind = XI_BLOCK_RETURN;
    then_blk->control = neg;
    else_blk->kind = XI_BLOCK_RETURN;
    else_blk->control = c0;

    xi_opt_sccp(f);

    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->succs[0] == then_blk);

    xi_func_free(f);
}

/* ========== INT64 Boundary Stress Tests ========== */

TEST(add_max_max_overflows) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(INT64_MAX);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

TEST(add_min_min_overflows) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(INT64_MIN);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

TEST(sub_min_max_overflows) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(INT64_MAX);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(sub_max_min_overflows) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(INT64_MIN);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(mul_max_two_overflows) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(2);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_top);
}

TEST(mul_min_two_overflows) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(2);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_top);
}

TEST(mul_min_neg_one_overflows) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_mul(a, b);
    ASSERT(r.is_top);
}

TEST(neg_int64_min_const_to_top) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange r = xi_range_neg(a);
    ASSERT(r.is_top);
}

TEST(add_max_one_overflows) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_add(a, b);
    ASSERT(r.is_top);
}

TEST(sub_min_one_overflows) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.is_top);
}

TEST(mul_max_by_one_ok) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_mul(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MAX);
}

TEST(mul_min_by_one_ok) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(1);
    XiRange r = xi_range_mul(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MIN);
}

TEST(add_max_minus_one_ok) {
    XiRange a = xi_range_const(INT64_MAX);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_add(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MAX - 1);
}

TEST(sub_min_neg_one_ok) {
    XiRange a = xi_range_const(INT64_MIN);
    XiRange b = xi_range_const(-1);
    XiRange r = xi_range_sub(a, b);
    ASSERT(xi_range_is_const(r) && r.lo == INT64_MIN + 1);
}

/* ========== Range Width / Span Tests ========== */

TEST(width_const_is_one) {
    XiRange r = xi_range_const(42);
    ASSERT(r.hi - r.lo == 0);
}

TEST(width_two_element) {
    XiRange r = xi_range_make(5, 6);
    ASSERT(r.hi - r.lo == 1);
}

TEST(width_hundred) {
    XiRange r = xi_range_make(0, 99);
    ASSERT(r.hi - r.lo == 99);
}

TEST(add_widens_range) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(0, 5);
    XiRange r = xi_range_add(a, b);
    int64_t wa = 10 - 0;
    int64_t wb = 5 - 0;
    int64_t wr = r.hi - r.lo;
    ASSERT(wr == wa + wb);
}

TEST(sub_widens_range) {
    XiRange a = xi_range_make(10, 20);
    XiRange b = xi_range_make(1, 3);
    XiRange r = xi_range_sub(a, b);
    ASSERT(r.hi - r.lo == (20 - 10) + (3 - 1));
}

/* ========== Lattice Monotonicity Tests ========== */

TEST(union_monotone_wrt_contains) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 15);
    XiRange u = xi_range_union(a, b);
    ASSERT(xi_range_contains(u, a));
    ASSERT(xi_range_contains(u, b));
}

TEST(intersect_monotone_wrt_contains) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 15);
    XiRange i = xi_range_intersect(a, b);
    ASSERT(xi_range_contains(a, i));
    ASSERT(xi_range_contains(b, i));
}

TEST(union_idempotent) {
    XiRange a = xi_range_make(3, 17);
    XiRange u1 = xi_range_union(a, a);
    XiRange u2 = xi_range_union(u1, a);
    ASSERT(u1.lo == u2.lo && u1.hi == u2.hi);
}

TEST(intersect_idempotent) {
    XiRange a = xi_range_make(3, 17);
    XiRange i1 = xi_range_intersect(a, a);
    XiRange i2 = xi_range_intersect(i1, a);
    ASSERT(i1.lo == i2.lo && i1.hi == i2.hi);
}

TEST(union_absorbs_intersection) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 15);
    XiRange i = xi_range_intersect(a, b);
    XiRange u = xi_range_union(a, i);
    ASSERT(u.lo == a.lo && u.hi == a.hi);
}

TEST(intersect_absorbs_union) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_make(5, 15);
    XiRange u = xi_range_union(a, b);
    XiRange i = xi_range_intersect(a, u);
    ASSERT(i.lo == a.lo && i.hi == a.hi);
}

/* ========== Query Combinations ========== */

TEST(known_nonneg_after_add) {
    XiRange r = xi_range_add(xi_range_make(0, 10), xi_range_make(0, 5));
    ASSERT(xi_range_known_nonneg(r));
}

TEST(known_positive_after_add) {
    XiRange r = xi_range_add(xi_range_make(1, 10), xi_range_make(1, 5));
    ASSERT(xi_range_known_positive(r));
}

TEST(known_lt_after_sub) {
    XiRange a = xi_range_make(0, 10);
    XiRange b = xi_range_const(5);
    XiRange r = xi_range_sub(a, b);
    ASSERT(xi_range_known_less_than(r, 6));
}

TEST(known_ge_after_add) {
    XiRange r = xi_range_add(xi_range_make(0, 10), xi_range_const(100));
    ASSERT(xi_range_known_ge(r, 100));
}

TEST(known_nonneg_after_mul_pos) {
    XiRange r = xi_range_mul(xi_range_make(0, 5), xi_range_make(0, 3));
    ASSERT(xi_range_known_nonneg(r));
}

TEST(contains_after_union) {
    XiRange a = xi_range_make(0, 5);
    XiRange b = xi_range_make(10, 15);
    XiRange u = xi_range_union(a, b);
    ASSERT(xi_range_contains(u, xi_range_const(3)));
    ASSERT(xi_range_contains(u, xi_range_const(12)));
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

    /* Constructor edge cases */
    run_range_const_zero();
    run_range_const_negative();
    run_range_const_int64_min();
    run_range_const_int64_max();
    run_range_make_full();
    run_range_make_single_element();
    run_range_make_two_elements();
    run_range_make_negative_span();

    /* Additional union tests */
    run_union_const_with_range();
    run_union_negative_ranges();
    run_union_disjoint_far();
    run_union_inner_contained();
    run_union_bot_with_top();
    run_union_top_with_bot();
    run_union_negative_to_positive();
    run_union_wide_covers_narrow();
    run_union_const_const_same();
    run_union_const_const_different();
    run_union_extends_left();
    run_union_extends_right();
    run_union_int64_extremes();

    /* Additional intersect tests */
    run_intersect_subset();
    run_intersect_superset();
    run_intersect_negative_ranges();
    run_intersect_left_edge_only();
    run_intersect_right_edge_only();
    run_intersect_const_inside_range();
    run_intersect_const_outside_range();
    run_intersect_const_at_lo_edge();
    run_intersect_const_at_hi_edge();
    run_intersect_two_consts_same();
    run_intersect_two_consts_different();
    run_intersect_bot_bot_is_bot();
    run_intersect_top_with_bot();
    run_intersect_spanning_zero();
    run_intersect_int64_extremes();

    /* Additional add tests */
    run_add_zero_identity();
    run_add_bot_propagates();
    run_add_symmetric();
    run_add_spanning_zero();
    run_add_same_range();
    run_add_const_to_range();
    run_add_large_no_overflow();
    run_add_min_plus_zero();
    run_add_min_plus_max();
    run_add_both_negative();
    run_add_negative_overflow_to_top();

    /* Additional sub tests */
    run_sub_zero_identity();
    run_sub_self_const_is_zero();
    run_sub_top_propagates();
    run_sub_spanning_zero();
    run_sub_const_from_range();
    run_sub_range_from_const();
    run_sub_negative_from_negative();
    run_sub_max_minus_zero();
    run_sub_zero_minus_max();
    run_sub_positive_overflow_to_top();
    run_sub_bot_left_propagates();
    run_sub_self_range_centered();

    /* Additional mul tests */
    run_mul_by_one_identity();
    run_mul_by_neg_one();
    run_mul_const_const();
    run_mul_zero_range_result();
    run_mul_both_positive();
    run_mul_both_negative_ranges();
    run_mul_symmetric();
    run_mul_bot_left_propagates();
    run_mul_bot_right_propagates();
    run_mul_spanning_zero();
    run_mul_by_two();
    run_mul_neg_by_neg_const();

    /* Additional neg tests */
    run_neg_positive_to_negative();
    run_neg_negative_to_positive();
    run_neg_zero_is_zero();
    run_neg_int64_max();
    run_neg_wide_range();
    run_neg_single_negative();
    run_neg_double_is_identity();

    /* Additional is_const tests */
    run_is_const_top_false();
    run_is_const_bot_false();
    run_is_const_wide_range_false();
    run_is_const_negative_value();

    /* Additional known_nonneg tests */
    run_known_nonneg_const_zero();
    run_known_nonneg_single_positive();
    run_known_nonneg_single_negative();
    run_known_nonneg_max_range();
    run_known_nonneg_bot();
    run_known_nonneg_full_range();

    /* Additional known_positive tests */
    run_known_positive_large();
    run_known_positive_negative_range();
    run_known_positive_bot();
    run_known_positive_spanning();

    /* Additional known_less_than tests */
    run_known_lt_negative_bound();
    run_known_lt_zero_bound();
    run_known_lt_single_value_pass();
    run_known_lt_single_value_fail();
    run_known_lt_bot();

    /* Additional known_ge tests */
    run_known_ge_zero();
    run_known_ge_negative();
    run_known_ge_large();
    run_known_ge_bot();
    run_known_ge_int64_min();

    /* Additional contains tests */
    run_contains_self();
    run_contains_const_in_range();
    run_contains_range_not_in_single();
    run_contains_disjoint_no();
    run_contains_partial_overlap_no();
    run_contains_bot_in_anything();
    run_contains_nothing_in_bot();
    run_contains_bot_in_bot();
    run_contains_top_in_top();
    run_contains_at_boundary();

    /* Arithmetic + lattice combined */
    run_add_then_sub_roundtrip_const();
    run_sub_then_add_roundtrip_const();
    run_add_preserves_width_const();
    run_sub_preserves_width_const();
    run_mul_by_one_preserves_range();
    run_neg_add_eq_sub();
    run_union_of_add_results();
    run_intersect_of_computed_ranges();

    /* Analysis pass: more tests */
    run_analyze_const_sub();
    run_analyze_const_mul();
    run_analyze_const_neg();
    run_analyze_chain_add_add();
    run_analyze_chain_add_sub();
    run_analyze_chain_mul_add();
    run_analyze_const_zero();
    run_analyze_const_negative();
    run_analyze_const_int64_max();
    run_analyze_const_int64_min();
    run_analyze_mul_by_zero();
    run_analyze_neg_neg_is_identity();
    run_analyze_sub_self_is_zero();
    run_analyze_reanalyze_stable();
    run_analyze_multiple_independent_ops();

    /* SCCP: more branch folding */
    run_sccp_eq_same_const_true();
    run_sccp_ne_same_const_false();
    run_sccp_le_true();
    run_sccp_le_equal_true();
    run_sccp_gt_true();
    run_sccp_gt_false();
    run_sccp_ne_different_true();
    run_sccp_ge_equal_true();
    run_sccp_ge_greater_true();
    run_sccp_lt_false_when_equal();
    run_sccp_gt_false_when_equal();
    run_sccp_with_add_result_lt();
    run_sccp_with_mul_result_eq();
    run_sccp_with_neg_comparison();

    /* INT64 boundary stress */
    run_add_max_max_overflows();
    run_add_min_min_overflows();
    run_sub_min_max_overflows();
    run_sub_max_min_overflows();
    run_mul_max_two_overflows();
    run_mul_min_two_overflows();
    run_mul_min_neg_one_overflows();
    run_neg_int64_min_const_to_top();
    run_add_max_one_overflows();
    run_sub_min_one_overflows();
    run_mul_max_by_one_ok();
    run_mul_min_by_one_ok();
    run_add_max_minus_one_ok();
    run_sub_min_neg_one_ok();

    /* Range width tests */
    run_width_const_is_one();
    run_width_two_element();
    run_width_hundred();
    run_add_widens_range();
    run_sub_widens_range();

    /* Lattice monotonicity */
    run_union_monotone_wrt_contains();
    run_intersect_monotone_wrt_contains();
    run_union_idempotent();
    run_intersect_idempotent();
    run_union_absorbs_intersection();
    run_intersect_absorbs_union();

    /* Query combinations */
    run_known_nonneg_after_add();
    run_known_positive_after_add();
    run_known_lt_after_sub();
    run_known_ge_after_add();
    run_known_nonneg_after_mul_pos();
    run_contains_after_union();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
