/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_storage.c - Independent ownership, fault and resource witnesses
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "frontend/analyzer/xanalyzer_export_query.h"
#include "toolchain/xcompiler_session.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "base/xarena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../base/parser_resources_observer.h"

static const XrCompileResourceLimits generous = {1048576, 1048576, 1048576};

static XrCompileResourceStatus session_result(XrCompilerSessionStatus status) {
    switch (status) {
        case XR_COMPILER_SESSION_OK: return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT: return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET: return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false);
    return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes == physical_live && result.allocated_bytes == physical_total);
    CHECK(result.peak_bytes == physical_peak);
    return result;
}

static void empty_storage(const XaAnalyzer *analyzer) {
    CHECK(!analyzer->consteval_arena && !analyzer->callable_target_arena && !analyzer->type_pool);
    CHECK(analyzer->semantic_revision == 73);
}

static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *result) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_release(resources);
        return status;
    }
    XrCompileState *state = xr_compile_session_compile_state(session);
    status = xa_analyzer_storage_open(state, &analyzer);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xr_compile_state_resources(analyzer.type_pool->state) == resources);
        CHECK(xr_compiler_arena_matches_state(analyzer.consteval_arena, state));
        CHECK(xr_compiler_arena_matches_state(analyzer.callable_target_arena, state));
        XrType *type = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (type) CHECK(type->kind == XR_KIND_INT && type->id == 1);
        char *constant = xr_arena_strdup(analyzer.consteval_arena, "ctfe");
        char *target = xr_arena_strdup(analyzer.callable_target_arena, "callee");
        char *name = xr_pool_strdup(analyzer.type_pool, "owned");
        if (constant) CHECK(!strcmp(constant, "ctfe"));
        if (target) CHECK(!strcmp(target, "callee"));
        if (name) CHECK(!strcmp(name, "owned"));
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(type && constant && target && name);
    } else {
        empty_storage(&analyzer);
        size_t attempts = attempt_count;
        XrCompileResourceStats failed = stats(resources);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == status);
        CHECK(attempt_count == attempts);
        XrCompileResourceStats retry = stats(resources);
        CHECK(retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
    }
    *result = stats(resources);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer);
    xa_analyzer_storage_close(&analyzer);
    empty_storage(&analyzer);
    CHECK(attempt_count == attempts);
    xr_compile_session_free(session);
    XrCompileResourceStats released = stats(resources);
    CHECK(released.work == result->work && released.allocated_bytes == result->allocated_bytes);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void producer_death_and_shared_failure(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_resources_new(&generous, &resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILER_SESSION_OK);
    XrCompileState *state = xr_compile_session_compile_state(session);
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    CHECK(!strcmp(xr_arena_strdup(analyzer.consteval_arena, "after"), "after"));
    CHECK(!strcmp(xr_pool_strdup(analyzer.type_pool, "producer"), "producer"));
    CHECK(xr_pool_alloc_type(analyzer.type_pool, XR_KIND_BOOL)->id == 1);
    XrCompileResourceStats before = stats(resources);
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
    CHECK(!xr_arena_alloc_raw(analyzer.consteval_arena, 8));
    CHECK(!xr_arena_alloc(analyzer.callable_target_arena, 8));
    CHECK(!xr_pool_alloc(analyzer.type_pool, 8));
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == before.work && after.allocated_bytes == before.allocated_bytes);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer);
    empty_storage(&analyzer);
    CHECK(attempt_count == attempts && !physical_live && !allocation_count);
}

static void occupied_output_and_foreign_state(void) {
    reset();
    XrCompileResources *owner = NULL, *other = NULL;
    XrCompileState *state = NULL, *foreign = NULL;
    CHECK(xr_compile_resources_new(&generous, &owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&generous, &other) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(owner, &state) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(other, &foreign) == XR_COMPILE_RESOURCE_OK);
    XaAnalyzer analyzer = {0};
    analyzer.semantic_revision = 73;
    CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
    XrArena *constant = analyzer.consteval_arena, *callable = analyzer.callable_target_arena;
    XrTypePool *pool = analyzer.type_pool;
    XrCompileResourceStats owner_before;
    CHECK(xr_compile_resources_stats(owner, &owner_before) == XR_COMPILE_RESOURCE_OK);
    size_t attempts = attempt_count;
    CHECK(xa_analyzer_storage_open(foreign, &analyzer) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(analyzer.consteval_arena == constant && analyzer.callable_target_arena == callable);
    CHECK(analyzer.type_pool == pool && analyzer.semantic_revision == 73);
    CHECK(attempt_count == attempts && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats owner_after;
    CHECK(xr_compile_resources_stats(owner, &owner_after) == XR_COMPILE_RESOURCE_OK);
    CHECK(owner_after.work == owner_before.work && owner_after.allocated_bytes == owner_before.allocated_bytes);
    CHECK(!strcmp(xr_arena_strdup(constant, "still"), "still"));
    xa_analyzer_storage_close(&analyzer);
    xr_compile_state_release(state);
    xr_compile_state_release(foreign);
    xr_compile_resources_release(owner);
    xr_compile_resources_release(other);
    CHECK(!physical_live && !allocation_count);
}

static XrCompileResourceStatus export_query_pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    XrHashMap *exports = NULL;
    XaAnalyzer analyzer = {0}; XaSymbol expected = {0};
    XaSymbol *output = &expected;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = session_result(xr_compile_session_new(resources, &session));
    analyzer.compiler_session = session;
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrOsIoPolicy policy = xr_compile_io_policy(resources);
        XrOsIoStatus created = xr_hashmap_owned_new(&policy, &exports);
        if (xa_accept_export_io(&analyzer, created)) {
            CHECK(xa_accept_export_io(&analyzer, xr_hashmap_owned_set(exports, "answer", &expected)) ||
                xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
            if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
                CHECK(xa_find_export_symbol(&analyzer, exports, "answer", &output) ||
                    xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
                if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) {
                    CHECK(output == &expected);
                    CHECK(xa_find_export_symbol(&analyzer, exports, "missing", &output) ||
                        xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK);
                    if (xr_compile_session_resource_status(session) == XR_COMPILE_RESOURCE_OK) CHECK(!output);
                }
            }
        }
        status = xr_compile_session_resource_status(session);
        if (status != XR_COMPILE_RESOURCE_OK) {
            XrCompileResourceStats failed = stats(resources);
            size_t attempts = attempt_count;
            output = &expected;
            CHECK(!xa_find_export_symbol(&analyzer, exports, "answer", &output));
            CHECK(output == &expected);
            CHECK(!xa_find_export_symbol(&analyzer, NULL, NULL, &output));
            CHECK(output == &expected && attempt_count == attempts);
            XrCompileResourceStats retry = stats(resources);
            CHECK(retry.work == failed.work && retry.allocated_bytes == failed.allocated_bytes);
        }
    }
    *observed = stats(resources);
    xr_hashmap_owned_free(exports);xr_compile_session_free(session);
    XrCompileResourceStats after = stats(resources);
    CHECK(after.work == observed->work && after.allocated_bytes == observed->allocated_bytes);
    xr_compile_resources_release(resources);
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void export_query_resources(void) {
    reset();XrCompileResourceStats baseline = {0};
    CHECK(export_query_pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;CHECK(sites > 0);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();fail_at = ordinal;XrCompileResourceStats observed = {0};
        CHECK(export_query_pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        reset();XrCompileResourceStats observed = {0};
        XrCompileResourceLimits limits = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound > 1);*bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(export_query_pipeline(&limits, &observed) ==
            (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    printf("export query: %zu allocation faults, three exact/minus1/plus1 axes, "
        "output preservation, sticky no-charge retry and physical zero passed\n", sites);
}

int main(void) {
    reset();
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(&generous, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count;
    CHECK(sites >= 9);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) {
        reset();
        fail_at = ordinal;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(&generous, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(!physical_live && !allocation_count);
    }
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    reset();
    XrCompileResourceStats observed = {0};
    CHECK(pipeline(&exact, &observed) == XR_COMPILE_RESOURCE_OK);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits below = exact;
        if (axis == 0) --below.allocated_bytes;
        if (axis == 1) --below.live_bytes;
        if (axis == 2) --below.work;
        reset();
        CHECK(pipeline(&below, &observed) == XR_COMPILE_RESOURCE_BUDGET);
        CHECK(!physical_live && !allocation_count);
    }
    producer_death_and_shared_failure();
    occupied_output_and_foreign_state();
    export_query_resources();
    printf("analyzer storage: %zu allocator faults, three exact/minus-one axes, "
        "producer death, shared sticky failure and occupied output passed\n", sites);
    return 0;
}
