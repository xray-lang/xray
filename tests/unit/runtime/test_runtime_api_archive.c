/*
 * test_runtime_api_archive.c - Runtime facade boundary over the installed archive
 *
 * The facade links from the runtime archive alone, so this test carries no
 * compiler builder and drives the same exact artifact pair a product host
 * would present. It proves both what the facade executes and where it refuses.
 */

#include "xray_runtime_api.h"
#include "runtime_scalar_artifacts.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/os/os_thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, label)                                                \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s failed at %s:%d (%s)\n", (label), __FILE__,    \
                    __LINE__, diagnostic);                                     \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static XrRuntimeGenerationBudget make_budget(void) {
    XrRuntimeGenerationBudget budget = {
        .schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION,
        .max_loaded_generations = 2,
        .max_total_pins = 8,
        .max_pins_per_generation = 4,
        .max_pins_by_kind = {4, 2, 2, 2, 2},
    };
    return budget;
}

typedef struct ActivationProviderContext {
    atomic_uint_least32_t allocations;
    atomic_uint_least32_t deallocations;
    atomic_uint_least32_t live_allocations;
} ActivationProviderContext;

static void *native_allocate(void *opaque, size_t size, size_t alignment) {
    ActivationProviderContext *context =
        (ActivationProviderContext *) opaque;
    if (!size || alignment > _Alignof(void *))
        return NULL;
    void *allocation = xr_malloc(size);
    if (allocation && context) {
        atomic_fetch_add_explicit(&context->allocations, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&context->live_allocations, 1,
                                  memory_order_relaxed);
    }
    return allocation;
}

static void native_deallocate(void *opaque, void *allocation, size_t size,
                              size_t alignment) {
    ActivationProviderContext *context =
        (ActivationProviderContext *) opaque;
    (void) size;
    (void) alignment;
    if (allocation && context) {
        atomic_fetch_add_explicit(&context->deallocations, 1,
                                  memory_order_relaxed);
        atomic_fetch_sub_explicit(&context->live_allocations, 1,
                                  memory_order_relaxed);
    }
    xr_free(allocation);
}

static void native_panic(void *context, const char *message,
                         size_t message_size) {
    (void) context;
    (void) message;
    (void) message_size;
    abort();
}

static XrRuntimeConfig make_config(void) {
    XrRuntimeConfig config = {
        .schema_version = XR_RUNTIME_CONFIG_SCHEMA_VERSION,
        .generation = make_budget(),
        .activation = {
            .max_active_entries = 8,
            .max_active_provider_registrations = 8,
            .max_active_finalizer_registrations = 4,
        },
        .providers = {
            .allocate = native_allocate,
            .deallocate = native_deallocate,
            .panic = native_panic,
        },
    };
    return config;
}

static XrRuntimeConfig make_tracked_config(
    ActivationProviderContext *context) {
    XrRuntimeConfig config = make_config();
    config.providers.allocate_context = context;
    config.providers.deallocate_context = context;
    return config;
}

/* A zero budget is not a default: the facade forwards it to the same checker
 * the generation authority uses, so an incomplete budget is refused rather
 * than filled in. */
static void test_runtime_requires_a_complete_budget(void) {
    char diagnostic[512] = {0};
    XrRuntime *runtime = (XrRuntime *) (uintptr_t) 1;
    XrRuntimeConfig empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(!xr_runtime_create(&empty, &runtime, diagnostic, sizeof(diagnostic)),
          "zero budget accepted");
    CHECK(runtime == NULL, "rejected create left a runtime handle");
    CHECK(strstr(diagnostic, "XR_EXEC_5003") != NULL,
          "zero budget diagnostic code");

    XrRuntimeConfig config = make_config();
    config.activation.max_active_provider_registrations = UINT32_C(65537);
    CHECK(!xr_runtime_create(&config, &runtime, diagnostic,
                             sizeof(diagnostic)),
          "runtime accepted an unbounded provider registration budget");
    CHECK(runtime == NULL,
          "rejected provider budget left a runtime handle");
    config = make_config();
    config.activation.max_active_finalizer_registrations = UINT32_C(65537);
    CHECK(!xr_runtime_create(&config, &runtime, diagnostic,
                             sizeof(diagnostic)),
          "runtime accepted an unbounded finalizer registration budget");
    CHECK(runtime == NULL,
          "rejected finalizer budget left a runtime handle");

    config = make_config();
    runtime = (XrRuntime *) (uintptr_t) 1;
    CHECK(!xr_runtime_create(&config, NULL, diagnostic, sizeof(diagnostic)),
          "create accepted a missing out parameter");
    CHECK(!xr_runtime_destroy(NULL, diagnostic, sizeof(diagnostic)),
          "destroy accepted a missing runtime");
    runtime = NULL;
    CHECK(!xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "destroy accepted a null runtime");
}

/* Both artifacts are authority, so neither may be inferred from the other or
 * from an empty image. */
static void test_load_requires_both_exact_artifacts(void) {
    char diagnostic[512] = {0};
    XrRuntimeConfig config = make_config();
    XrRuntime *runtime = NULL;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic)),
          "runtime create");
    if (!runtime)
        return;

    XrModule *module = (XrModule *) (uintptr_t) 1;
    CHECK(!xr_module_load_target_plan(runtime, NULL, 0, NULL, 0, &module,
                                      diagnostic, sizeof(diagnostic)),
          "load accepted two empty artifacts");
    CHECK(module == NULL, "rejected load left a module handle");
    CHECK(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL,
          "empty artifact diagnostic code");

    module = (XrModule *) (uintptr_t) 1;
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
              NULL, 0, &module, diagnostic, sizeof(diagnostic)),
          "load accepted a missing target artifact");
    CHECK(module == NULL, "missing target artifact left a module handle");

    /* The target image is the semantic image here: it decodes, and it is not
     * the artifact this authority binds. */
    module = (XrModule *) (uintptr_t) 1;
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
              xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm), &module,
              diagnostic, sizeof(diagnostic)),
          "load accepted a semantic image as its target plan");
    CHECK(module == NULL, "mismatched artifact left a module handle");

    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "runtime destroy after rejected loads");
    CHECK(runtime == NULL, "destroy left a runtime handle");
}

static void check_export_result(XrModule *module, int64_t expected,
                                const char *label) {
    char diagnostic[512] = {0};
    const XrExport *found = NULL;
    CHECK(xr_module_find_export(module, "xtp_probe", &found, diagnostic,
                                sizeof(diagnostic)), label);
    XrExportValue result = {0};
    CHECK(found && xr_export_call(found, NULL, 0, &result, diagnostic,
                                  sizeof(diagnostic)), label);
    CHECK(result.kind == XR_EXPORT_VALUE_I64 && result.i64 == expected,
          label);
}

/* The whole product path: verify, atomically publish one exact export, execute,
 * unload, and destroy using only the runtime archive. */
static void test_loaded_module_lifecycle_and_export_boundary(void) {
    char diagnostic[512] = {0};
    XrRuntimeConfig config = make_config();
    XrRuntime *runtime = NULL;
    XrModule *module = NULL;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic)),
          "runtime create");
    if (!runtime)
        return;
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm, sizeof(xr_runtime_export_xsm),
              xr_runtime_export_xtp, sizeof(xr_runtime_export_xtp), &module,
              diagnostic, sizeof(diagnostic)),
          "exact artifact pair load");
    CHECK(module != NULL, "successful load produced no module");
    if (!module) {
        xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic));
        return;
    }

    /* A live module holds the runtime open. Destroying it here would strand a
     * loaded generation, so the facade refuses. */
    XrRuntime *borrowed = runtime;
    CHECK(!xr_runtime_destroy(&borrowed, diagnostic, sizeof(diagnostic)),
          "destroy accepted a runtime with a loaded module");
    CHECK(borrowed == runtime, "rejected destroy cleared the runtime handle");
    CHECK(strstr(diagnostic, "XR_EXEC_5006") != NULL,
          "loaded module destroy diagnostic code");

    check_export_result(module, 42, "published export execution");

    const XrExport *found = (const XrExport *) (uintptr_t) 1;
    CHECK(!xr_module_find_export(module, "no_such_export", &found, diagnostic,
                                 sizeof(diagnostic)),
          "find_export resolved an absent name");
    CHECK(found == NULL, "absent name left an export handle");
    found = (const XrExport *) (uintptr_t) 1;
    CHECK(!xr_module_find_export(module, "", &found, diagnostic,
                                 sizeof(diagnostic)),
          "find_export accepted an empty name");
    CHECK(!xr_module_find_export(module, "xtp_probe", NULL, diagnostic,
                                 sizeof(diagnostic)),
          "find_export accepted a missing out parameter");

    /* The call entry refuses every unresolved input rather than dereferencing
     * it even though the module owns another valid entry. */
    XrExportValue result;
    memset(&result, 0xAB, sizeof(result));
    CHECK(!xr_export_call(NULL, NULL, 0, &result, diagnostic,
                          sizeof(diagnostic)),
          "export_call accepted a null handle");
    CHECK(result.kind == 0 && result.i64 == 0,
          "rejected call left a stale result");
    CHECK(strstr(diagnostic, "XR_ARTIFACT_2004") != NULL,
          "null handle diagnostic code");
    XrExportValue argument = {.kind = XR_EXPORT_VALUE_I64, .i64 = 1};
    CHECK(!xr_export_call(NULL, &argument, 1, &result, diagnostic,
                          sizeof(diagnostic)),
          "export_call accepted a null handle with arguments");
    CHECK(!xr_export_call(NULL, NULL, 1, &result, diagnostic,
                          sizeof(diagnostic)),
          "export_call accepted a counted null argument vector");

    /* Unload begins by draining, and draining accepts no state but ACTIVE, so
     * a successful unload here is the proof that load really activated this
     * generation rather than leaving it verified or ready. */
    CHECK(xr_module_unload(&module, diagnostic, sizeof(diagnostic)),
          "module unload");
    CHECK(module == NULL, "unload left a module handle");
    CHECK(!xr_module_unload(&module, diagnostic, sizeof(diagnostic)),
          "unload accepted an already unloaded module");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "runtime destroy after unload");
    CHECK(runtime == NULL, "destroy left a runtime handle");
}

/* A duplicate active export key is not a hot-reload guess. The second load
 * rolls its active-but-unpublished generation back, leaves the first module
 * callable, and the same key can be published again only after unload. */
static void test_duplicate_publication_rolls_back(void) {
    char diagnostic[512] = {0};
    XrRuntimeConfig config = make_config();
    XrRuntime *runtime = NULL;
    XrModule *first = NULL;
    XrModule *duplicate = (XrModule *) (uintptr_t) 1;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic)),
          "duplicate runtime create");
    uint8_t corrupted[sizeof(xr_runtime_export_xtp)];
    memcpy(corrupted, xr_runtime_export_xtp, sizeof(corrupted));
    corrupted[sizeof(corrupted) - 1u] ^= 1u;
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm, sizeof(xr_runtime_export_xsm),
              corrupted, sizeof(corrupted), &duplicate, diagnostic,
              sizeof(diagnostic)),
          "corrupted plan reached entry publication");
    CHECK(duplicate == NULL,
          "corrupted plan left a module or registration owner");
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm, sizeof(xr_runtime_export_xsm),
              xr_runtime_export_xtp, sizeof(xr_runtime_export_xtp), &first,
              diagnostic, sizeof(diagnostic)),
          "first exported module load");
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm, sizeof(xr_runtime_export_xsm),
              xr_runtime_export_xtp, sizeof(xr_runtime_export_xtp),
              &duplicate, diagnostic, sizeof(diagnostic)),
          "duplicate exported module load");
    CHECK(duplicate == NULL && strstr(diagnostic, "XR_EXEC_5005") != NULL,
          "duplicate publication rollback diagnostic");
    check_export_result(first, 42, "first export survives duplicate rollback");
    CHECK(xr_module_unload(&first, diagnostic, sizeof(diagnostic)),
          "first duplicate fixture unload");
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm, sizeof(xr_runtime_export_xsm),
              xr_runtime_export_xtp, sizeof(xr_runtime_export_xtp),
              &duplicate, diagnostic, sizeof(diagnostic)),
          "republish after exact unload");
    check_export_result(duplicate, 42, "republished export execution");
    CHECK(xr_module_unload(&duplicate, diagnostic, sizeof(diagnostic)),
          "republished module unload");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "duplicate runtime destroy");
}

typedef struct ConcurrentLoadContext {
    XrRuntime *runtime;
    XrModule *module;
    bool loaded;
} ConcurrentLoadContext;

static void *load_exported_module(void *opaque) {
    ConcurrentLoadContext *context = (ConcurrentLoadContext *) opaque;
    char diagnostic[512] = {0};
    context->loaded = xr_module_load_target_plan(
        context->runtime, xr_runtime_export_xsm,
        sizeof(xr_runtime_export_xsm), xr_runtime_export_xtp,
        sizeof(xr_runtime_export_xtp), &context->module, diagnostic,
        sizeof(diagnostic));
    return NULL;
}

static void test_concurrent_duplicate_publication_is_unique(void) {
    char diagnostic[512] = {0};
    ActivationProviderContext provider_context = {0};
    XrRuntimeConfig config = make_tracked_config(&provider_context);
    XrRuntime *runtime = NULL;
    ConcurrentLoadContext contexts[2] = {0};
    xr_thread_t threads[2] = {0};
    CHECK(xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic)),
          "concurrent runtime create");
    for (uint32_t i = 0; i < 2; i++) {
        contexts[i].runtime = runtime;
        CHECK(xr_thread_create(&threads[i], load_exported_module,
                               &contexts[i]),
              "concurrent load thread create");
    }
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < 2; i++) {
        CHECK(xr_thread_join(threads[i], NULL) == 0,
              "concurrent load thread join");
        loaded += contexts[i].loaded ? 1u : 0u;
    }
    CHECK(loaded == 1, "concurrent duplicate publication count");
    CHECK(atomic_load_explicit(&provider_context.allocations,
                               memory_order_relaxed) == 2 &&
              atomic_load_explicit(&provider_context.deallocations,
                                   memory_order_relaxed) == 1 &&
              atomic_load_explicit(&provider_context.live_allocations,
                                   memory_order_relaxed) == 1,
          "concurrent losing activation did not roll back exactly once");
    for (uint32_t i = 0; i < 2; i++) {
        if (!contexts[i].module)
            continue;
        check_export_result(contexts[i].module, 42,
                            "concurrent winner execution");
        CHECK(xr_module_unload(&contexts[i].module, diagnostic,
                               sizeof(diagnostic)),
              "concurrent winner unload");
    }
    CHECK(atomic_load_explicit(&provider_context.deallocations,
                               memory_order_relaxed) == 2 &&
              atomic_load_explicit(&provider_context.live_allocations,
                                   memory_order_relaxed) == 0,
          "concurrent winning activation finalizer did not run exactly once");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "concurrent runtime destroy");
}

/* Loading twice and unloading in the other order proves the runtime tracks
 * live modules by count rather than by the last handle it saw. */
static void test_two_modules_load_and_unload_out_of_order(void) {
    char diagnostic[512] = {0};
    XrRuntimeConfig config = make_config();
    XrRuntime *runtime = NULL;
    XrModule *first = NULL;
    XrModule *second = NULL;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic, sizeof(diagnostic)),
          "runtime create");
    if (!runtime)
        return;
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
              xr_runtime_scalar_xtp, sizeof(xr_runtime_scalar_xtp), &first,
              diagnostic, sizeof(diagnostic)),
          "first module load");
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
              xr_runtime_scalar_xtp, sizeof(xr_runtime_scalar_xtp), &second,
              diagnostic, sizeof(diagnostic)),
          "second module load");

    /* The budget admits two generations, so a third is refused by the same
     * hard budget rather than by an allocation failure. */
    XrModule *third = (XrModule *) (uintptr_t) 1;
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm, sizeof(xr_runtime_scalar_xsm),
              xr_runtime_scalar_xtp, sizeof(xr_runtime_scalar_xtp), &third,
              diagnostic, sizeof(diagnostic)),
          "load exceeded the loaded generation budget");
    CHECK(third == NULL, "over-budget load left a module handle");
    CHECK(strstr(diagnostic, "XR_EXEC_5003") != NULL,
          "over-budget diagnostic code");

    if (second)
        CHECK(xr_module_unload(&second, diagnostic, sizeof(diagnostic)),
              "second module unload");
    XrRuntime *borrowed = runtime;
    CHECK(!xr_runtime_destroy(&borrowed, diagnostic, sizeof(diagnostic)),
          "destroy accepted a runtime with one module still loaded");
    if (first)
        CHECK(xr_module_unload(&first, diagnostic, sizeof(diagnostic)),
              "first module unload");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "runtime destroy after both unloads");
}

static void test_verification_precedes_every_activation_callback(void) {
    char diagnostic[512] = {0};
    ActivationProviderContext context = {0};
    XrRuntimeConfig config = make_tracked_config(&context);
    XrRuntime *runtime = NULL;
    XrModule *module = (XrModule *) (uintptr_t) 1;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic,
                            sizeof(diagnostic)),
          "verification-order runtime create");
    uint8_t corrupted[sizeof(xr_runtime_export_xtp)];
    memcpy(corrupted, xr_runtime_export_xtp, sizeof(corrupted));
    corrupted[sizeof(corrupted) - 1u] ^= 1u;
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm,
              sizeof(xr_runtime_export_xsm), corrupted,
              sizeof(corrupted), &module, diagnostic,
              sizeof(diagnostic)),
          "corrupt artifact reached activation callbacks");
    CHECK(module == NULL, "corrupt artifact left a module");
    CHECK(atomic_load_explicit(&context.allocations, memory_order_relaxed) ==
                  0 &&
              atomic_load_explicit(&context.deallocations,
                                   memory_order_relaxed) == 0 &&
              atomic_load_explicit(&context.live_allocations,
                                   memory_order_relaxed) == 0,
          "verification failure invoked a provider or finalizer");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "verification-order runtime destroy");
}

static void test_missing_binding_rejects_whole_activation(void) {
    for (uint32_t missing = 0; missing < 3; missing++) {
        char diagnostic[512] = {0};
        ActivationProviderContext context = {0};
        XrRuntimeConfig config = make_tracked_config(&context);
        if (missing == 0)
            config.providers.allocate = NULL;
        else if (missing == 1)
            config.providers.deallocate = NULL;
        else
            config.providers.panic = NULL;
        XrRuntime *runtime = NULL;
        XrModule *module = (XrModule *) (uintptr_t) 1;
        CHECK(xr_runtime_create(&config, &runtime, diagnostic,
                                sizeof(diagnostic)),
              "missing-binding runtime create");
        CHECK(!xr_module_load_target_plan(
                  runtime, xr_runtime_export_xsm,
                  sizeof(xr_runtime_export_xsm), xr_runtime_export_xtp,
                  sizeof(xr_runtime_export_xtp), &module, diagnostic,
                  sizeof(diagnostic)),
              "missing binding activated a module");
        CHECK(module == NULL && strstr(diagnostic, "XR_EXEC_5008") != NULL,
              "missing binding activation diagnostic");
        CHECK(atomic_load_explicit(&context.allocations,
                                   memory_order_relaxed) == 0 &&
                  atomic_load_explicit(&context.deallocations,
                                       memory_order_relaxed) == 0,
              "missing binding invoked a partial activation callback");
        CHECK(xr_runtime_destroy(&runtime, diagnostic,
                                 sizeof(diagnostic)),
              "missing-binding runtime destroy");
    }
}

static void test_registration_budget_failure_rolls_back_whole_batch(
    bool provider_budget) {
    char diagnostic[512] = {0};
    ActivationProviderContext context = {0};
    XrRuntimeConfig config = make_tracked_config(&context);
    config.activation.max_active_provider_registrations =
        provider_budget ? 3 : 4;
    config.activation.max_active_finalizer_registrations =
        provider_budget ? 2 : 1;
    XrRuntime *runtime = NULL;
    XrModule *first = NULL;
    XrModule *rejected = (XrModule *) (uintptr_t) 1;
    CHECK(xr_runtime_create(&config, &runtime, diagnostic,
                            sizeof(diagnostic)),
          "registration-budget runtime create");
    CHECK(xr_module_load_target_plan(
              runtime, xr_runtime_export_xsm,
              sizeof(xr_runtime_export_xsm), xr_runtime_export_xtp,
              sizeof(xr_runtime_export_xtp), &first, diagnostic,
              sizeof(diagnostic)),
          "registration-budget first load");
    CHECK(!xr_module_load_target_plan(
              runtime, xr_runtime_scalar_xsm,
              sizeof(xr_runtime_scalar_xsm), xr_runtime_scalar_xtp,
              sizeof(xr_runtime_scalar_xtp), &rejected, diagnostic,
              sizeof(diagnostic)),
          "registration budget admitted a partial second activation");
    CHECK(rejected == NULL && strstr(diagnostic, "XR_EXEC_5003") != NULL,
          "registration-budget rollback diagnostic");
    CHECK(atomic_load_explicit(&context.allocations, memory_order_relaxed) ==
                  2 &&
              atomic_load_explicit(&context.deallocations,
                                   memory_order_relaxed) == 1 &&
              atomic_load_explicit(&context.live_allocations,
                                   memory_order_relaxed) == 1,
          "rejected registration batch did not run exact rollback finalizer");
    check_export_result(first, 42,
                        "published module survives registration rollback");
    CHECK(xr_module_unload(&first, diagnostic, sizeof(diagnostic)),
          "registration-budget first unload");
    CHECK(atomic_load_explicit(&context.deallocations,
                               memory_order_relaxed) == 2 &&
              atomic_load_explicit(&context.live_allocations,
                                   memory_order_relaxed) == 0,
          "successful activation did not consume its unload finalizer");
    CHECK(xr_runtime_destroy(&runtime, diagnostic, sizeof(diagnostic)),
          "registration-budget runtime destroy");
}

int main(void) {
    test_runtime_requires_a_complete_budget();
    test_load_requires_both_exact_artifacts();
    test_loaded_module_lifecycle_and_export_boundary();
    test_duplicate_publication_rolls_back();
    test_concurrent_duplicate_publication_is_unique();
    test_two_modules_load_and_unload_out_of_order();
    test_verification_precedes_every_activation_callback();
    test_missing_binding_rejects_whole_activation();
    test_registration_budget_failure_rolls_back_whole_batch(true);
    test_registration_budget_failure_rolls_back_whole_batch(false);
    if (failures) {
        fprintf(stderr, "runtime API archive boundary failures: %d\n", failures);
        return 1;
    }
    puts("runtime API archive boundary passed");
    return 0;
}
