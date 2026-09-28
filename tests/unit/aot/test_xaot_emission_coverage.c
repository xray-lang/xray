/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xaot_emission_coverage.c - Scoped coverage admission and ownership
 */

#define xaot_boundary_direct_i64_function_status probe_direct_i64_function_status
#define xaot_boundary_direct_i64_abi_status probe_direct_i64_abi_status
#define xaot_boundary_direct_i64_call_view probe_direct_i64_call_view
#define xaot_boundary_leaf_aggregate_function_status probe_leaf_aggregate_function_status
#define xaot_boundary_leaf_aggregate_semantic_value probe_leaf_aggregate_semantic_value
#define xaot_boundary_leaf_aggregate_abi_status probe_leaf_aggregate_abi_status
#define xaot_boundary_leaf_aggregate_call_view probe_leaf_aggregate_call_view
#define xaot_boundary_resolve_constructor_call_target probe_resolve_constructor_call_target
#define xaot_boundary_reason_name probe_boundary_reason_name
#define xaot_boundary_step_kind_name probe_boundary_step_kind_name
#define xaot_boundary_resolve_direct_call_target probe_resolve_direct_call_target
#define xaot_boundary_resolve_call_batches probe_resolve_call_batches
#define xaot_boundary_resolve_function_calls probe_resolve_function_calls
#include "aot/xaot_boundary.h"
#include "aot/xaot_bundle.h"
#include "base/xmalloc.h"
#include "ir/xi_module.h"
#include "plan/semantic/xr_semantic_builder.h"
#include "plan/target/xr_target_builder.h"
#include "plan/target/xr_target_plan_internal.h"
#include "runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                        \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                              \
        }                                                                                         \
    } while (0)

static size_t fingerprint_checks;
static bool counted_fingerprint(const XrTargetPlan *plan) {
    fingerprint_checks++;
    return xr_target_plan_fingerprint_is_intact(plan);
}

/* Compile the real boundary under test-local names and observe its complete
 * fingerprint checks without adding hooks to the production implementation. */
#define xr_target_plan_fingerprint_is_intact counted_fingerprint
#include "../../../src/aot/xaot_boundary.c"
#undef xr_target_plan_fingerprint_is_intact

static size_t allocation_attempts, live_allocations;
static bool fail_allocation;
static size_t fail_allocation_at = SIZE_MAX;
static void *coverage_calloc(size_t count, size_t size) {
    size_t attempt = allocation_attempts++;
    if (fail_allocation || attempt == fail_allocation_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        live_allocations++;
    return pointer;
}

static void coverage_free(void *pointer) {
    if (pointer) {
        REQUIRE(live_allocations != 0);
        live_allocations--;
    }
    xr_free(pointer);
}

#undef xr_calloc
#undef xr_free
#define xr_calloc coverage_calloc
#define xr_free coverage_free
#include "../../../src/aot/xi_cgen_emission_coverage.inc.c"
#include "../../../src/aot/xaot_prepare_calls.inc.c"
#undef xr_calloc
#undef xr_free

static XrType integer = {.kind = XR_KIND_INT, .id = 29001,
                         .scalar_rep = XR_NATIVE_I64, .frozen = true};

static void test_preparation_facts(XaotBundle *bundle, XiFunc *root, XrTargetPlan *target) {
    XiFunc callee = {0};
    XiValue closure = {.op = XI_CLOSURE_NEW, .aux = &callee};
    XiValue *arguments[] = {&closure};
    XiValue call = {.op = XI_CALL, .id = 1, .nargs = 1, .args = arguments};
    XiValue *values[] = {&call};
    XiBlock block = {.values = values, .nvalues = 1};
    XiBlock *blocks[] = {&block};
    XiFunc caller = {.blocks = blocks, .nblocks = 1, .next_value_id = 2};
    bundle->func_plans[0].func = &caller;
    bundle->func_plans[0].reachable = true;
    PrepareCallFacts facts = {0};
    fingerprint_checks = 0;
    REQUIRE(prepare_call_facts_build(bundle, &facts));
    REQUIRE(fingerprint_checks == 1 && facts.count == 2 && live_allocations == 2);
    const XaotBoundaryCallTargets *fact = prepare_call_fact(&facts.functions[0], &caller, &call);
    REQUIRE(fact && fact->uncovered_direct == &callee && fact->first_arg == 1);
    for (uint32_t i = 0; i < 10000; ++i)
        REQUIRE(prepare_call_fact(&facts.functions[0], &caller, &call) == fact);
    REQUIRE(fingerprint_checks == 1);
    REQUIRE(!prepare_call_fact(&facts.functions[0], root, &call));
    prepare_call_facts_dispose(&facts);
    REQUIRE(!facts.functions && !facts.targets && !facts.count && !live_allocations);

    closure.aux = root;
    REQUIRE(prepare_call_facts_build(bundle, &facts));
    REQUIRE(fingerprint_checks == 2);
    REQUIRE(prepare_call_fact(&facts.functions[0], &caller, &call)->uncovered_direct == root);
    prepare_call_facts_dispose(&facts);
    target->fingerprint.bytes[0] ^= 1;
    REQUIRE(!prepare_call_facts_build(bundle, &facts));
    REQUIRE(!facts.functions && !facts.targets && !facts.count && !live_allocations);
    target->fingerprint.bytes[0] ^= 1;
    call.id = 2;
    REQUIRE(!prepare_call_facts_build(bundle, &facts));
    REQUIRE(!facts.functions && !facts.targets && !facts.count && !live_allocations);
    call.id = 1;
    for (size_t offset = 0; offset < 2; ++offset) {
        fail_allocation_at = allocation_attempts + offset;
        REQUIRE(!prepare_call_facts_build(bundle, &facts));
        REQUIRE(!facts.functions && !facts.targets && !facts.count && !live_allocations);
    }
    fail_allocation_at = SIZE_MAX;
    caller.next_value_id = UINT32_MAX;
    size_t attempts = allocation_attempts;
    REQUIRE(!prepare_call_facts_build(bundle, &facts));
    REQUIRE(attempts == allocation_attempts && !live_allocations);
    caller.next_value_id = 2;
    REQUIRE(prepare_call_facts_build(bundle, &facts));
    prepare_call_facts_dispose(&facts);
    REQUIRE(!live_allocations);
    bundle->func_plans[0].func = root;
    bundle->func_plans[0].reachable = false;
    puts("PASS: preparation facts, fresh identity, corrupt authority, bounds and both OOM sites");
}

int main(void) {
    char error[512] = {0};
    XiFunc *root = xi_func_new("coverage_root", &integer);
    REQUIRE(root != NULL);
    XiBlock *block = xi_block_new(root);
    REQUIRE(block != NULL);
    XiValue *answer = xi_const_int(root, block, 42, &integer);
    REQUIRE(answer != NULL);
    xi_block_set_return(block, answer);
    root->stage = XI_STAGE_OPTIMIZED;
    XiModule module = {.name = "coverage", .path = "coverage.xr", .init = root,
                       .identity = "memory-module-v1:id=8:coverage"};
    root->module = &module;
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(profile != NULL);
    XrTargetPlan *target = NULL;
    REQUIRE(xr_target_plan_build(root->semantic_plan, profile, &target, error, sizeof(error)));
    XiModule *modules[] = {&module};
    XiFunc uncovered = {0};
    XaotFuncPlan functions[] = {{.func = &uncovered}, {.func = root}};
    XaotBundle bundle = {.modules = modules, .nmodules = 1, .program_target_plan = target,
                          .func_plans = functions, .nfunc_plans = XR_COUNTOF(functions)};
    CgEmissionCoverage coverage = {0};

    fingerprint_checks = 0;
    REQUIRE(cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(fingerprint_checks == 1 && coverage.count == 2 && live_allocations == 1);
    XaotBoundaryFunctionCoverage root_coverage = cg_emission_coverage_lookup(&coverage, root);
    REQUIRE(root_coverage == XAOT_BOUNDARY_FUNCTION_DIRECT_I64);
    for (uint32_t i = 0; i < 10000; i++) {
        REQUIRE(cg_emission_coverage_lookup(&coverage, root) == root_coverage);
        REQUIRE(cg_emission_coverage_lookup(&coverage, &uncovered) ==
                XAOT_BOUNDARY_FUNCTION_UNCOVERED);
    }
    REQUIRE(fingerprint_checks == 1);
    REQUIRE(xaot_boundary_direct_i64_function_status(&bundle, root, NULL, NULL, NULL, 0) ==
            XAOT_DIRECT_I64_TARGET_FOUND);
    REQUIRE(fingerprint_checks == 2);
    cg_emission_coverage_dispose(&coverage);
    REQUIRE(!coverage.functions && !coverage.count && !live_allocations);
    REQUIRE(cg_emission_coverage_lookup(&coverage, root) == XAOT_BOUNDARY_FUNCTION_INVALID);
    REQUIRE(cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(fingerprint_checks == 3);
    cg_emission_coverage_dispose(&coverage);

    target->fingerprint.bytes[0] ^= 1;
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(!coverage.functions && !coverage.count && !live_allocations);
    target->fingerprint.bytes[0] ^= 1;

    uint32_t saved_index = root->semantic_plan_function_index;
    root->semantic_plan_function_index = UINT32_MAX - 1;
    XaotBoundaryFunctionCalls batch[] = {
        {.function = &uncovered}, {.function = root}};
    REQUIRE(!xaot_boundary_resolve_call_batches(&bundle, batch, XR_COUNTOF(batch)));
    REQUIRE(batch[0].coverage == XAOT_BOUNDARY_FUNCTION_INVALID &&
            batch[1].coverage == XAOT_BOUNDARY_FUNCTION_INVALID);
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(!coverage.functions && !coverage.count && !live_allocations);
    root->semantic_plan_function_index = saved_index;

    fail_allocation = true;
    size_t attempts = allocation_attempts;
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(allocation_attempts == attempts + 1 && !coverage.functions &&
            !coverage.count && !live_allocations);
    fail_allocation = false;
    functions[0].func = root;
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage) && !live_allocations);
    functions[0].func = NULL;
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage) && !live_allocations);
    functions[0].func = &uncovered;
    bundle.nfunc_plans = UINT32_MAX;
    attempts = allocation_attempts;
    REQUIRE(!cg_emission_coverage_build(&bundle, &coverage));
    REQUIRE(allocation_attempts == attempts && !live_allocations);
    bundle.nfunc_plans = XR_COUNTOF(functions);
    REQUIRE(cg_emission_coverage_build(&bundle, &coverage));
    cg_emission_coverage_dispose(&coverage);
    REQUIRE(!live_allocations);

    test_preparation_facts(&bundle, root, target);

    xr_target_plan_free(target);
    xr_target_profile_free(profile);
    root->module = NULL;
    xi_func_free(root);
    puts("PASS: 20000 coverage lookups, 1 content admission; fresh scopes and standalone queries recheck");
    puts("PASS: no-call functions, failed batches, input mutation, OOM, duplicates and byte budget");
    return 0;
}
