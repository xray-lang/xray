/*
 * Unit tests for Xi range analysis (xi_range.h / xi_range.c).
 * Covers lattice operations, arithmetic transfer functions, queries,
 * and the analysis pass on synthetic IR.
 */

#include "../../../src/ir/xi_range.h"
#include "../../../src/ir/xi.h"
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

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
