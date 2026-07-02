/*
 * Unit tests for Xi inline cost model (xi_inline_benefit).
 * Covers benefit scoring under various callee and call-site scenarios.
 */

#include "../../../src/ir/xi_opt_inline.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
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
    /* leaf straight-line score=60-5+1=56 */
    ASSERT(b == 56);
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
    /* leaf straight-line score=60-35+1, +15(const) = 41 */
    ASSERT(b == 41);
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
    /* leaf straight-line score=60-38+1, +10(single) = 33 */
    ASSERT(b == 33);
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

/* ========== Test: straight-line helper ignores large caller penalty ========== */

TEST(straightline_helper_ignores_large_caller_penalty) {
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
    /* leaf straight-line score=60-25+1; large caller does not suppress it */
    ASSERT(b == 36);
}

/* ========== Test: large caller still penalizes non-leaf helper ========== */

TEST(large_caller_penalizes_general_helper) {
    XiInlineCostModel cost = {
        .value_count = 25,
        .call_count = 1,
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
    /* base=30-25=5, -3(call), -15(large caller) = -13 */
    ASSERT(b == -13);
}

/* ========== Test: exception flow is not inlined ========== */

TEST(throw_never_inline) {
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
    ASSERT(b == -1000);
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

TEST(inlines_unannotated_callee_under_tbaa_invariant) {
    XiFunc *callee = make_func("shared_load_callee", &stub_int);
    XiFunc *caller = make_func("shared_load_caller", &stub_int);
    ASSERT(callee != NULL);
    ASSERT(caller != NULL);

    XiValue *shared = xi_value_new(callee, callee->entry, XI_GET_SHARED, &stub_int, 0);
    ASSERT(shared != NULL);
    shared->aux_int = 0;
    xi_block_set_return(callee->entry, shared);

    XiValue *call = add_known_call(caller, callee);
    ASSERT(call != NULL);
    xi_block_set_return(caller->entry, call);
    caller->invariant_mask |= XI_INV_TBAA_ANNOTATED;

    XiPassChange chg = xi_opt_inline(caller);
    ASSERT(chg.cfg_changed && chg.values_changed);
    char errbuf[512];
    ASSERT(xi_verify(caller, errbuf, sizeof(errbuf)));
    ASSERT(caller->entry->succs[0] != NULL);
    XiValue *cloned = caller->entry->succs[0]->values[0];
    ASSERT(cloned != NULL);
    ASSERT(cloned->op == XI_GET_SHARED);
    ASSERT(cloned->mem_group == XI_MEM_SHARED);

    xi_func_free(caller);
    xi_func_free(callee);
}

TEST(inlines_top_level_shared_function_load) {
    XiFunc *program = make_func("program", &stub_int);
    XiFunc *callee = make_func("recv_int", &stub_int);
    XiFunc *caller = make_func("stage", &stub_int);
    ASSERT(program != NULL);
    ASSERT(callee != NULL);
    ASSERT(caller != NULL);

    program->nshared = 1;
    program->shared_slot_func_count = 1;
    program->shared_slot_funcs = (XiFunc **) xi_func_arena_alloc(program, sizeof(XiFunc *));
    ASSERT(program->shared_slot_funcs != NULL);
    program->shared_slot_funcs[0] = callee;
    callee->parent_func = program;
    caller->parent_func = program;

    XiValue *ret = xi_const_int(callee, callee->entry, 42, &stub_int);
    ASSERT(ret != NULL);
    xi_block_set_return(callee->entry, ret);

    XiValue *load = xi_value_new(caller, caller->entry, XI_GET_SHARED, &stub_func, 0);
    ASSERT(load != NULL);
    load->aux_int = 0;
    XiValue *call = xi_value_new(caller, caller->entry, XI_CALL, &stub_int, 1);
    ASSERT(call != NULL);
    call->args[0] = load;
    xi_block_set_return(caller->entry, call);

    XiPassChange chg = xi_opt_inline(caller);
    ASSERT(chg.cfg_changed && chg.values_changed);
    char errbuf[512];
    ASSERT(xi_verify(caller, errbuf, sizeof(errbuf)));
    ASSERT(caller->entry->succs[0] != NULL);
    XiBlock *inlined_entry = caller->entry->succs[0];
    ASSERT(inlined_entry->nvalues == 1);
    ASSERT(inlined_entry->values[0]->op == XI_CONST);

    xi_func_free(caller);
    xi_func_free(callee);
    xi_func_free(program);
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
    run_straightline_helper_ignores_large_caller_penalty();
    run_large_caller_penalizes_general_helper();
    run_throw_never_inline();
    run_combined_bonuses();
    run_budget_small_caller();
    run_budget_medium_caller();
    run_budget_large_caller();
    run_inlines_unannotated_callee_under_tbaa_invariant();
    run_inlines_top_level_shared_function_load();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
