/*
 * test_runtime_generation.c - Loaded module generation authority tests
 */

#include "../../../include/xray_runtime_generation.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/os/os_thread.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/plan/target/xr_target_plan.h"
#include "../../../src/plan/target/xr_target_profile_internal.h"
#include "../../../src/runtime/abi/xr_runtime_target_authority.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/runtime/xr_module_generation_internal.h"
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

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrTargetPlan *finish_plan(XiFunc *root,
                                 XrSemanticPlan **semantic_out,
                                 XrTargetProfile **profile_out) {
    XrSemanticPlan *semantic = NULL;
    char diagnostic[512] = {0};
    REQUIRE(xr_semantic_plan_build(root, &semantic, diagnostic,
                                   sizeof(diagnostic)));
    xi_func_free(root);

    XrRuntimeTargetAuthority native;
    REQUIRE(xr_runtime_target_authority_native_hosted(&native) ==
            XR_RUNTIME_ABI_OK);
    XrTargetProfileBuildInput input = {
        .machine = native.machine,
        .runtime_abi = &native.runtime_abi,
        .object_header_materialization =
            &native.object_header_materialization,
        .string_contract = &native.string_contract,
        .providers = native.providers,
        .provider_count = native.provider_count,
    };
    XrTargetProfile *profile = NULL;
    REQUIRE(xr_target_profile_build(&input, &profile, diagnostic,
                                    sizeof(diagnostic)));
    XrTargetPlan *plan = NULL;
    REQUIRE(xr_target_plan_build(semantic, profile, &plan, diagnostic,
                                 sizeof(diagnostic)));
    REQUIRE(xr_target_plan_is_verified(plan));
    *semantic_out = semantic;
    *profile_out = profile;
    return plan;
}

static XrTargetPlan *build_plan(XrSemanticPlan **semantic_out,
                                XrTargetProfile **profile_out) {
    XiFunc *function = xi_func_new("generation_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    return finish_plan(function, semantic_out, profile_out);
}

/* A rotate is an exact-width bit intrinsic keyed on a receiver width the
 * scalar execution family has no authority over, so it keeps the sole function
 * outside that family and this route fail closed. */
static XrTargetPlan *build_unsupported_plan(
    XrSemanticPlan **semantic_out, XrTargetProfile **profile_out) {
    XiFunc *function = xi_func_new("generation_rotate_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, 84, &stub_int);
    XiValue *right = xi_const_int(function, entry, 2, &stub_int);
    XiValue *result = xi_binary(function, entry, XI_BIT_ROTL, &stub_int,
                                left, right);
    REQUIRE(left != NULL && right != NULL && result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    return finish_plan(function, semantic_out, profile_out);
}

/* Division is executable, so this sole function is eligible all the way to
 * ACTIVE and only its zero divisor stops it. That separates a program fault
 * from the authority failures the unsupported plan produces. */
static XrTargetPlan *build_divide_by_zero_plan(
    XrSemanticPlan **semantic_out, XrTargetProfile **profile_out) {
    XiFunc *function = xi_func_new("generation_divide_by_zero_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *left = xi_const_int(function, entry, 84, &stub_int);
    XiValue *right = xi_const_int(function, entry, 0, &stub_int);
    XiValue *result = xi_binary(function, entry, XI_DIV, &stub_int,
                                left, right);
    REQUIRE(left != NULL && right != NULL && result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;
    return finish_plan(function, semantic_out, profile_out);
}

static XrTargetPlan *build_multi_function_plan(
    XrSemanticPlan **semantic_out, XrTargetProfile **profile_out) {
    XiFunc *root = xi_func_new("generation_multi_root", &stub_int);
    XiFunc *child = xi_func_new("generation_multi_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *root_result = xi_const_int(root, root_entry, 7, &stub_int);
    XiValue *child_result = xi_const_int(child, child_entry, 9, &stub_int);
    REQUIRE(root_result != NULL && child_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_calloc(1, sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->parent_func = root;
    root->stage = child->stage = XI_STAGE_OPTIMIZED;
    return finish_plan(root, semantic_out, profile_out);
}

static XrRuntimeGenerationBudget make_budget(uint32_t generations) {
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = generations,
        .max_total_pins = 32,
        .max_pins_per_generation = 8,
        .max_pins_by_kind = {8, 4, 4, 4, 4},
    };
    return budget;
}

static void test_scalar_generation_lifecycle(XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(2);
    budget.max_pins_by_kind[XR_MODULE_GENERATION_INFLIGHT_CALL] = 1;
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));

    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, plan, &generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(generation != NULL);
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    XrModuleGenerationSnapshot before;
    XrModuleGenerationSnapshot after;
    REQUIRE(xr_module_generation_snapshot(generation, &before));
    REQUIRE(before.state == XR_MODULE_GENERATION_VERIFIED);
    REQUIRE(before.identity.generation_number == 1);

    REQUIRE(xr_runtime_generation_activation_available());
    int64_t result = 99;
    REQUIRE(!xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == 0 && strstr(diagnostic, "XR_OWN_3003") != NULL);
    REQUIRE(xr_module_generation_prepare(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_READY);
    REQUIRE(xr_module_generation_activate(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == 42);
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_ACTIVE &&
            after.total_pins == 0 &&
            after.pins_by_kind[XR_MODULE_GENERATION_INFLIGHT_CALL] == 0);

    REQUIRE(xr_module_generation_pin_acquire(
        generation, XR_MODULE_GENERATION_INFLIGHT_CALL, diagnostic,
        sizeof(diagnostic)));
    result = 99;
    REQUIRE(!xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == 0 && strstr(diagnostic, "XR_EXEC_5003") != NULL);
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.total_pins == 1 &&
            after.pins_by_kind[XR_MODULE_GENERATION_INFLIGHT_CALL] == 1);
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic,
                                             sizeof(diagnostic)));
    result = 99;
    REQUIRE(!xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    REQUIRE(result == 0 && strstr(diagnostic, "XR_OWN_3003") != NULL);
    REQUIRE(!xr_module_generation_retire(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(xr_module_generation_pin_release(
        generation, XR_MODULE_GENERATION_INFLIGHT_CALL, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(generation == NULL);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority == NULL);
}

static void test_unsupported_generation_remains_verified(
    XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(2);
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, plan, &generation, diagnostic, sizeof(diagnostic)));
    XrModuleGenerationSnapshot before;
    XrModuleGenerationSnapshot after;
    REQUIRE(xr_module_generation_snapshot(generation, &before));
    REQUIRE(before.state == XR_MODULE_GENERATION_VERIFIED);
    REQUIRE(!xr_module_generation_prepare(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5004") != NULL);
    REQUIRE(!xr_module_generation_activate(generation, diagnostic,
                                           sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL);
    REQUIRE(!xr_module_generation_pin_acquire(
        generation, XR_MODULE_GENERATION_INFLIGHT_CALL, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_OWN_3003") != NULL);
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);

    xr_mutex_lock(&authority->gate);
    generation->state = XR_MODULE_GENERATION_READY;
    xr_mutex_unlock(&authority->gate);
    REQUIRE(!xr_module_generation_verify(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5004") != NULL);
    xr_mutex_lock(&authority->gate);
    generation->state = XR_MODULE_GENERATION_VERIFIED;
    xr_mutex_unlock(&authority->gate);

    uint8_t poison[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE] = {0};
    REQUIRE(!xr_module_generation_poison(generation, poison, diagnostic,
                                         sizeof(diagnostic)));
    poison[0] = 0xa7;
    poison[31] = 0x5c;
    REQUIRE(xr_module_generation_poison(generation, poison, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(!xr_module_generation_prepare(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5005") != NULL);
    REQUIRE(xr_module_generation_rollback(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_RETIRED);
    REQUIRE(after.poisoned == 1 && after.rollback_requested == 1);
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(generation == NULL);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(authority == NULL);
}

static void test_divide_by_zero_generation_fails_at_execution(
    XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(1);
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, plan, &generation, diagnostic, sizeof(diagnostic)));
    /* The row is verified and the generation is eligible: nothing here objects
     * to the program before it runs. */
    REQUIRE(xr_module_generation_prepare(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(generation, diagnostic,
                                          sizeof(diagnostic)));
    int64_t result = 99;
    REQUIRE(!xr_module_generation_execute_sole_scalar_i64(
        generation, &result, diagnostic, sizeof(diagnostic)));
    /* A program fault, distinct from the authority and verification codes, and
     * it yields no value. */
    REQUIRE(result == 0 && strstr(diagnostic, "XR_EXEC_5009") != NULL);
    XrModuleGenerationSnapshot after;
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_ACTIVE &&
            after.total_pins == 0);
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
}

static void seed_snapshot(XrModuleGenerationSnapshot *snapshot,
                          XrModuleGenerationState state, uint64_t revision) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->identity.schema_version =
        XR_RUNTIME_GENERATION_SCHEMA_VERSION;
    snapshot->identity.target_plan_schema_version = 1;
    snapshot->identity.generation_number = 7;
    snapshot->identity.completed_family_mask = 1;
    snapshot->identity.required_capability_mask = 3;
    for (size_t i = 0;
         i < sizeof(snapshot->identity.generation_fingerprint); i++)
        snapshot->identity.generation_fingerprint[i] = (uint8_t) (i + 1u);
    snapshot->state = state;
    snapshot->revision = revision;
}

static void expect_transition_rejected(
    const XrModuleGenerationSnapshot *before,
    const XrModuleGenerationSnapshot *after,
    XrModuleGenerationMutation mutation, XrModuleGenerationPinKind pin_kind) {
    char diagnostic[256] = {0};
    REQUIRE(!xr_module_generation_verify_transition(
        before, after, mutation, pin_kind, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5005") != NULL);
}

static void test_independent_state_machine_verifier(void) {
    char diagnostic[256] = {0};
    XrModuleGenerationSnapshot before;
    XrModuleGenerationSnapshot after;
    seed_snapshot(&before, XR_MODULE_GENERATION_LOADING, 1);
    after = before;
    after.state = XR_MODULE_GENERATION_VERIFIED;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_VERIFY,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));

    before = after;
    after.state = XR_MODULE_GENERATION_READY;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_PREPARE,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));
    before = after;
    after.state = XR_MODULE_GENERATION_ACTIVE;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_ACTIVATE,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));

    before = after;
    after.total_pins++;
    after.pins_by_kind[XR_MODULE_GENERATION_CALLBACK]++;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
        XR_MODULE_GENERATION_CALLBACK, diagnostic, sizeof(diagnostic)));
    before = after;
    after.state = XR_MODULE_GENERATION_DRAINING;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_BEGIN_DRAIN,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));
    before = after;
    after.total_pins--;
    after.pins_by_kind[XR_MODULE_GENERATION_CALLBACK]--;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_PIN_RELEASE,
        XR_MODULE_GENERATION_CALLBACK, diagnostic, sizeof(diagnostic)));
    before = after;
    after.state = XR_MODULE_GENERATION_RETIRED;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_RETIRE,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));
    before = after;
    after.state = XR_MODULE_GENERATION_UNLOADED;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_UNLOAD,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));

    XrModuleGenerationSnapshot invalid = after;
    invalid.identity.generation_number++;
    expect_transition_rejected(&before, &invalid,
                               XR_MODULE_GENERATION_MUTATION_UNLOAD,
                               XR_MODULE_GENERATION_PIN);
    invalid = after;
    invalid.revision++;
    expect_transition_rejected(&before, &invalid,
                               XR_MODULE_GENERATION_MUTATION_UNLOAD,
                               XR_MODULE_GENERATION_PIN);
    invalid = before;
    invalid.state = XR_MODULE_GENERATION_ACTIVE;
    invalid.revision++;
    expect_transition_rejected(&before, &invalid,
                               XR_MODULE_GENERATION_MUTATION_UNLOAD,
                               XR_MODULE_GENERATION_PIN);

    seed_snapshot(&before, XR_MODULE_GENERATION_VERIFIED, 11);
    after = before;
    after.poisoned = 1;
    after.poison_fingerprint[3] = 1;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_POISON,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));
    before = after;
    after.state = XR_MODULE_GENERATION_RETIRED;
    after.rollback_requested = 1;
    after.revision++;
    REQUIRE(xr_module_generation_verify_transition(
        &before, &after, XR_MODULE_GENERATION_MUTATION_ROLLBACK,
        XR_MODULE_GENERATION_PIN, diagnostic, sizeof(diagnostic)));
}

static void test_actual_identity_mutations(XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(1);
    XrRuntimeGenerationAuthority *authority = NULL;
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_load_verified_target_plan(
        authority, plan, &generation, diagnostic, sizeof(diagnostic)));

    xr_mutex_lock(&authority->gate);
    generation->identity.generation_fingerprint[0] ^= 1u;
    xr_mutex_unlock(&authority->gate);
    REQUIRE(!xr_module_generation_verify(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5008") != NULL);
    xr_mutex_lock(&authority->gate);
    generation->identity.generation_fingerprint[0] ^= 1u;
    xr_mutex_unlock(&authority->gate);
    REQUIRE(xr_module_generation_verify(generation, diagnostic,
                                        sizeof(diagnostic)));
    xr_mutex_lock(&authority->gate);
    generation->total_pins = 1;
    xr_mutex_unlock(&authority->gate);
    REQUIRE(!xr_module_generation_verify(generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5008") != NULL);
    xr_mutex_lock(&authority->gate);
    generation->total_pins = 0;
    xr_mutex_unlock(&authority->gate);
    REQUIRE(xr_module_generation_rollback(generation, diagnostic,
                                          sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
}

typedef struct LoadContext {
    XrRuntimeGenerationAuthority *authority;
    XrTargetPlan *plan;
    XrLoadedModuleGeneration *generation;
    bool loaded;
} LoadContext;

static void *load_generation_thread(void *opaque) {
    LoadContext *context = (LoadContext *) opaque;
    char diagnostic[256] = {0};
    context->loaded = xr_module_generation_load_verified_target_plan(
        context->authority, context->plan, &context->generation, diagnostic,
        sizeof(diagnostic));
    return NULL;
}

static void test_concurrent_generation_budget(XrTargetPlan *plan) {
    enum { THREAD_COUNT = 12, GENERATION_LIMIT = 4 };
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(GENERATION_LIMIT);
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &authority, diagnostic, sizeof(diagnostic)));
    LoadContext contexts[THREAD_COUNT] = {0};
    xr_thread_t threads[THREAD_COUNT] = {0};
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        contexts[i].authority = authority;
        contexts[i].plan = plan;
        REQUIRE(xr_thread_create(&threads[i], load_generation_thread,
                                 &contexts[i]));
    }
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        loaded += contexts[i].loaded ? 1u : 0u;
    }
    REQUIRE(loaded == GENERATION_LIMIT);
    for (uint32_t i = 0; i < THREAD_COUNT; i++) {
        if (!contexts[i].generation)
            continue;
        REQUIRE(xr_module_generation_rollback(contexts[i].generation,
                                              diagnostic,
                                              sizeof(diagnostic)));
        REQUIRE(xr_module_generation_unload(&contexts[i].generation,
                                            diagnostic,
                                            sizeof(diagnostic)));
    }
    REQUIRE(xr_runtime_generation_authority_destroy(
        &authority, diagnostic, sizeof(diagnostic)));
}

int main(void) {
    XrSemanticPlan *semantic = NULL;
    XrTargetProfile *profile = NULL;
    XrTargetPlan *plan = build_plan(&semantic, &profile);
    XrSemanticPlan *unsupported_semantic = NULL;
    XrTargetProfile *unsupported_profile = NULL;
    XrTargetPlan *unsupported = build_unsupported_plan(
        &unsupported_semantic, &unsupported_profile);
    XrSemanticPlan *zero_semantic = NULL;
    XrTargetProfile *zero_profile = NULL;
    XrTargetPlan *zero = build_divide_by_zero_plan(&zero_semantic,
                                                   &zero_profile);
    XrSemanticPlan *multi_semantic = NULL;
    XrTargetProfile *multi_profile = NULL;
    XrTargetPlan *multi = build_multi_function_plan(
        &multi_semantic, &multi_profile);
    uint32_t function_count = 0;
    REQUIRE(xr_target_plan_function_execution_family_mask(unsupported, 0) == 0);
    REQUIRE(xr_target_plan_function_execution_family_mask(zero, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    REQUIRE(xr_target_plan_functions(multi, &function_count) != NULL &&
            function_count == 2);
    REQUIRE(xr_target_plan_function_execution_family_mask(multi, 0) ==
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED);
    test_scalar_generation_lifecycle(plan);
    test_unsupported_generation_remains_verified(unsupported);
    test_unsupported_generation_remains_verified(multi);
    test_divide_by_zero_generation_fails_at_execution(zero);
    test_independent_state_machine_verifier();
    test_actual_identity_mutations(plan);
    test_concurrent_generation_budget(plan);
    xr_target_plan_free(plan);
    xr_target_profile_free(profile);
    xr_semantic_plan_free(semantic);
    xr_target_plan_free(unsupported);
    xr_target_profile_free(unsupported_profile);
    xr_semantic_plan_free(unsupported_semantic);
    xr_target_plan_free(zero);
    xr_target_profile_free(zero_profile);
    xr_semantic_plan_free(zero_semantic);
    xr_target_plan_free(multi);
    xr_target_profile_free(multi_profile);
    xr_semantic_plan_free(multi_semantic);
    puts("runtime generation authority tests passed");
    return 0;
}
