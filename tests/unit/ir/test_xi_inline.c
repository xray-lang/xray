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

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 3, .frozen = true};

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

static XiFunc *make_func(const char *name, XrType *ret_type) {
    XiFunc *f = xi_func_new(name, ret_type);
    if (!f)
        return NULL;
    XiBlock *entry = xi_block_new(f);
    if (!entry) {
        xi_func_free(f);
        return NULL;
    }
    entry->sealed = true;
    return f;
}

static XiValue *make_multi_ret_value(XiFunc *f, XiBlock *blk, uint16_t nret, int64_t base) {
    XiValue *vals[4];
    if (nret > 4)
        return NULL;
    for (uint16_t i = 0; i < nret; i++) {
        vals[i] = xi_const_int(f, blk, base + i, &stub_int);
        if (!vals[i])
            return NULL;
    }
    XiValue *ret = xi_value_new(f, blk, XI_MULTI_RET, &stub_int, nret);
    if (!ret)
        return NULL;
    for (uint16_t i = 0; i < nret; i++)
        ret->args[i] = vals[i];
    return ret;
}

static XiFunc *make_multi_ret_callee(uint16_t nret) {
    XiFunc *callee = make_func("multi_ret_callee", &stub_int);
    if (!callee)
        return NULL;
    XiValue *ret = make_multi_ret_value(callee, callee->entry, nret, 10);
    if (!ret) {
        xi_func_free(callee);
        return NULL;
    }
    xi_block_set_return(callee->entry, ret);
    return callee;
}

static XiFunc *make_branch_multi_ret_callee(void) {
    XiFunc *callee = make_func("branch_multi_ret_callee", &stub_int);
    if (!callee)
        return NULL;
    XiBlock *entry = callee->entry;
    XiBlock *then_blk = xi_block_new(callee);
    XiBlock *else_blk = xi_block_new(callee);
    if (!then_blk || !else_blk) {
        xi_func_free(callee);
        return NULL;
    }
    XiValue *cond = xi_const_bool(callee, entry, true, &stub_bool);
    if (!cond) {
        xi_func_free(callee);
        return NULL;
    }
    xi_block_set_if(entry, cond, then_blk, else_blk);

    XiValue *then_ret = make_multi_ret_value(callee, then_blk, 2, 10);
    XiValue *else_ret = make_multi_ret_value(callee, else_blk, 2, 20);
    if (!then_ret || !else_ret) {
        xi_func_free(callee);
        return NULL;
    }
    xi_block_set_return(then_blk, then_ret);
    xi_block_set_return(else_blk, else_ret);
    return callee;
}

static XiValue *add_known_call(XiFunc *caller, XiFunc *callee) {
    XiBlock *entry = caller->entry;
    XiValue *closure = xi_value_new(caller, entry, XI_CLOSURE_NEW, &stub_func, 0);
    if (!closure)
        return NULL;
    closure->aux = callee;
    XiValue *call = xi_value_new(caller, entry, XI_CALL, &stub_int, 1);
    if (!call)
        return NULL;
    call->args[0] = closure;
    return call;
}

static bool run_inline_multi_ret_arity(uint16_t nret) {
    XiFunc *callee = make_multi_ret_callee(nret);
    XiFunc *caller = make_func("multi_ret_caller", &stub_int);
    if (!callee || !caller) {
        xi_func_free(caller);
        xi_func_free(callee);
        return false;
    }

    XiValue *call = add_known_call(caller, callee);
    XiValue *extracts[4];
    if (!call || nret > 4) {
        xi_func_free(caller);
        xi_func_free(callee);
        return false;
    }
    for (uint16_t i = 0; i < nret; i++) {
        extracts[i] = xi_value_new(caller, caller->entry, XI_EXTRACT, &stub_int, 1);
        if (!extracts[i]) {
            xi_func_free(caller);
            xi_func_free(callee);
            return false;
        }
        extracts[i]->args[0] = call;
        extracts[i]->aux_int = i;
    }
    xi_block_set_return(caller->entry, extracts[nret - 1]);

    XiPassChange chg = xi_opt_inline(caller);
    bool ok = chg.cfg_changed && chg.values_changed;
    for (uint16_t i = 0; ok && i < nret; i++) {
        ok = extracts[i]->op == XI_COPY && extracts[i]->nargs == 1 && extracts[i]->args[0] &&
             extracts[i]->args[0]->op == XI_CONST && extracts[i]->args[0]->aux_int == 10 + i;
    }

    xi_func_free(caller);
    xi_func_free(callee);
    return ok;
}

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

TEST(inlines_multi_return_two_values) {
    ASSERT(run_inline_multi_ret_arity(2));
}

TEST(inlines_multi_return_three_values) {
    ASSERT(run_inline_multi_ret_arity(3));
}

TEST(inlines_multi_return_four_values) {
    ASSERT(run_inline_multi_ret_arity(4));
}

TEST(inlines_multi_return_multiple_paths) {
    XiFunc *callee = make_branch_multi_ret_callee();
    XiFunc *caller = make_func("branch_multi_ret_caller", &stub_int);
    ASSERT(callee != NULL);
    ASSERT(caller != NULL);

    XiValue *call = add_known_call(caller, callee);
    ASSERT(call != NULL);
    XiValue *extract0 = xi_value_new(caller, caller->entry, XI_EXTRACT, &stub_int, 1);
    XiValue *extract1 = xi_value_new(caller, caller->entry, XI_EXTRACT, &stub_int, 1);
    ASSERT(extract0 != NULL);
    ASSERT(extract1 != NULL);
    extract0->args[0] = call;
    extract0->aux_int = 0;
    extract1->args[0] = call;
    extract1->aux_int = 1;
    xi_block_set_return(caller->entry, extract1);

    XiPassChange chg = xi_opt_inline(caller);
    ASSERT(chg.cfg_changed && chg.values_changed);
    ASSERT(extract0->op == XI_COPY);
    ASSERT(extract1->op == XI_COPY);
    ASSERT(extract0->args[0] && extract0->args[0]->op == XI_PHI);
    ASSERT(extract1->args[0] && extract1->args[0]->op == XI_PHI);
    ASSERT(extract0->args[0]->nargs == 2);
    ASSERT(extract1->args[0]->nargs == 2);
    ASSERT(extract0->args[0]->args[0]->op == XI_CONST);
    ASSERT(extract0->args[0]->args[1]->op == XI_CONST);
    ASSERT(extract1->args[0]->args[0]->op == XI_CONST);
    ASSERT(extract1->args[0]->args[1]->op == XI_CONST);
    ASSERT(extract0->args[0]->args[0]->aux_int == 10);
    ASSERT(extract0->args[0]->args[1]->aux_int == 20);
    ASSERT(extract1->args[0]->args[0]->aux_int == 11);
    ASSERT(extract1->args[0]->args[1]->aux_int == 21);

    xi_func_free(caller);
    xi_func_free(callee);
}

TEST(rejects_multi_return_extract_out_of_range) {
    XiFunc *callee = make_multi_ret_callee(2);
    XiFunc *caller = make_func("bad_extract_caller", &stub_int);
    ASSERT(callee != NULL);
    ASSERT(caller != NULL);

    XiValue *call = add_known_call(caller, callee);
    ASSERT(call != NULL);
    XiValue *extract = xi_value_new(caller, caller->entry, XI_EXTRACT, &stub_int, 1);
    ASSERT(extract != NULL);
    extract->args[0] = call;
    extract->aux_int = 2;
    xi_block_set_return(caller->entry, extract);

    XiPassChange chg = xi_opt_inline(caller);
    ASSERT(!chg.cfg_changed && !chg.values_changed && !chg.types_changed);
    ASSERT(call->op == XI_CALL);
    ASSERT(extract->op == XI_EXTRACT);
    ASSERT(extract->args[0] == call);

    xi_func_free(caller);
    xi_func_free(callee);
}

TEST(inlines_direct_multi_return_use_as_first_value) {
    XiFunc *callee = make_multi_ret_callee(2);
    XiFunc *caller = make_func("direct_multi_ret_use_caller", &stub_int);
    ASSERT(callee != NULL);
    ASSERT(caller != NULL);

    XiValue *call = add_known_call(caller, callee);
    ASSERT(call != NULL);
    XiValue *one = xi_const_int(caller, caller->entry, 1, &stub_int);
    XiValue *sum = xi_value_new(caller, caller->entry, XI_ADD, &stub_int, 2);
    ASSERT(one != NULL);
    ASSERT(sum != NULL);
    sum->args[0] = call;
    sum->args[1] = one;
    xi_block_set_return(caller->entry, sum);

    XiPassChange chg = xi_opt_inline(caller);
    ASSERT(chg.cfg_changed && chg.values_changed);
    ASSERT(sum->args[0] != call);
    ASSERT(sum->args[0] != NULL);
    ASSERT(sum->args[0]->op == XI_CONST);
    ASSERT(sum->args[0]->aux_int == 10);
    ASSERT(sum->args[1] == one);

    xi_func_free(caller);
    xi_func_free(callee);
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
    run_inlines_multi_return_two_values();
    run_inlines_multi_return_three_values();
    run_inlines_multi_return_four_values();
    run_inlines_multi_return_multiple_paths();
    run_rejects_multi_return_extract_out_of_range();
    run_inlines_direct_multi_return_use_as_first_value();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
