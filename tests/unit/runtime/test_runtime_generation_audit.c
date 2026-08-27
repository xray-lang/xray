/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_generation_audit.c - Independent audit of a real lifecycle
 *
 * KEY CONCEPT:
 *   The state-machine verifier models every legal mutation on its own and
 *   never calls the production transition helper. Feeding it the snapshot pair
 *   each real mutation produced makes it a second opinion on the running
 *   lifecycle rather than a checker of synthetic pairs. It is handed no field
 *   that states the answer: it re-derives from state, revision, flags, and pin
 *   counts which mutation could have produced the pair.
 */

#include "test_runtime_generation_audit.h"

#include "../../../include/xray_runtime_generation.h"
#include "../../../src/plan/target/xr_target_plan.h"
#include "../../../src/runtime/xr_module_generation_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrRuntimeGenerationBudget audit_budget(void) {
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 1,
        .max_total_pins = 32,
        .max_pins_per_generation = 8,
        .max_pins_by_kind = {8, 4, 4, 4, 4},
    };
    return budget;
}

/* One mutation, verified twice: once by the lifecycle owner that performed it
 * and once by the verifier that models it independently. */
typedef struct AuditedStep {
    XrModuleGenerationSnapshot before;
    XrModuleGenerationSnapshot after;
} AuditedStep;

static void step_begin(const XrLoadedModuleGeneration *generation, AuditedStep *step) {
    REQUIRE(xr_module_generation_snapshot(generation, &step->before));
}

static void step_end(const XrLoadedModuleGeneration *generation, AuditedStep *step,
                     XrModuleGenerationMutation mutation, XrModuleGenerationPinKind pin_kind) {
    char diagnostic[512] = {0};
    REQUIRE(xr_module_generation_snapshot(generation, &step->after));
    if (!xr_module_generation_verify_transition(&step->before, &step->after, mutation, pin_kind,
                                                diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "independent verifier rejected a real mutation %d: %s\n", (int) mutation,
                diagnostic);
        abort();
    }
}

/* Every mutation the public surface can bracket with two snapshots. LOADING ->
 * VERIFIED happens inside the load call and UNLOADED is only reached after the
 * handle is freed, so neither pair is observable from here; the terminal facts
 * for those two are asserted directly instead. */
static void test_real_lifecycle_is_independently_verified(const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = audit_budget();
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(&budget, &authority, diagnostic,
                                                   sizeof(diagnostic)));
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(authority, plan, &generation, diagnostic,
                                                           sizeof(diagnostic)));
    AuditedStep step;

    step_begin(generation, &step);
    REQUIRE(xr_module_generation_prepare(generation, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_PREPARE, XR_MODULE_GENERATION_PIN);

    step_begin(generation, &step);
    REQUIRE(xr_module_generation_activate(generation, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_ACTIVATE, XR_MODULE_GENERATION_PIN);

    for (uint32_t kind = 0; kind < XR_MODULE_GENERATION_PIN_KIND_COUNT; kind++) {
        step_begin(generation, &step);
        REQUIRE(xr_module_generation_pin_acquire(generation, (XrModuleGenerationPinKind) kind,
                                                 diagnostic, sizeof(diagnostic)));
        step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
                 (XrModuleGenerationPinKind) kind);
    }
    for (uint32_t kind = 0; kind < XR_MODULE_GENERATION_PIN_KIND_COUNT; kind++) {
        step_begin(generation, &step);
        REQUIRE(xr_module_generation_pin_release(generation, (XrModuleGenerationPinKind) kind,
                                                 diagnostic, sizeof(diagnostic)));
        step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_PIN_RELEASE,
                 (XrModuleGenerationPinKind) kind);
    }

    step_begin(generation, &step);
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_BEGIN_DRAIN,
             XR_MODULE_GENERATION_PIN);

    step_begin(generation, &step);
    REQUIRE(xr_module_generation_retire(generation, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_RETIRE, XR_MODULE_GENERATION_PIN);

    REQUIRE(authority->live_generations == 1);
    REQUIRE(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(generation == NULL);
    REQUIRE(authority->live_generations == 0);
    REQUIRE(xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    puts("  real lifecycle is independently verified");
}

/* POISON and ROLLBACK are the failure path, so they get their own run: both
 * are legal only before RETIRED and both must keep the pin ledger intact. */
static void test_real_failure_path_is_independently_verified(const XrTargetPlan *plan) {
    static const uint8_t poison[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE] = {0xA5, 0x5A, 0x11};
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = audit_budget();
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(&budget, &authority, diagnostic,
                                                   sizeof(diagnostic)));
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(authority, plan, &generation, diagnostic,
                                                           sizeof(diagnostic)));
    AuditedStep step;

    step_begin(generation, &step);
    REQUIRE(xr_module_generation_poison(generation, poison, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_POISON, XR_MODULE_GENERATION_PIN);

    /* A poisoned generation cannot become ready, so rollback is the only way
     * out and it must land in RETIRED without ever having been active. */
    REQUIRE(!xr_module_generation_prepare(generation, diagnostic, sizeof(diagnostic)));
    step_begin(generation, &step);
    REQUIRE(xr_module_generation_rollback(generation, diagnostic, sizeof(diagnostic)));
    step_end(generation, &step, XR_MODULE_GENERATION_MUTATION_ROLLBACK, XR_MODULE_GENERATION_PIN);
    REQUIRE(step.after.state == XR_MODULE_GENERATION_RETIRED);

    REQUIRE(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    puts("  real failure path is independently verified");
}

/* Hostile mutation of the snapshot pair a real mutation produced. The pairs are
 * genuine - each comes from a lifecycle call that succeeded - so a rejection
 * can only come from the verifier re-deriving the transition, never from a
 * field that carries the answer. */
static void test_hostile_snapshot_mutations_are_rejected(const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = audit_budget();
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(&budget, &authority, diagnostic,
                                                   sizeof(diagnostic)));
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(authority, plan, &generation, diagnostic,
                                                           sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_activate(generation, diagnostic, sizeof(diagnostic)));

    AuditedStep pin;
    step_begin(generation, &pin);
    REQUIRE(xr_module_generation_pin_acquire(generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
                                             diagnostic, sizeof(diagnostic)));
    step_end(generation, &pin, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
             XR_MODULE_GENERATION_INFLIGHT_CALL);

#define REJECTS(before_snapshot, after_snapshot, mutation, kind)                                   \
    REQUIRE(!xr_module_generation_verify_transition((before_snapshot), (after_snapshot),           \
                                                    (mutation), (kind), diagnostic,                \
                                                    sizeof(diagnostic)) &&                         \
            strstr(diagnostic, "XR_EXEC_5005") != NULL)

    /* Naming a different pin kind than the one the counters moved. */
    REJECTS(&pin.before, &pin.after, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_CALLBACK);
    /* Naming the opposite direction. */
    REJECTS(&pin.before, &pin.after, XR_MODULE_GENERATION_MUTATION_PIN_RELEASE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);
    /* Replaying the pair backwards. */
    REJECTS(&pin.after, &pin.before, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);
    /* Claiming a state transition that never happened. */
    REJECTS(&pin.before, &pin.after, XR_MODULE_GENERATION_MUTATION_BEGIN_DRAIN,
            XR_MODULE_GENERATION_PIN);

    /* A pin that moved only the authority-wide total and not its own kind. */
    XrModuleGenerationSnapshot forged = pin.after;
    forged.pins_by_kind[XR_MODULE_GENERATION_INFLIGHT_CALL]--;
    forged.pins_by_kind[XR_MODULE_GENERATION_CALLBACK]++;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);

    /* A mutation that skipped a revision, and one that never recorded it. */
    forged = pin.after;
    forged.revision++;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);
    forged = pin.after;
    forged.revision = pin.before.revision;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);

    /* A mutation that changed the immutable identity it was supposed to carry. */
    forged = pin.after;
    forged.identity.generation_fingerprint[0] ^= 0xFFu;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);
    forged = pin.after;
    forged.identity.generation_number++;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);

    /* A total that no per-kind ledger adds up to. */
    forged = pin.after;
    forged.total_pins++;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);

    /* Poisoned without a diagnostic fingerprint, and a fingerprint without the
     * flag - the two are one fact and the verifier owns both directions. */
    forged = pin.after;
    forged.poisoned = 1u;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);
    forged = pin.after;
    forged.poison_fingerprint[0] = 0x7Fu;
    REJECTS(&pin.before, &forged, XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE,
            XR_MODULE_GENERATION_INFLIGHT_CALL);

#undef REJECTS

    /* The forgeries changed nothing real: the generation still owns the pin it
     * took and finishes its lifecycle normally. */
    XrModuleGenerationSnapshot live;
    REQUIRE(xr_module_generation_snapshot(generation, &live));
    REQUIRE(memcmp(&live, &pin.after, sizeof(live)) == 0);
    REQUIRE(xr_module_generation_pin_release(generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
                                             diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    puts("  hostile snapshot mutations are rejected");
}

void run_generation_lifecycle_audit(const XrTargetPlan *plan) {
    puts("generation lifecycle independent audit:");
    test_real_lifecycle_is_independently_verified(plan);
    test_real_failure_path_is_independently_verified(plan);
    test_hostile_snapshot_mutations_are_rejected(plan);
}
