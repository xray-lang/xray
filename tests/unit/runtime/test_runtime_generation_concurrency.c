/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_generation_concurrency.c - Generation lifecycle race gates
 *
 * KEY CONCEPT:
 *   Each case names one linearization point and the exact set of outcomes it
 *   admits. Threads meet at a reusable barrier instead of sleeping, so an
 *   admitted-outcome violation reproduces on the iteration that first showed
 *   it rather than on a timing accident.
 */

#include "test_runtime_generation_concurrency.h"

#include "../../../include/xray_runtime_generation.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/os/os_thread.h"
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

#define REQUIRE_AT(condition, iteration)                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d on iteration %u: %s\n", __FILE__,         \
                    __LINE__, (unsigned) (iteration), #condition);                                 \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Iterations per race. The barrier decides the interleaving, so this only
 * widens the window the scheduler is asked to place the two sides in. */
#define RACE_ITERATIONS 64u
#define PROBE_RESULT 42

/* ========== reusable barrier ========== */

typedef struct Barrier {
    xr_mutex_t gate;
    xr_cond_t signal;
    uint32_t required;
    uint32_t arrived;
    uint32_t epoch;
} Barrier;

static void barrier_init(Barrier *barrier, uint32_t required) {
    xr_mutex_init(&barrier->gate);
    xr_cond_init(&barrier->signal);
    barrier->required = required;
    barrier->arrived = 0;
    barrier->epoch = 0;
}

static void barrier_destroy(Barrier *barrier) {
    xr_cond_destroy(&barrier->signal);
    xr_mutex_destroy(&barrier->gate);
}

/* Releases every participant of one epoch together. The epoch counter makes
 * the barrier reusable without a second barrier to separate rounds. */
static void barrier_wait(Barrier *barrier) {
    xr_mutex_lock(&barrier->gate);
    uint32_t epoch = barrier->epoch;
    barrier->arrived++;
    if (barrier->arrived == barrier->required) {
        barrier->arrived = 0;
        barrier->epoch++;
        xr_cond_broadcast(&barrier->signal);
    } else {
        while (epoch == barrier->epoch)
            xr_cond_wait(&barrier->signal, &barrier->gate);
    }
    xr_mutex_unlock(&barrier->gate);
}

/* ========== fixture ========== */

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

static XrRuntimeGenerationAuthority *make_authority(uint32_t generations) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationBudget budget = make_budget(generations);
    XrRuntimeGenerationAuthority *authority = NULL;
    REQUIRE(xr_runtime_generation_authority_create(&budget, &authority, diagnostic,
                                                   sizeof(diagnostic)));
    return authority;
}

static XrLoadedModuleGeneration *make_ready(XrRuntimeGenerationAuthority *authority,
                                            const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrLoadedModuleGeneration *generation = NULL;
    REQUIRE(xr_module_generation_load_verified_target_plan(authority, plan, &generation, diagnostic,
                                                           sizeof(diagnostic)));
    REQUIRE(xr_module_generation_prepare(generation, diagnostic, sizeof(diagnostic)));
    return generation;
}

static XrLoadedModuleGeneration *make_active(XrRuntimeGenerationAuthority *authority,
                                             const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrLoadedModuleGeneration *generation = make_ready(authority, plan);
    REQUIRE(xr_module_generation_activate(generation, diagnostic, sizeof(diagnostic)));
    return generation;
}

/* Takes a generation in any pre-retired state to UNLOADED so an authority can
 * be destroyed. Rollback is the lifecycle's own failure path, so this adds no
 * second teardown owner. */
static void discard_generation(XrLoadedModuleGeneration **generation) {
    char diagnostic[512] = {0};
    XrModuleGenerationSnapshot snapshot;
    REQUIRE(xr_module_generation_snapshot(*generation, &snapshot));
    if (snapshot.state < XR_MODULE_GENERATION_RETIRED)
        REQUIRE(xr_module_generation_rollback(*generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_snapshot(*generation, &snapshot));
    if (snapshot.state == XR_MODULE_GENERATION_DRAINING)
        REQUIRE(xr_module_generation_retire(*generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(xr_module_generation_unload(generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(*generation == NULL);
}

static void destroy_authority(XrRuntimeGenerationAuthority **authority) {
    char diagnostic[512] = {0};
    REQUIRE(xr_runtime_generation_authority_destroy(authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(*authority == NULL);
}

/* ========== execute pin vs begin_drain ==========
 *
 * Linearization point: the ACTIVE -> DRAINING store inside begin_drain, taken
 * under the authority gate. Admitted outcomes for one execute attempt are
 * exactly two: it acquired its in-flight pin before that store and ran to a
 * verified result, or it was refused a pin after it. A pin acquired before the
 * store and a generation retired underneath it is not an admitted outcome, and
 * neither is a partial result. */

#define EXECUTE_ATTEMPTS 128u

typedef struct DrainRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    uint32_t executed;
    uint32_t refused;
    bool wrong_result;
    bool drained;
} DrainRace;

static void *drain_race_executor(void *opaque) {
    DrainRace *race = (DrainRace *) opaque;
    barrier_wait(race->barrier);
    for (uint32_t attempt = 0; attempt < EXECUTE_ATTEMPTS; attempt++) {
        int64_t result = 0;
        char diagnostic[256] = {0};
        if (xr_module_generation_execute_sole_scalar_i64(race->generation, &result, diagnostic,
                                                         sizeof(diagnostic))) {
            race->executed++;
            if (result != PROBE_RESULT)
                race->wrong_result = true;
        } else {
            race->refused++;
        }
    }
    return NULL;
}

static void *drain_race_drainer(void *opaque) {
    DrainRace *race = (DrainRace *) opaque;
    char diagnostic[256] = {0};
    barrier_wait(race->barrier);
    race->drained =
        xr_module_generation_begin_drain(race->generation, diagnostic, sizeof(diagnostic));
    return NULL;
}

static void test_execute_pin_versus_begin_drain(const XrTargetPlan *plan) {
    for (uint32_t iteration = 0; iteration < RACE_ITERATIONS; iteration++) {
        char diagnostic[512] = {0};
        XrRuntimeGenerationAuthority *authority = make_authority(1);
        XrLoadedModuleGeneration *generation = make_active(authority, plan);
        Barrier barrier;
        barrier_init(&barrier, 2);
        DrainRace race = {.barrier = &barrier, .generation = generation};
        xr_thread_t executor;
        xr_thread_t drainer;
        REQUIRE_AT(xr_thread_create(&executor, drain_race_executor, &race), iteration);
        REQUIRE_AT(xr_thread_create(&drainer, drain_race_drainer, &race), iteration);
        REQUIRE_AT(xr_thread_join(executor, NULL) == 0, iteration);
        REQUIRE_AT(xr_thread_join(drainer, NULL) == 0, iteration);

        REQUIRE_AT(race.drained, iteration);
        REQUIRE_AT(!race.wrong_result, iteration);
        REQUIRE_AT(race.executed + race.refused == EXECUTE_ATTEMPTS, iteration);

        /* Drain has returned, so the generation owns no pin and admits none. */
        XrModuleGenerationSnapshot snapshot;
        REQUIRE_AT(xr_module_generation_snapshot(generation, &snapshot), iteration);
        REQUIRE_AT(snapshot.state == XR_MODULE_GENERATION_DRAINING, iteration);
        REQUIRE_AT(snapshot.total_pins == 0, iteration);
        REQUIRE_AT(!xr_module_generation_pin_acquire(generation, XR_MODULE_GENERATION_PIN,
                                                     diagnostic, sizeof(diagnostic)),
                   iteration);
        REQUIRE_AT(strstr(diagnostic, "XR_OWN_3003") != NULL, iteration);
        int64_t late = 0;
        REQUIRE_AT(!xr_module_generation_execute_sole_scalar_i64(generation, &late, diagnostic,
                                                                 sizeof(diagnostic)),
                   iteration);
        REQUIRE_AT(authority->total_pins == 0, iteration);

        REQUIRE_AT(xr_module_generation_retire(generation, diagnostic, sizeof(diagnostic)),
                   iteration);
        REQUIRE_AT(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)),
                   iteration);
        destroy_authority(&authority);
        barrier_destroy(&barrier);
    }
    puts("  execute pin versus begin_drain");
}

/* ========== last pin release vs retire ==========
 *
 * Linearization point: the zero-pin observation inside retire, taken under the
 * authority gate. Retire may only publish RETIRED once the last release has
 * landed, so every release the racing thread issues must succeed: a retire that
 * won early would leave a release with no state that accepts it. */

#define HELD_PINS 6u

typedef struct RetireRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    uint32_t released;
    uint32_t release_refused;
    uint32_t retire_attempts;
    uint32_t retire_refused;
    bool retired;
    bool retired_with_pins;
} RetireRace;

static void *retire_race_releaser(void *opaque) {
    RetireRace *race = (RetireRace *) opaque;
    barrier_wait(race->barrier);
    for (uint32_t i = 0; i < HELD_PINS; i++) {
        char diagnostic[256] = {0};
        if (xr_module_generation_pin_release(race->generation, XR_MODULE_GENERATION_PIN, diagnostic,
                                             sizeof(diagnostic)))
            race->released++;
        else
            race->release_refused++;
    }
    return NULL;
}

static void *retire_race_retirer(void *opaque) {
    RetireRace *race = (RetireRace *) opaque;
    barrier_wait(race->barrier);
    while (!race->retired) {
        char diagnostic[256] = {0};
        race->retire_attempts++;
        if (xr_module_generation_retire(race->generation, diagnostic, sizeof(diagnostic))) {
            race->retired = true;
            XrModuleGenerationSnapshot snapshot;
            if (xr_module_generation_snapshot(race->generation, &snapshot) &&
                snapshot.total_pins != 0)
                race->retired_with_pins = true;
        } else {
            race->retire_refused++;
        }
    }
    return NULL;
}

static void test_last_pin_release_versus_retire(const XrTargetPlan *plan) {
    for (uint32_t iteration = 0; iteration < RACE_ITERATIONS; iteration++) {
        char diagnostic[512] = {0};
        XrRuntimeGenerationAuthority *authority = make_authority(1);
        XrLoadedModuleGeneration *generation = make_active(authority, plan);
        for (uint32_t i = 0; i < HELD_PINS; i++)
            REQUIRE_AT(xr_module_generation_pin_acquire(generation, XR_MODULE_GENERATION_PIN,
                                                        diagnostic, sizeof(diagnostic)),
                       iteration);
        REQUIRE_AT(xr_module_generation_begin_drain(generation, diagnostic, sizeof(diagnostic)),
                   iteration);

        Barrier barrier;
        barrier_init(&barrier, 2);
        RetireRace race = {.barrier = &barrier, .generation = generation};
        xr_thread_t releaser;
        xr_thread_t retirer;
        REQUIRE_AT(xr_thread_create(&releaser, retire_race_releaser, &race), iteration);
        REQUIRE_AT(xr_thread_create(&retirer, retire_race_retirer, &race), iteration);
        REQUIRE_AT(xr_thread_join(releaser, NULL) == 0, iteration);
        REQUIRE_AT(xr_thread_join(retirer, NULL) == 0, iteration);

        REQUIRE_AT(race.retired, iteration);
        REQUIRE_AT(!race.retired_with_pins, iteration);
        REQUIRE_AT(race.released == HELD_PINS, iteration);
        REQUIRE_AT(race.release_refused == 0, iteration);
        REQUIRE_AT(authority->total_pins == 0, iteration);

        XrModuleGenerationSnapshot snapshot;
        REQUIRE_AT(xr_module_generation_snapshot(generation, &snapshot), iteration);
        REQUIRE_AT(snapshot.state == XR_MODULE_GENERATION_RETIRED, iteration);
        REQUIRE_AT(snapshot.total_pins == 0, iteration);
        REQUIRE_AT(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)),
                   iteration);
        destroy_authority(&authority);
        barrier_destroy(&barrier);
    }
    puts("  last pin release versus retire");
}

/* ========== two concurrent activation attempts ==========
 *
 * Linearization point: the READY -> ACTIVE store inside activate, taken under
 * the authority gate together with the live-manifest publication. Exactly one
 * caller may observe READY, so exactly one may succeed, and the manifest must
 * name the generation once no matter which one won. */

typedef struct ActivationRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    bool activated;
    char diagnostic[256];
} ActivationRace;

static void *activation_race_worker(void *opaque) {
    ActivationRace *race = (ActivationRace *) opaque;
    barrier_wait(race->barrier);
    race->activated =
        xr_module_generation_activate(race->generation, race->diagnostic, sizeof(race->diagnostic));
    return NULL;
}

static void test_concurrent_activation_admits_one(const XrTargetPlan *plan) {
    for (uint32_t iteration = 0; iteration < RACE_ITERATIONS; iteration++) {
        char diagnostic[512] = {0};
        XrRuntimeGenerationAuthority *authority = make_authority(1);
        XrLoadedModuleGeneration *generation = make_ready(authority, plan);
        Barrier barrier;
        barrier_init(&barrier, 2);
        ActivationRace first = {.barrier = &barrier, .generation = generation};
        ActivationRace second = {.barrier = &barrier, .generation = generation};
        xr_thread_t threads[2];
        REQUIRE_AT(xr_thread_create(&threads[0], activation_race_worker, &first), iteration);
        REQUIRE_AT(xr_thread_create(&threads[1], activation_race_worker, &second), iteration);
        REQUIRE_AT(xr_thread_join(threads[0], NULL) == 0, iteration);
        REQUIRE_AT(xr_thread_join(threads[1], NULL) == 0, iteration);

        REQUIRE_AT(first.activated != second.activated, iteration);
        const ActivationRace *loser = first.activated ? &second : &first;
        REQUIRE_AT(strstr(loser->diagnostic, "XR_ARTIFACT_2004") != NULL, iteration);

        /* One winner means one manifest row and one live active generation. */
        REQUIRE_AT(authority->active_generation_count == 1, iteration);
        XrRuntimeGenerationLiveManifest manifest = {0};
        REQUIRE_AT(xr_runtime_generation_live_manifest_snapshot(generation, &manifest), iteration);
        REQUIRE_AT(manifest.active_generation_count == 1, iteration);
        XrModuleGenerationSnapshot snapshot;
        REQUIRE_AT(xr_module_generation_snapshot(generation, &snapshot), iteration);
        REQUIRE_AT(snapshot.state == XR_MODULE_GENERATION_ACTIVE, iteration);
        REQUIRE_AT(snapshot.total_pins == 0, iteration);

        int64_t result = 0;
        REQUIRE_AT(xr_module_generation_execute_sole_scalar_i64(generation, &result, diagnostic,
                                                                sizeof(diagnostic)),
                   iteration);
        REQUIRE_AT(result == PROBE_RESULT, iteration);
        discard_generation(&generation);
        destroy_authority(&authority);
        barrier_destroy(&barrier);
    }
    puts("  concurrent activation admits one");
}

/* ========== activate vs rollback ==========
 *
 * Linearization point: whichever of the two reaches the authority gate first.
 * Both orders are admitted and both leave the same terminal fact - the
 * generation owns no live manifest row - but by different routes: rollback
 * first refuses the activation outright, activate first is undone by the
 * rollback that follows it. A live manifest row surviving either order is a
 * rollback that did not unwind its own publication. */

typedef struct RollbackRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    bool activated;
    bool rolled_back;
} RollbackRace;

static void *rollback_race_activator(void *opaque) {
    RollbackRace *race = (RollbackRace *) opaque;
    char diagnostic[256] = {0};
    barrier_wait(race->barrier);
    race->activated =
        xr_module_generation_activate(race->generation, diagnostic, sizeof(diagnostic));
    return NULL;
}

static void *rollback_race_rollbacker(void *opaque) {
    RollbackRace *race = (RollbackRace *) opaque;
    char diagnostic[256] = {0};
    barrier_wait(race->barrier);
    race->rolled_back =
        xr_module_generation_rollback(race->generation, diagnostic, sizeof(diagnostic));
    return NULL;
}

static void test_activate_versus_rollback(const XrTargetPlan *plan) {
    for (uint32_t iteration = 0; iteration < RACE_ITERATIONS; iteration++) {
        char diagnostic[512] = {0};
        XrRuntimeGenerationAuthority *authority = make_authority(1);
        XrLoadedModuleGeneration *generation = make_ready(authority, plan);
        Barrier barrier;
        barrier_init(&barrier, 2);
        RollbackRace race = {.barrier = &barrier, .generation = generation};
        xr_thread_t activator;
        xr_thread_t rollbacker;
        REQUIRE_AT(xr_thread_create(&activator, rollback_race_activator, &race), iteration);
        REQUIRE_AT(xr_thread_create(&rollbacker, rollback_race_rollbacker, &race), iteration);
        REQUIRE_AT(xr_thread_join(activator, NULL) == 0, iteration);
        REQUIRE_AT(xr_thread_join(rollbacker, NULL) == 0, iteration);

        REQUIRE_AT(race.rolled_back, iteration);
        XrModuleGenerationSnapshot snapshot;
        REQUIRE_AT(xr_module_generation_snapshot(generation, &snapshot), iteration);
        REQUIRE_AT(snapshot.rollback_requested == 1u, iteration);
        /* Activate first lands in DRAINING because rollback unwinds a live
         * generation; rollback first lands in RETIRED because there was never
         * a manifest row to unwind. */
        REQUIRE_AT(snapshot.state == (race.activated ? XR_MODULE_GENERATION_DRAINING
                                                     : XR_MODULE_GENERATION_RETIRED),
                   iteration);
        REQUIRE_AT(snapshot.total_pins == 0, iteration);
        REQUIRE_AT(authority->active_generation_count == 0, iteration);
        REQUIRE_AT(authority->active_generations == NULL, iteration);
        REQUIRE_AT(authority->total_pins == 0, iteration);
        XrRuntimeGenerationLiveManifest manifest = {0};
        REQUIRE_AT(!xr_runtime_generation_live_manifest_snapshot(generation, &manifest), iteration);
        REQUIRE_AT(!xr_module_generation_pin_acquire(generation, XR_MODULE_GENERATION_PIN,
                                                     diagnostic, sizeof(diagnostic)),
                   iteration);
        discard_generation(&generation);
        destroy_authority(&authority);
        barrier_destroy(&barrier);
    }
    puts("  activate versus rollback");
}

/* ========== pin budget conservation under contention ==========
 *
 * Linearization point: each pin counter update under the authority gate. The
 * per-kind, per-generation, and authority-wide counters are three views of the
 * same fact, so after every acquire has been matched by a release all three
 * must read zero and the revision must have advanced by exactly one step per
 * counter mutation - no lost update, no double count. */

#define PIN_THREADS 8u
#define PIN_ROUNDS 32u

typedef struct PinBudgetRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    uint32_t acquired;
    uint32_t released;
    uint32_t refused;
} PinBudgetRace;

static void *pin_budget_worker(void *opaque) {
    PinBudgetRace *race = (PinBudgetRace *) opaque;
    barrier_wait(race->barrier);
    for (uint32_t round = 0; round < PIN_ROUNDS; round++) {
        char diagnostic[256] = {0};
        if (!xr_module_generation_pin_acquire(race->generation, XR_MODULE_GENERATION_PIN,
                                              diagnostic, sizeof(diagnostic))) {
            race->refused++;
            continue;
        }
        race->acquired++;
        if (xr_module_generation_pin_release(race->generation, XR_MODULE_GENERATION_PIN, diagnostic,
                                             sizeof(diagnostic)))
            race->released++;
    }
    return NULL;
}

static void test_pin_budget_is_conserved(const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationAuthority *authority = make_authority(1);
    XrLoadedModuleGeneration *generation = make_active(authority, plan);
    XrModuleGenerationSnapshot before;
    REQUIRE(xr_module_generation_snapshot(generation, &before));

    Barrier barrier;
    barrier_init(&barrier, PIN_THREADS);
    PinBudgetRace races[PIN_THREADS] = {0};
    xr_thread_t threads[PIN_THREADS];
    for (uint32_t i = 0; i < PIN_THREADS; i++) {
        races[i].barrier = &barrier;
        races[i].generation = generation;
        REQUIRE(xr_thread_create(&threads[i], pin_budget_worker, &races[i]));
    }
    uint32_t acquired = 0;
    uint32_t released = 0;
    uint32_t refused = 0;
    for (uint32_t i = 0; i < PIN_THREADS; i++) {
        REQUIRE(xr_thread_join(threads[i], NULL) == 0);
        acquired += races[i].acquired;
        released += races[i].released;
        refused += races[i].refused;
    }

    /* Every acquire that succeeded was released, and the budget refused the
     * rest rather than overrunning it. */
    REQUIRE(acquired == released);
    REQUIRE(acquired + refused == PIN_THREADS * PIN_ROUNDS);
    XrModuleGenerationSnapshot after;
    REQUIRE(xr_module_generation_snapshot(generation, &after));
    REQUIRE(after.state == XR_MODULE_GENERATION_ACTIVE);
    REQUIRE(after.total_pins == 0);
    REQUIRE(after.pins_by_kind[XR_MODULE_GENERATION_PIN] == 0);
    REQUIRE(after.revision == before.revision + (uint64_t) acquired + released);
    REQUIRE(authority->total_pins == 0);
    REQUIRE(xr_module_generation_verify(generation, diagnostic, sizeof(diagnostic)));

    discard_generation(&generation);
    destroy_authority(&authority);
    barrier_destroy(&barrier);
    puts("  pin budget is conserved under contention");
}

/* ========== retire vs late pin acquire ==========
 *
 * Linearization point: the DRAINING -> RETIRED store inside retire. A pin
 * request that arrives on either side of it must be refused, because pins
 * require ACTIVE - so this race admits exactly one outcome for the pin and the
 * retire must still see a zero-pin generation. */

typedef struct LatePinRace {
    Barrier *barrier;
    XrLoadedModuleGeneration *generation;
    bool retired;
    bool pinned;
    char pin_diagnostic[256];
} LatePinRace;

static void *late_pin_race_retirer(void *opaque) {
    LatePinRace *race = (LatePinRace *) opaque;
    char diagnostic[256] = {0};
    barrier_wait(race->barrier);
    race->retired = xr_module_generation_retire(race->generation, diagnostic, sizeof(diagnostic));
    return NULL;
}

static void *late_pin_race_pinner(void *opaque) {
    LatePinRace *race = (LatePinRace *) opaque;
    barrier_wait(race->barrier);
    race->pinned =
        xr_module_generation_pin_acquire(race->generation, XR_MODULE_GENERATION_CALLBACK,
                                         race->pin_diagnostic, sizeof(race->pin_diagnostic));
    return NULL;
}

static void test_retire_versus_late_pin(const XrTargetPlan *plan) {
    for (uint32_t iteration = 0; iteration < RACE_ITERATIONS; iteration++) {
        char diagnostic[512] = {0};
        XrRuntimeGenerationAuthority *authority = make_authority(1);
        XrLoadedModuleGeneration *generation = make_active(authority, plan);
        REQUIRE_AT(xr_module_generation_begin_drain(generation, diagnostic, sizeof(diagnostic)),
                   iteration);
        Barrier barrier;
        barrier_init(&barrier, 2);
        LatePinRace race = {.barrier = &barrier, .generation = generation};
        xr_thread_t retirer;
        xr_thread_t pinner;
        REQUIRE_AT(xr_thread_create(&retirer, late_pin_race_retirer, &race), iteration);
        REQUIRE_AT(xr_thread_create(&pinner, late_pin_race_pinner, &race), iteration);
        REQUIRE_AT(xr_thread_join(retirer, NULL) == 0, iteration);
        REQUIRE_AT(xr_thread_join(pinner, NULL) == 0, iteration);

        REQUIRE_AT(race.retired, iteration);
        REQUIRE_AT(!race.pinned, iteration);
        REQUIRE_AT(strstr(race.pin_diagnostic, "XR_OWN_3003") != NULL, iteration);
        XrModuleGenerationSnapshot snapshot;
        REQUIRE_AT(xr_module_generation_snapshot(generation, &snapshot), iteration);
        REQUIRE_AT(snapshot.state == XR_MODULE_GENERATION_RETIRED, iteration);
        REQUIRE_AT(snapshot.total_pins == 0, iteration);
        REQUIRE_AT(authority->total_pins == 0, iteration);
        REQUIRE_AT(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)),
                   iteration);
        destroy_authority(&authority);
        barrier_destroy(&barrier);
    }
    puts("  retire versus late pin acquire");
}

/* ========== authority destroy refuses live state ==========
 *
 * Destroy is the authority's own quiescence proof, so it must refuse while a
 * generation is loaded, while a pin is held, and while a generation is merely
 * retired but not unloaded. A refused destroy leaves the handle intact so the
 * caller can finish the teardown it skipped. */

static void test_authority_destroy_refuses_live_state(const XrTargetPlan *plan) {
    char diagnostic[512] = {0};
    XrRuntimeGenerationAuthority *authority = make_authority(1);
    XrLoadedModuleGeneration *generation = make_active(authority, plan);

    REQUIRE(!xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(authority != NULL);

    REQUIRE(xr_module_generation_pin_acquire(generation, XR_MODULE_GENERATION_PIN, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(!xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(authority != NULL);

    REQUIRE(xr_module_generation_begin_drain(generation, diagnostic, sizeof(diagnostic)));
    /* A pin outliving its drain still blocks retire, so it still blocks
     * destroy. */
    REQUIRE(!xr_module_generation_retire(generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(xr_module_generation_pin_release(generation, XR_MODULE_GENERATION_PIN, diagnostic,
                                             sizeof(diagnostic)));
    REQUIRE(xr_module_generation_retire(generation, diagnostic, sizeof(diagnostic)));

    /* Retired is not unloaded: the generation is still one of the authority's. */
    REQUIRE(!xr_runtime_generation_authority_destroy(&authority, diagnostic, sizeof(diagnostic)));
    REQUIRE(strstr(diagnostic, "XR_EXEC_5006") != NULL);
    REQUIRE(authority != NULL);

    REQUIRE(xr_module_generation_unload(&generation, diagnostic, sizeof(diagnostic)));
    REQUIRE(generation == NULL);
    destroy_authority(&authority);
    puts("  authority destroy refuses live state");
}

void run_generation_lifecycle_races(const XrTargetPlan *plan) {
    puts("generation lifecycle race gates:");
    test_execute_pin_versus_begin_drain(plan);
    test_last_pin_release_versus_retire(plan);
    test_concurrent_activation_admits_one(plan);
    test_activate_versus_rollback(plan);
    test_pin_budget_is_conserved(plan);
    test_retire_versus_late_pin(plan);
    test_authority_destroy_refuses_live_state(plan);
}
