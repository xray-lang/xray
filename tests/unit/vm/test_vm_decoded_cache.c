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
#include "../../../src/vm/xr_typed_dispatch.h"
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

static CacheFixture build_divide_by_zero_fixture(void) {
    CacheFixture fixture = {0};
    XiFunc *function = xi_func_new("decoded_cache_divide_by_zero", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, 42, &stub_int);
    XiValue *right = xi_const_int(function, entry, 0, &stub_int);
    XiValue *result = xi_binary(function, entry, XI_DIV, &stub_int,
                                left, right);
    REQUIRE(left && right && result);
    xi_block_set_return(entry, result);
    entry->sealed = true;
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
    return fixture;
}

static bool freeze_with_rows(const CacheFixture *fixture,
                             const XrTargetInstructionRecord *rows,
                             uint32_t row_count, XrTargetPlan **out,
                             char *error, size_t error_size) {
    XrTargetPlanDraft draft = {
        .semantic_plan = fixture->semantic,
        .profile = fixture->profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
    };
#define COPY_TABLE(name)                                                        \
    draft.name = xr_target_plan_##name(fixture->plan, &draft.name##_count)
    COPY_TABLE(machine_reps);
    COPY_TABLE(value_reps);
    COPY_TABLE(extents);
    COPY_TABLE(layouts);
    COPY_TABLE(fields);
    COPY_TABLE(storage);
    COPY_TABLE(allocations);
    COPY_TABLE(extent_operands);
    COPY_TABLE(functions);
    COPY_TABLE(slots);
    COPY_TABLE(calls);
    COPY_TABLE(call_arguments);
    COPY_TABLE(root_maps);
    COPY_TABLE(root_slots);
    COPY_TABLE(cleanups);
    COPY_TABLE(adapters);
    COPY_TABLE(capabilities);
    COPY_TABLE(coroutines);
#undef COPY_TABLE
    draft.instructions = rows;
    draft.instructions_count = row_count;
    return xr_target_plan_freeze(&draft, out, error, error_size);
}

static XrTypedDispatchStatus execute_i64(
    const XrTargetPlan *plan, const XrFingerprint *fingerprint,
    const XrVmDecodedCache *cache, const int64_t *arguments,
    uint32_t argument_count, int64_t *result) {
    REQUIRE(result != NULL);
    int64_t switch_result = *result;
    int64_t table_result = *result;
    XrTypedDispatchI64Request switch_request = {
        .verified_plan = plan,
        .required_plan_fingerprint = fingerprint,
        .arguments = arguments,
        .result = &switch_result,
        .decoded_cache = cache,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
        .function = 0,
        .argument_count = argument_count,
    };
    XrTypedDispatchI64Request table_request = switch_request;
    table_request.result = &table_result;
    table_request.provider =
        XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE;
    XrTypedDispatchStatus switch_status =
        xr_typed_dispatch_execute_i64(&switch_request);
    XrTypedDispatchStatus table_status =
        xr_typed_dispatch_execute_i64(&table_request);
    REQUIRE(table_status == switch_status);
    REQUIRE(table_result == switch_result);
    *result = switch_result;
    return switch_status;
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
    REQUIRE(xr_typed_decoded_cache_create(fixture.plan, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(cache != NULL);

    XrVmDecodedCacheStats stats;
    REQUIRE(xr_typed_decoded_cache_stats(cache, &stats));
    REQUIRE(stats.plan_schema_version == XR_TARGET_PLAN_SCHEMA_VERSION);
    REQUIRE(stats.function_count == 1);
    REQUIRE(stats.instruction_count == 8);
    REQUIRE(stats.block_count == 3);
    REQUIRE(stats.total_bytes <= XR_VM_DECODED_CACHE_MAX_BYTES);
    REQUIRE(xr_fingerprint_equal(stats.plan_fingerprint, fingerprint));

    XrVmDecodedFunctionView function;
    REQUIRE(xr_typed_decoded_cache_function(cache, 0, &function));
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
    REQUIRE(xr_typed_decoded_cache_size_within_budget(1, 8, 3, &bytes));
    REQUIRE(bytes == stats.total_bytes);
    REQUIRE(!xr_typed_decoded_cache_size_within_budget(
        XR_VM_DECODED_CACHE_MAX_FUNCTIONS + 1u, 0, 0, &bytes));
    REQUIRE(!xr_typed_decoded_cache_size_within_budget(
        0, XR_VM_DECODED_CACHE_MAX_ROWS + 1u, 0, &bytes));
    REQUIRE(!xr_typed_decoded_cache_size_within_budget(
        0, 0, XR_VM_DECODED_CACHE_MAX_BLOCKS + 1u, &bytes));

    xr_typed_decoded_cache_free(cache);
    dispose_fixture(&fixture);
}

static void test_identity_and_invalid_plan_fail_closed(void) {
    CacheFixture first = build_branch_fixture();
    CacheFixture second = build_branch_fixture();
    XrFingerprint first_fingerprint = xr_target_plan_fingerprint(first.plan);
    XrFingerprint second_fingerprint = xr_target_plan_fingerprint(second.plan);
    REQUIRE(xr_fingerprint_equal(first_fingerprint, second_fingerprint));
    XrVmDecodedCache *cache = NULL;
    REQUIRE(xr_typed_decoded_cache_create(first.plan, &first_fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(xr_typed_decoded_cache_require_exact(
                cache, first.plan, &first_fingerprint) ==
            XR_VM_DECODED_CACHE_OK);
    REQUIRE(xr_typed_decoded_cache_require_exact(
                cache, second.plan, &second_fingerprint) ==
            XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH);

    first_fingerprint.bytes[0] ^= 1u;
    REQUIRE(xr_typed_decoded_cache_require_exact(
                cache, first.plan, &first_fingerprint) ==
            XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH);
    first_fingerprint.bytes[0] ^= 1u;

    /* A miss always re-verifies the supposedly immutable plan before it can
     * publish anything. This deliberate internal corruption is therefore
     * refused even though a prior cache exists for the original frozen image. */
    first.plan->instructions[0].immediate_bits ^= UINT64_C(1);
    XrVmDecodedCache *invalid = (XrVmDecodedCache *) (uintptr_t) 1;
    REQUIRE(xr_typed_decoded_cache_create(first.plan, &first_fingerprint,
                                       &invalid) ==
            XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED);
    REQUIRE(invalid == NULL);
    first.plan->instructions[0].immediate_bits ^= UINT64_C(1);

    xr_typed_decoded_cache_free(cache);
    dispose_fixture(&second);
    dispose_fixture(&first);
}

static void test_cached_and_uncached_execution_parity(void) {
    CacheFixture branch = build_branch_fixture();
    XrFingerprint fingerprint = xr_target_plan_fingerprint(branch.plan);
    XrVmDecodedCache *cache = NULL;
    REQUIRE(xr_typed_decoded_cache_create(branch.plan, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    const int64_t cases[][2] = {{9, 4}, {4, 9}};
    for (uint32_t i = 0; i < 2; i++) {
        int64_t uncached = 91;
        int64_t cached = 92;
        XrTypedDispatchStatus uncached_status = execute_i64(
            branch.plan, &fingerprint, NULL, cases[i], 2, &uncached);
        XrTypedDispatchStatus cached_status = execute_i64(
            branch.plan, &fingerprint, cache, cases[i], 2, &cached);
        REQUIRE(uncached_status == XR_TYPED_DISPATCH_OK);
        REQUIRE(cached_status == uncached_status);
        REQUIRE(cached == uncached);
    }
    int64_t uncached = 91;
    int64_t cached = 92;
    REQUIRE(execute_i64(branch.plan, &fingerprint, NULL, cases[0], 1,
                        &uncached) == XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(execute_i64(branch.plan, &fingerprint, cache, cases[0], 1,
                        &cached) == XR_TYPED_DISPATCH_ARGUMENT_MISMATCH);
    REQUIRE(uncached == 0 && cached == 0);
    xr_typed_decoded_cache_free(cache);
    dispose_fixture(&branch);

    CacheFixture divide = build_divide_by_zero_fixture();
    fingerprint = xr_target_plan_fingerprint(divide.plan);
    cache = NULL;
    REQUIRE(xr_typed_decoded_cache_create(divide.plan, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    uncached = 91;
    cached = 92;
    REQUIRE(execute_i64(divide.plan, &fingerprint, NULL, NULL, 0,
                        &uncached) == XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
    REQUIRE(execute_i64(divide.plan, &fingerprint, cache, NULL, 0,
                        &cached) == XR_TYPED_DISPATCH_DIVIDE_BY_ZERO);
    REQUIRE(uncached == 0 && cached == 0);
    xr_typed_decoded_cache_free(cache);
    dispose_fixture(&divide);

    CacheFixture loop_source = build_branch_fixture();
    uint32_t row_count = 0;
    const XrTargetInstructionRecord *source_rows =
        xr_target_plan_instructions(loop_source.plan, &row_count);
    REQUIRE(source_rows != NULL && row_count == 8);
    XrTargetInstructionRecord rows[8];
    memcpy(rows, source_rows, sizeof(rows));
    uint32_t last = row_count - 1u;
    REQUIRE(rows[last].opcode == XR_TARGET_INSTRUCTION_RETURN_I64);
    rows[last] = (XrTargetInstructionRecord) {
        .id = source_rows[last].id,
        .function = source_rows[last].function,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                          XR_TARGET_INSTRUCTION_SLOT_NONE},
        .immediate_bits = XR_TARGET_INSTRUCTION_TARGET_PACK(last - 1u, 0),
        .opcode = XR_TARGET_INSTRUCTION_JUMP,
        .operand_count = 0,
    };
    XrTargetPlan *loop = NULL;
    char error[512] = {0};
    REQUIRE(freeze_with_rows(&loop_source, rows, row_count, &loop, error,
                             sizeof(error)));
    fingerprint = xr_target_plan_fingerprint(loop);
    cache = NULL;
    REQUIRE(xr_typed_decoded_cache_create(loop, &fingerprint, &cache) ==
            XR_VM_DECODED_CACHE_OK);
    const int64_t loop_arguments[2] = {4, 9};
    uncached = 91;
    cached = 92;
    REQUIRE(execute_i64(loop, &fingerprint, NULL, loop_arguments, 2,
                        &uncached) == XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED);
    REQUIRE(execute_i64(loop, &fingerprint, cache, loop_arguments, 2,
                        &cached) == XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED);
    REQUIRE(uncached == 0 && cached == 0);
    xr_typed_decoded_cache_free(cache);
    xr_target_plan_free(loop);
    dispose_fixture(&loop_source);
}

static void *read_cache_concurrently(void *argument) {
    ConcurrentProbe *probe = (ConcurrentProbe *) argument;
    probe->ok = true;
    for (uint32_t iteration = 0; iteration < 10u; iteration++) {
        XrVmDecodedFunctionView function;
        XrVmDecodedCacheStats stats;
        if (xr_typed_decoded_cache_require_exact(
                probe->cache, probe->plan, &probe->fingerprint) !=
                XR_VM_DECODED_CACHE_OK ||
            !xr_typed_decoded_cache_function(probe->cache, 0, &function) ||
            !xr_typed_decoded_cache_stats(probe->cache, &stats) ||
            function.instruction_count != stats.instruction_count ||
            function.block_count != stats.block_count ||
            function.instructions[3].target_if_nonzero !=
                function.blocks[1].first_row) {
            probe->ok = false;
            break;
        }
        const int64_t arguments[2] = {9, 4};
        int64_t result = 0;
        if (execute_i64(probe->plan, &probe->fingerprint, probe->cache,
                        arguments, 2, &result) != XR_TYPED_DISPATCH_OK ||
            result != 13) {
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
    REQUIRE(xr_typed_decoded_cache_create(fixture.plan, &fingerprint, &cache) ==
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
    xr_typed_decoded_cache_free(cache);
    dispose_fixture(&fixture);
}

int main(void) {
    test_exact_metadata_and_budgets();
    test_identity_and_invalid_plan_fail_closed();
    test_cached_and_uncached_execution_parity();
    test_concurrent_read_only_reuse();
    puts("vm decoded cache tests passed");
    return 0;
}
