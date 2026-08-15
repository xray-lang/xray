/*
 * test_vm_decoded_cache.c - Verified immutable instruction cache tests
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/os/os_thread.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan_internal.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/vm/xr_vm_decoded_cache.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct CacheFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
} CacheFixture;

typedef struct ConcurrentProbe {
    const XrVmDecodedCache *cache;
    const XrTargetPlan *plan;
    XrFingerprint fingerprint;
    bool ok;
} ConcurrentProbe;

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL,
                           .id = 2,
                           .frozen = true,
                           .scalar_rep = XR_SCALAR_REP_NONE};

static CacheFixture build_branch_fixture(void) {
    CacheFixture fixture = {0};
    XiFunc *function = xi_func_new("decoded_cache_branch", &stub_int);
    REQUIRE(function != NULL);
    function->nparams = function->min_params = 2;
    function->params = (XiValue **) xr_calloc(2, sizeof(*function->params));
    REQUIRE(function->params != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *taken = xi_block_new(function);
    XiBlock *untaken = xi_block_new(function);
    REQUIRE(entry && taken && untaken);
    entry->sealed = taken->sealed = untaken->sealed = true;
    function->params[0] = xi_param(function, entry, 0, &stub_int);
    function->params[1] = xi_param(function, entry, 1, &stub_int);
    XiValue *condition =
        xi_value_new(function, entry, XI_GT, &stub_bool, 2);
    XiValue *sum = xi_value_new(function, taken, XI_ADD, &stub_int, 2);
    XiValue *difference =
        xi_value_new(function, untaken, XI_SUB, &stub_int, 2);
    REQUIRE(function->params[0] && function->params[1] && condition && sum &&
            difference);
    condition->args[0] = function->params[0];
    condition->args[1] = function->params[1];
    sum->args[0] = function->params[0];
    sum->args[1] = function->params[1];
    difference->args[0] = function->params[0];
    difference->args[1] = function->params[1];
    xi_block_set_if(entry, condition, taken, untaken);
    xi_block_set_return(taken, sum);
    xi_block_set_return(untaken, difference);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build(function, &fixture.semantic, error,
                                   sizeof(error)));
    xi_func_free(function);
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture.profile != NULL);
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile,
                                 &fixture.plan, error, sizeof(error)));
    REQUIRE(xr_target_plan_is_verified(fixture.plan));
    return fixture;
}

static void dispose_fixture(CacheFixture *fixture) {
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static void test_exact_metadata_and_budgets(void) {
    CacheFixture fixture = build_branch_fixture();
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    XrVmDecodedCache *cache = NULL;
    REQUIRE(xr_vm_decoded_cache_create(fixture.plan, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(cache != NULL);

    XrVmDecodedCacheStats stats;
    REQUIRE(xr_vm_decoded_cache_stats(cache, &stats));
    REQUIRE(stats.plan_schema_version == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(stats.function_count == 1);
    REQUIRE(stats.instruction_count == 8);
    REQUIRE(stats.block_count == 3);
    REQUIRE(stats.total_bytes <= XR_VM_DECODED_CACHE_MAX_BYTES);
    REQUIRE(xr_fingerprint_equal(stats.plan_fingerprint, fingerprint));

    XrVmDecodedFunctionView function;
    REQUIRE(xr_vm_decoded_cache_function(cache, 0, &function));
    REQUIRE(function.instruction_count == 8);
    REQUIRE(function.block_count == 3);
    REQUIRE(function.parameter_count == 2);
    REQUIRE(function.blocks[0].first_row == 0);
    REQUIRE(function.blocks[0].row_count == 4);
    REQUIRE(function.blocks[0].successor_count == 2);
    REQUIRE(function.blocks[0].successors[0] == 2);
    REQUIRE(function.blocks[0].successors[1] == 1);
    REQUIRE(function.blocks[1].successor_count == 0);
    REQUIRE(function.blocks[2].successor_count == 0);
    for (uint32_t i = 0; i < function.instruction_count; i++) {
        const XrVmDecodedInstruction *row = &function.instructions[i];
        const XrTargetInstructionContract *expected =
            xr_target_instruction_contract(row->row.opcode);
        REQUIRE(row->contract != NULL);
        REQUIRE(expected != NULL);
        REQUIRE(row->contract->dispatch_kind == expected->dispatch_kind);
        REQUIRE(row->contract->dispatch_argument ==
                expected->dispatch_argument);
        REQUIRE(row->contract->control_kind == expected->control_kind);
        REQUIRE(row->contract->immediate_kind == expected->immediate_kind);
        REQUIRE(row->block < function.block_count);
    }
    const XrVmDecodedInstruction *branch = &function.instructions[3];
    REQUIRE(branch->contract->control_kind ==
            XR_TARGET_INSTRUCTION_CONTROL_BRANCH);
    REQUIRE(branch->target_if_zero == function.blocks[2].first_row);
    REQUIRE(branch->target_if_nonzero == function.blocks[1].first_row);

    size_t bytes = 0;
    REQUIRE(xr_vm_decoded_cache_size_within_budget(1, 8, 3, &bytes));
    REQUIRE(bytes == stats.total_bytes);
    REQUIRE(!xr_vm_decoded_cache_size_within_budget(
        XR_VM_DECODED_CACHE_MAX_FUNCTIONS + 1u, 0, 0, &bytes));
    REQUIRE(!xr_vm_decoded_cache_size_within_budget(
        0, XR_VM_DECODED_CACHE_MAX_ROWS + 1u, 0, &bytes));
    REQUIRE(!xr_vm_decoded_cache_size_within_budget(
        0, 0, XR_VM_DECODED_CACHE_MAX_BLOCKS + 1u, &bytes));

    xr_vm_decoded_cache_free(cache);
    dispose_fixture(&fixture);
}

static void test_identity_and_invalid_plan_fail_closed(void) {
    CacheFixture first = build_branch_fixture();
    CacheFixture second = build_branch_fixture();
    XrFingerprint first_fingerprint = xr_target_plan_fingerprint(first.plan);
    XrFingerprint second_fingerprint = xr_target_plan_fingerprint(second.plan);
    REQUIRE(xr_fingerprint_equal(first_fingerprint, second_fingerprint));
    XrVmDecodedCache *cache = NULL;
    REQUIRE(xr_vm_decoded_cache_create(first.plan, &first_fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(xr_vm_decoded_cache_require_exact(
                cache, first.plan, &first_fingerprint) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(xr_vm_decoded_cache_require_exact(
                cache, second.plan, &second_fingerprint) ==
            XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH);

    first_fingerprint.bytes[0] ^= 1u;
    REQUIRE(xr_vm_decoded_cache_require_exact(
                cache, first.plan, &first_fingerprint) ==
            XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH);
    first_fingerprint.bytes[0] ^= 1u;

    /* A miss always re-verifies the supposedly immutable plan before it can
     * publish anything. This deliberate internal corruption is therefore
     * refused even though a prior cache exists for the original frozen image. */
    first.plan->instructions[0].immediate_bits ^= UINT64_C(1);
    XrVmDecodedCache *invalid = (XrVmDecodedCache *) (uintptr_t) 1;
    REQUIRE(xr_vm_decoded_cache_create(first.plan, &first_fingerprint,
                                       &invalid) ==
            XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED);
    REQUIRE(invalid == NULL);
    first.plan->instructions[0].immediate_bits ^= UINT64_C(1);

    xr_vm_decoded_cache_free(cache);
    dispose_fixture(&second);
    dispose_fixture(&first);
}

static void *read_cache_concurrently(void *argument) {
    ConcurrentProbe *probe = (ConcurrentProbe *) argument;
    probe->ok = true;
    for (uint32_t iteration = 0; iteration < 10u; iteration++) {
        XrVmDecodedFunctionView function;
        XrVmDecodedCacheStats stats;
        if (xr_vm_decoded_cache_require_exact(
                probe->cache, probe->plan, &probe->fingerprint) !=
                XR_VM_DECODED_CACHE_OK ||
            !xr_vm_decoded_cache_function(probe->cache, 0, &function) ||
            !xr_vm_decoded_cache_stats(probe->cache, &stats) ||
            function.instruction_count != stats.instruction_count ||
            function.block_count != stats.block_count ||
            function.instructions[3].target_if_nonzero !=
                function.blocks[1].first_row) {
            probe->ok = false;
            break;
        }
    }
    return NULL;
}

static void test_concurrent_read_only_reuse(void) {
    enum { THREAD_COUNT = 8 };
    CacheFixture fixture = build_branch_fixture();
    XrFingerprint fingerprint = xr_target_plan_fingerprint(fixture.plan);
    XrVmDecodedCache *cache = NULL;
    REQUIRE(xr_vm_decoded_cache_create(fixture.plan, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    xr_thread_t threads[THREAD_COUNT] = {0};
    ConcurrentProbe probes[THREAD_COUNT] = {0};
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        probes[i].cache = cache;
        probes[i].plan = fixture.plan;
        probes[i].fingerprint = fingerprint;
        REQUIRE(xr_thread_create(&threads[i], read_cache_concurrently,
                                 &probes[i]));
    }
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        REQUIRE(probes[i].ok);
    }
    xr_vm_decoded_cache_free(cache);
    dispose_fixture(&fixture);
}

int main(void) {
    test_exact_metadata_and_budgets();
    test_identity_and_invalid_plan_fail_closed();
    test_concurrent_read_only_reuse();
    puts("vm decoded cache tests passed");
    return 0;
}
