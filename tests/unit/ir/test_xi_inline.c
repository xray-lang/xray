/*
 * Unit tests for Xi inline cost model (xi_inline_benefit).
 * Covers benefit scoring under various callee and call-site scenarios.
 */

#include "../../../src/ir/xi_opt_inline.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

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

/* ========== Test: small callee, positive benefit ========== */

TEST(small_callee_positive) {
    XiInlineCostModel cost = {
        .value_count = 5,
        .call_count = 0,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = false,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30, cost=5, score=25 */
    ASSERT(b == 25);
}

/* ========== Test: large callee, negative benefit ========== */

TEST(large_callee_negative) {
    XiInlineCostModel cost = {
        .value_count = 50,
        .call_count = 2,
        .branch_count = 3,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = false,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-50=-20, -2*3=-6, -3*2=-6 => -32 */
    ASSERT(b < 0);
}

/* ========== Test: const args boost ========== */

TEST(const_args_boost) {
    XiInlineCostModel cost = {
        .value_count = 35,
        .call_count = 0,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = true,
        .single_call_site = false,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-35=-5, +15(const) = 10 */
    ASSERT(b == 10);
}

/* ========== Test: single call site boost ========== */

TEST(single_call_site_boost) {
    XiInlineCostModel cost = {
        .value_count = 38,
        .call_count = 0,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = true,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-38=-8, +10(single) = 2 */
    ASSERT(b == 2);
}

/* ========== Test: loop penalty ========== */

TEST(loop_penalty) {
    XiInlineCostModel cost = {
        .value_count = 10,
        .call_count = 0,
        .branch_count = 1,
        .has_loop = true,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = false,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-10=20, -1*2=-2(branch), -20(loop) => -2 */
    ASSERT(b == -2);
}

/* ========== Test: self-recursive always negative ========== */

TEST(self_recursive_never_inline) {
    XiInlineCostModel cost = {
        .value_count = 3,
        .call_count = 1,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = true,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = true,
        .single_call_site = true,
        .caller_size = 10,
    };
    int b = xi_inline_benefit(&cost, &site);
    ASSERT(b == -1000);
}

/* ========== Test: large caller penalty ========== */

TEST(large_caller_penalty) {
    XiInlineCostModel cost = {
        .value_count = 25,
        .call_count = 0,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = false,
        .caller_size = 400,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-25=5, -15(large caller) = -10 */
    ASSERT(b == -10);
}

/* ========== Test: throw penalty ========== */

TEST(throw_penalty) {
    XiInlineCostModel cost = {
        .value_count = 28,
        .call_count = 0,
        .branch_count = 0,
        .has_loop = false,
        .calls_self = false,
        .has_throw = true,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = false,
        .single_call_site = false,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-28=2, -5(throw) = -3 */
    ASSERT(b == -3);
}

/* ========== Test: combined bonuses overcome borderline ========== */

TEST(combined_bonuses) {
    XiInlineCostModel cost = {
        .value_count = 45,
        .call_count = 1,
        .branch_count = 1,
        .has_loop = false,
        .calls_self = false,
        .has_throw = false,
    };
    XiInlineCallSiteInfo site = {
        .all_args_const = true,
        .single_call_site = true,
        .caller_size = 50,
    };
    int b = xi_inline_benefit(&cost, &site);
    /* base=30-45=-15, +15(const)+10(single)=+25, -1*3(call)-1*2(branch)=-5 => 5 */
    ASSERT(b == 5);
}

/* ========== Test: budget scales with caller size ========== */

TEST(budget_small_caller) {
    /* < 100 → aggressive: default + 2 */
    ASSERT(xi_inline_budget(0) == XI_INLINE_MAX_PER_PASS + 2);
    ASSERT(xi_inline_budget(50) == XI_INLINE_MAX_PER_PASS + 2);
    ASSERT(xi_inline_budget(99) == XI_INLINE_MAX_PER_PASS + 2);
}

TEST(budget_medium_caller) {
    /* 100..300 → default */
    ASSERT(xi_inline_budget(100) == XI_INLINE_MAX_PER_PASS);
    ASSERT(xi_inline_budget(200) == XI_INLINE_MAX_PER_PASS);
    ASSERT(xi_inline_budget(300) == XI_INLINE_MAX_PER_PASS);
}

TEST(budget_large_caller) {
    /* > 300 → conservative: default - 2 */
    ASSERT(xi_inline_budget(301) == XI_INLINE_MAX_PER_PASS - 2);
    ASSERT(xi_inline_budget(1000) == XI_INLINE_MAX_PER_PASS - 2);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Inline Cost Model Tests ===\n\n");

    run_small_callee_positive();
    run_large_callee_negative();
    run_const_args_boost();
    run_single_call_site_boost();
    run_loop_penalty();
    run_self_recursive_never_inline();
    run_large_caller_penalty();
    run_throw_penalty();
    run_combined_bonuses();
    run_budget_small_caller();
    run_budget_medium_caller();
    run_budget_large_caller();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
