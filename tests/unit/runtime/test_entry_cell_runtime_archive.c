/*
 * test_entry_cell_runtime_archive.c - Runtime-only typed entry-cell boundary
 *
 * This executable links only xray_vm. It loads exact artifact bytes, creates
 * two active generations, and proves that a cell can swap between them without
 * leaking its published or in-flight pins.
 */

#include "runtime_scalar_artifacts.h"
#include "runtime/xr_entry_cell.h"
#include "xray_target_plan_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "requirement failed at %s:%d: %s (%s)\n",         \
                    __FILE__, __LINE__, #condition, diagnostic);               \
            abort();                                                           \
        }                                                                      \
    } while (0)

typedef struct Fixture {
    XrRuntimeArtifactAuthority *artifact;
    XrTargetPlan *plan;
    XrRuntimeGenerationAuthority *authority;
    XrLoadedModuleGeneration *first;
    XrLoadedModuleGeneration *second;
    XrEntryCell cell;
} Fixture;

static char diagnostic[512];

static XrRuntimeGenerationBudget make_budget(void) {
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 2,
        .max_total_pins = 16,
        .max_pins_per_generation = 8,
        .max_pins_by_kind = {4, 4, 4, 4, 4},
    };
    return budget;
}

static void activate_generation(Fixture *fixture,
                                XrLoadedModuleGeneration **generation) {
    REQUIRE(xr_module_generation_load_verified_target_plan(
        fixture->authority, fixture->plan, generation, diagnostic,
        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(*generation, diagnostic,
                                         sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(*generation, diagnostic,
                                          sizeof(diagnostic)));
}

static void fixture_init(Fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    REQUIRE(xr_runtime_artifact_authority_load_xsm(
        xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
        &fixture->artifact, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_target_plan_load(
        xr_runtime_scalar_xtp, sizeof(xr_runtime_scalar_xtp),
        fixture->artifact, &fixture->plan, diagnostic, sizeof(diagnostic)));
    XrRuntimeGenerationBudget budget = make_budget();
    REQUIRE(xr_runtime_generation_authority_create(
        &budget, &fixture->authority, diagnostic, sizeof(diagnostic)));
    activate_generation(fixture, &fixture->first);
    activate_generation(fixture, &fixture->second);
    REQUIRE(xr_entry_cell_init(&fixture->cell));
}

static XrEntryCellRegistration vm_registration(
    Fixture *fixture, XrLoadedModuleGeneration *generation) {
    XrEntryCellRegistration registration = {
        .generation = generation,
        .verified_plan = fixture->plan,
        .function = 0,
        .executor_kind = XR_ENTRY_EXECUTOR_TYPED_VM,
    };
    return registration;
}

static void require_pin_counts(XrLoadedModuleGeneration *generation,
                               uint32_t total, uint32_t calls,
                               uint32_t roots) {
    XrModuleGenerationSnapshot snapshot;
    REQUIRE(xr_module_generation_snapshot(generation, &snapshot));
    REQUIRE(snapshot.total_pins == total);
    REQUIRE(snapshot.pins_by_kind[XR_MODULE_GENERATION_INFLIGHT_CALL] ==
            calls);
    REQUIRE(snapshot.pins_by_kind[XR_MODULE_GENERATION_STATIC_ROOT] == roots);
}

static void require_expectation_rejected(
    XrEntryCell *cell, const XrEntryCellExpectation *expectation,
    XrLoadedModuleGeneration *generation) {
    XrEntryCallToken token;
    memset(diagnostic, 0, sizeof(diagnostic));
    REQUIRE(!xr_entry_cell_acquire(cell, expectation, &token, diagnostic,
                                   sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5008") != NULL);
    require_pin_counts(generation, 1, 0, 1);
}

static void test_vm_binding_and_seeded_mismatches(Fixture *fixture,
                                                  XrEntryCellExpectation *out) {
    XrEntryCellRegistration registration =
        vm_registration(fixture, fixture->first);
    REQUIRE(xr_entry_cell_bind(&fixture->cell, &registration, out, diagnostic,
                               sizeof(diagnostic)));
    require_pin_counts(fixture->first, 1, 0, 1);

    int64_t result = 0;
    uint32_t executor_status = UINT32_MAX;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, out, NULL, 0, &result, &executor_status,
                diagnostic, sizeof(diagnostic)) == XR_ENTRY_INVOKE_OK);
    REQUIRE(result == 42 && executor_status == 0);
    require_pin_counts(fixture->first, 1, 0, 1);

    XrEntryCellExpectation mutated = *out;
    mutated.abi.fingerprint.bytes[0] ^= 1u;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.adapter_fingerprint.bytes[0] ^= 1u;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.target_plan_fingerprint.bytes[0] ^= 1u;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.generation_fingerprint.bytes[0] ^= 1u;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.binding_fingerprint.bytes[0] ^= 1u;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.executor_kind = XR_ENTRY_EXECUTOR_NATIVE_I64;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
    mutated = *out;
    mutated.adapter_kind = XR_ENTRY_ADAPTER_INVALID;
    require_expectation_rejected(&fixture->cell, &mutated, fixture->first);
}

static void test_token_release_is_exactly_once(
    Fixture *fixture, const XrEntryCellExpectation *expectation) {
    XrEntryCallToken token;
    REQUIRE(xr_entry_cell_acquire(&fixture->cell, expectation, &token,
                                  diagnostic, sizeof(diagnostic)));
    require_pin_counts(fixture->first, 2, 1, 1);
    REQUIRE(xr_entry_call_release(&token, diagnostic, sizeof(diagnostic)));
    require_pin_counts(fixture->first, 1, 0, 1);
    REQUIRE(!xr_entry_call_release(&token, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_OWN_3003") != NULL);
    require_pin_counts(fixture->first, 1, 0, 1);
}

typedef struct NativeContext {
    XrEntryNativeStatus status;
    int64_t value;
    uint32_t calls;
} NativeContext;

static XrEntryNativeStatus native_entry(void *opaque, const int64_t *arguments,
                                        uint32_t argument_count,
                                        int64_t *result) {
    NativeContext *context = (NativeContext *) opaque;
    if (!context || arguments || argument_count != 0 || !result)
        return XR_ENTRY_NATIVE_ERROR;
    context->calls++;
    *result = context->value;
    return context->status;
}

static XrEntryCellRegistration native_registration(Fixture *fixture,
                                                   NativeContext *context) {
    XrEntryCellRegistration registration = {
        .generation = fixture->second,
        .verified_plan = fixture->plan,
        .function = 0,
        .executor_kind = XR_ENTRY_EXECUTOR_NATIVE_I64,
        .native_entry = native_entry,
        .native_context = context,
    };
    for (size_t i = 0; i < sizeof(registration.native_entry_identity.bytes);
         i++)
        registration.native_entry_identity.bytes[i] = (uint8_t) (0x80u + i);
    return registration;
}

static void test_native_success_error_cancel_release(
    Fixture *fixture, const XrEntryCellExpectation *stale) {
    NativeContext context = {.status = XR_ENTRY_NATIVE_OK, .value = 77};
    XrEntryCellRegistration registration =
        native_registration(fixture, &context);
    XrEntryCellExpectation expected;
    REQUIRE(xr_entry_cell_bind(&fixture->cell, &registration, &expected,
                               diagnostic, sizeof(diagnostic)));
    require_pin_counts(fixture->first, 0, 0, 0);
    require_pin_counts(fixture->second, 1, 0, 1);
    require_expectation_rejected(&fixture->cell, stale, fixture->second);

    int64_t result = 0;
    uint32_t executor_status = UINT32_MAX;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, &expected, NULL, 0, &result,
                &executor_status, diagnostic, sizeof(diagnostic)) ==
            XR_ENTRY_INVOKE_OK);
    REQUIRE(result == 77 && context.calls == 1);
    require_pin_counts(fixture->second, 1, 0, 1);

    context.status = XR_ENTRY_NATIVE_ERROR;
    result = 99;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, &expected, NULL, 0, &result,
                &executor_status, diagnostic, sizeof(diagnostic)) ==
            XR_ENTRY_INVOKE_NATIVE_ERROR);
    REQUIRE(result == 0 && context.calls == 2);
    require_pin_counts(fixture->second, 1, 0, 1);

    context.status = XR_ENTRY_NATIVE_CANCELLED;
    result = 99;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, &expected, NULL, 0, &result,
                &executor_status, diagnostic, sizeof(diagnostic)) ==
            XR_ENTRY_INVOKE_CANCELLED);
    REQUIRE(result == 0 && context.calls == 3);
    require_pin_counts(fixture->second, 1, 0, 1);
}

typedef struct HeldCall {
    XrEntryCell *cell;
    XrEntryCellExpectation expectation;
    XrEntryCallToken token;
    atomic_uint acquired;
    atomic_uint finish;
    bool released;
} HeldCall;

static void *hold_call(void *opaque) {
    HeldCall *held = (HeldCall *) opaque;
    char local_diagnostic[256] = {0};
    if (!xr_entry_cell_acquire(held->cell, &held->expectation, &held->token,
                               local_diagnostic,
                               sizeof(local_diagnostic))) {
        atomic_store_explicit(&held->acquired, 2u, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&held->acquired, 1u, memory_order_release);
    while (!atomic_load_explicit(&held->finish, memory_order_acquire))
        xr_thread_yield();
    held->released = xr_entry_call_release(
        &held->token, local_diagnostic, sizeof(local_diagnostic));
    return NULL;
}

static void wait_for_acquire(HeldCall *held) {
    for (uint32_t i = 0; i < 10000; i++) {
        unsigned int state =
            atomic_load_explicit(&held->acquired, memory_order_acquire);
        if (state != 0) {
            REQUIRE(state == 1);
            return;
        }
        xr_thread_sleep_ms(1);
    }
    REQUIRE(false);
}

static void test_concurrent_swap_drain_and_stale(Fixture *fixture) {
    XrEntryCellRegistration first =
        vm_registration(fixture, fixture->first);
    XrEntryCellRegistration second =
        vm_registration(fixture, fixture->second);
    XrEntryCellExpectation first_expected;
    XrEntryCellExpectation second_expected;
    REQUIRE(xr_entry_cell_bind(&fixture->cell, &first, &first_expected,
                               diagnostic, sizeof(diagnostic)));
    HeldCall held = {.cell = &fixture->cell, .expectation = first_expected};
    atomic_init(&held.acquired, 0);
    atomic_init(&held.finish, 0);
    xr_thread_t thread = {0};
    REQUIRE(xr_thread_create(&thread, hold_call, &held));
    wait_for_acquire(&held);

    REQUIRE(xr_entry_cell_bind(&fixture->cell, &second, &second_expected,
                               diagnostic, sizeof(diagnostic)));
    require_pin_counts(fixture->first, 1, 1, 0);
    require_pin_counts(fixture->second, 1, 0, 1);
    REQUIRE(xr_module_generation_begin_drain(
        fixture->first, diagnostic, sizeof(diagnostic)));
    REQUIRE(!xr_module_generation_retire(fixture->first, diagnostic,
                                         sizeof(diagnostic)));
    atomic_store_explicit(&held.finish, 1u, memory_order_release);
    REQUIRE(xr_thread_join(thread, NULL) == 0 && held.released);
    require_pin_counts(fixture->first, 0, 0, 0);
    REQUIRE(xr_module_generation_retire(fixture->first, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&fixture->first, diagnostic,
                                        sizeof(diagnostic)));

    require_expectation_rejected(&fixture->cell, &first_expected,
                                 fixture->second);
    int64_t result = 0;
    uint32_t executor_status = UINT32_MAX;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, &second_expected, NULL, 0, &result,
                &executor_status, diagnostic, sizeof(diagnostic)) ==
            XR_ENTRY_INVOKE_OK);
    REQUIRE(result == 42);

    REQUIRE(xr_module_generation_begin_drain(
        fixture->second, diagnostic, sizeof(diagnostic)));
    result = 99;
    REQUIRE(xr_entry_cell_invoke_i64(
                &fixture->cell, &second_expected, NULL, 0, &result,
                &executor_status, diagnostic, sizeof(diagnostic)) ==
            XR_ENTRY_INVOKE_AUTHORITY_ERROR);
    REQUIRE(result == 0 && strstr(diagnostic, "XR_OWN_3003") != NULL);
    require_pin_counts(fixture->second, 1, 0, 1);
    REQUIRE(xr_entry_cell_clear(&fixture->cell, diagnostic,
                                sizeof(diagnostic)));
    require_pin_counts(fixture->second, 0, 0, 0);
    REQUIRE(xr_entry_cell_dispose(&fixture->cell, diagnostic,
                                  sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(fixture->second, diagnostic,
                                        sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&fixture->second, diagnostic,
                                        sizeof(diagnostic)));
}

static void fixture_dispose(Fixture *fixture) {
    REQUIRE(!fixture->first && !fixture->second);
    REQUIRE(xr_runtime_generation_authority_destroy(
        &fixture->authority, diagnostic, sizeof(diagnostic)));
    xr_target_plan_free(fixture->plan);
    xr_runtime_artifact_authority_free(fixture->artifact);
}

int main(void) {
    Fixture fixture;
    fixture_init(&fixture);
    XrEntryCellExpectation first;
    test_vm_binding_and_seeded_mismatches(&fixture, &first);
    test_token_release_is_exactly_once(&fixture, &first);
    test_native_success_error_cancel_release(&fixture, &first);
    test_concurrent_swap_drain_and_stale(&fixture);
    fixture_dispose(&fixture);
    puts("runtime entry-cell archive boundary passed");
    return 0;
}
