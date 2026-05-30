/*
 * Unit tests for xi_guard_cost — deoptimization cost model.
 */

#include "../../../src/ir/xi_guard_cost.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ic.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

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

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_guard_cost", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

TEST(fill_null_safe) {
    XiPassChange c = xi_guard_cost_fill(NULL);
    ASSERT(!c.values_changed);
}

TEST(fill_sets_invariant) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *c = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    c->aux_int = 1;
    xi_block_set_return(entry, c);

    ASSERT(!(f->invariant_mask & XI_INV_GUARD_COST));
    xi_guard_cost_fill(f);
    ASSERT(f->invariant_mask & XI_INV_GUARD_COST);

    xi_func_free(f);
}

TEST(penalty_for_non_guard_is_zero) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *c = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    c->aux_int = 42;
    xi_block_set_return(entry, c);

    float p = xi_guard_expected_penalty(c);
    ASSERT(p == 0.0f);

    xi_func_free(f);
}

TEST(penalty_for_null_is_zero) {
    float p = xi_guard_expected_penalty(NULL);
    ASSERT(p == 0.0f);
}

TEST(compute_penalty_basic) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    xi_block_set_return(entry, guard);

    float p = xi_guard_compute_penalty(f, entry, 1, guard);
    ASSERT(p >= 0.0f);

    xi_func_free(f);
}

TEST(compute_penalty_null_safe) {
    float p = xi_guard_compute_penalty(NULL, NULL, 0, NULL);
    ASSERT(p == 0.0f);
}

TEST(higher_miss_rate_higher_penalty) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    for (int i = 0; i < 10; i++) {
        xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    }

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;
    xi_block_set_return(entry, guard);

    float p = xi_guard_compute_penalty(f, entry, 12, guard);
    ASSERT(p > 0.0f);

    xi_func_free(f);
}

TEST(should_speculate_low_cost) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;
    xi_block_set_return(entry, guard);

    bool should = xi_guard_should_speculate(f, entry, 1, guard);
    ASSERT(should);

    xi_func_free(f);
}

TEST(should_speculate_null_safe) {
    bool should = xi_guard_should_speculate(NULL, NULL, 0, NULL);
    ASSERT(!should);
}

int main(void) {
    printf("=== Xi Guard Cost Tests ===\n\n");

    run_fill_null_safe();
    run_fill_sets_invariant();
    run_penalty_for_non_guard_is_zero();
    run_penalty_for_null_is_zero();
    run_compute_penalty_basic();
    run_compute_penalty_null_safe();
    run_higher_miss_rate_higher_penalty();
    run_should_speculate_low_cost();
    run_should_speculate_null_safe();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
