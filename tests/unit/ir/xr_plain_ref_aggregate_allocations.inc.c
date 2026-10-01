/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_plain_ref_aggregate_allocations.inc.c - Receiving plan allocation ownership
 */
#include "../program/xr_program_allocation_probe.h"
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) xr_program_test_system_malloc(size)
#define xr_calloc(count, size) xr_program_test_system_calloc(count, size)
#define xr_realloc(pointer, size) xr_program_test_system_realloc(pointer, size)
#define xr_free(pointer) xr_program_test_system_free(pointer)

XR_FUNC bool xr_test_c_emission_plan_build(const XrTargetPlan *, const XrSemanticPlan *,
    XrFingerprint, XrCEmissionPlan **, char *, size_t);
XR_FUNC void xr_test_c_emission_plan_free(XrCEmissionPlan *);
XR_FUNC void xr_test_emission_arena_flush_cache(void);

static size_t plain_ref_alloc_attempts, plain_ref_alloc_fail_at, plain_ref_alloc_live;
static bool plain_ref_alloc_failed;
static void *plain_ref_alloc_pointers[1024];

static bool plain_ref_alloc_should_fail(void) {
    if (++plain_ref_alloc_attempts != plain_ref_alloc_fail_at)
        return false;
    plain_ref_alloc_failed = true;
    return true;
}

static void *plain_ref_alloc_track(void *pointer) {
    TEST_REQUIRE(pointer != NULL, "injected allocator has physical memory");
    for (size_t i = 0; i < XR_COUNTOF(plain_ref_alloc_pointers); i++) {
        if (plain_ref_alloc_pointers[i])
            continue;
        plain_ref_alloc_pointers[i] = pointer;
        plain_ref_alloc_live++;
        return pointer;
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return plain_ref_alloc_should_fail() ? NULL
        : plain_ref_alloc_track(xr_program_test_system_malloc(size));
}

void *xr_program_test_calloc(size_t count, size_t size) {
    return plain_ref_alloc_should_fail() ? NULL
        : plain_ref_alloc_track(xr_program_test_system_calloc(count, size));
}

void *xr_program_test_realloc(void *pointer, size_t size) {
    if (plain_ref_alloc_should_fail())
        return NULL;
    if (!pointer)
        return plain_ref_alloc_track(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0; i < XR_COUNTOF(plain_ref_alloc_pointers); i++) {
        if (plain_ref_alloc_pointers[i] != pointer)
            continue;
        void *result = xr_program_test_system_realloc(pointer, size);
        TEST_REQUIRE(result != NULL, "injected reallocation has physical memory");
        plain_ref_alloc_pointers[i] = result;
        return result;
    }
    abort();
}

void xr_program_test_free(void *pointer) {
    if (!pointer)
        return;
    for (size_t i = 0; i < XR_COUNTOF(plain_ref_alloc_pointers); i++) {
        if (plain_ref_alloc_pointers[i] != pointer)
            continue;
        plain_ref_alloc_pointers[i] = NULL;
        plain_ref_alloc_live--;
        break;
    }
    /* The ordinary reachability owner can transfer temporary module arrays. */
    xr_program_test_system_free(pointer);
}

char *xr_program_test_strdup(const char *source) {
    if (!source)
        return NULL;
    size_t size = strlen(source) + 1u;
    char *copy = (char *) xr_program_test_malloc(size);
    if (copy)
        memcpy(copy, source, size);
    return copy;
}

TEST(cgen_plain_ref_aggregate_allocation_ownership) {
    XiFunc *producer = compile_to_ir(plain_ref_aggregate_source);
    XrTargetProfile *profile = xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    XrTargetPlan *target = NULL;
    char error[512] = {0};
    TEST_REQUIRE(producer && profile && xr_target_plan_build(producer->semantic_plan, profile,
        &target, error, sizeof(error)), "ordinary Source and Target fixture is independent of injection");
    test_func_free(producer);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target);
    size_t failed_builds = 0, arena_cache_failures = 0;
    for (size_t fail_at = 1; ; fail_at++) {
        TEST_REQUIRE(plain_ref_alloc_live == 0, "all previous receiving owners were released");
        plain_ref_alloc_attempts = 0;
        plain_ref_alloc_fail_at = fail_at;
        plain_ref_alloc_failed = false;
        XrCEmissionPlan *emission = (XrCEmissionPlan *) (uintptr_t) 1;
        bool built = xr_test_c_emission_plan_build(target, semantic,
            xr_target_profile_fingerprint(profile), &emission, error, sizeof(error));
        size_t build_attempts = plain_ref_alloc_attempts;
        bool build_failed_allocation = plain_ref_alloc_failed;
        if (built) {
            TEST_REQUIRE(emission && xr_c_emission_plan_verify(emission, target, semantic,
                xr_target_profile_fingerprint(profile), error, sizeof(error)),
                "every successful receiving plan independently verifies");
            xr_test_c_emission_plan_free(emission);
        } else {
            TEST_REQUIRE(build_failed_allocation && emission == NULL,
                "allocation failure publishes no partial receiving plan");
            failed_builds++;
        }
        bool cleanup_failed_allocation = plain_ref_alloc_failed && !build_failed_allocation;
        /* Cache disposal itself must finish even when caching was refused. */
        plain_ref_alloc_fail_at = SIZE_MAX;
        xr_test_emission_arena_flush_cache();
        TEST_REQUIRE(plain_ref_alloc_live == 0, "rows, owned type segments and cache release exactly once");
        if (cleanup_failed_allocation)
            arena_cache_failures++;
        if (built && !build_failed_allocation && !cleanup_failed_allocation) {
            TEST_REQUIRE(fail_at > build_attempts && failed_builds > 8,
                "injection swept every actual receiving allocation");
            break;
        }
    }
    plain_ref_alloc_fail_at = SIZE_MAX;
    printf("  %zu receiving build failures and %zu cache failures release all owners\n",
        failed_builds, arena_cache_failures);
    xr_target_plan_free(target);
    xr_target_profile_free(profile);
}
