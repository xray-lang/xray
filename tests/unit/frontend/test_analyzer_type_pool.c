/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_type_pool.c - Actual pool pins and registry-held type storage
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "runtime/value/xtype_pool.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool record_work;
static uint64_t boundaries[32768];
static size_t boundary_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,
                                                         uint64_t units) {
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (record_work && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current = {0};
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(boundary_count < sizeof(boundaries) / sizeof(boundaries[0]));
        boundaries[boundary_count++] = current.work;
    }
    return status;
}
static const XrCompileResourceLimits ample = {1048576, 1048576, 1048576};
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static XrCompileResourceStatus session_open(XrCompileResources *resources,
                                           XrCompilerSession **output) {
    switch (xr_compile_session_new(resources, output)) {
        case XR_COMPILER_SESSION_OK: return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT: return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET: return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false); return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

typedef struct TypeWitness {
    XrType *function;
    const char *name;
} TypeWitness;
/* Assemble nonempty payloads using the production pool birth/allocation
 * functions. This witnesses storage, not full signature or nominal semantics. */
static XrCompileResourceStatus build_types(XrTypePool *pool, TypeWitness *output) {
    XrCompileState *state = pool->state;
    XrType *leaf = xr_pool_alloc_type(pool, XR_KIND_INT);
    if (!leaf) return xr_compile_state_status(state);
    XrType *array = xr_pool_alloc_type(pool, XR_KIND_ARRAY);
    if (!array) return xr_compile_state_status(state);
    array->container.element_type = leaf;
    XrType *function = xr_pool_alloc_type(pool, XR_KIND_FUNCTION);
    if (!function) return xr_compile_state_status(state);
    XrFunctionParam *params = xr_pool_alloc_array(pool, sizeof(*params), 1);
    if (!params) return xr_compile_state_status(state);
    params[0].type = array;
    function->function.params = params;
    function->function.param_count = 1;
    function->function.min_params = 1;
    function->function.return_type = array;
    char name[] = "retained-type";
    char *copy = xr_pool_strdup(pool, name);
    if (!copy) return xr_compile_state_status(state);
    memset(name, '!', sizeof(name) - 1);
    CHECK(!strcmp(copy, "retained-type"));
    (void) xr_pool_alloc(pool, XR_ARENA_SEGMENT_SIZE + 8);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return xr_compile_state_status(state);
    output->function = function; output->name = copy;
    return XR_COMPILE_RESOURCE_OK;
}
static void verify_types(TypeWitness witness) {
    CHECK(witness.function->kind == XR_KIND_FUNCTION && witness.function->id == 3);
    CHECK(witness.function->function.param_count == 1);
    XrType *array = witness.function->function.params[0].type;
    CHECK(array == witness.function->function.return_type && array->kind == XR_KIND_ARRAY && array->id == 2);
    CHECK(array->container.element_type->kind == XR_KIND_INT && array->container.element_type->id == 1);
    CHECK(!strcmp(witness.name, "retained-type"));
}
static XrCompileResourceStatus pipeline(bool dead, const XrCompileResourceLimits *limits,
                                      XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0}; XaScope *view = NULL; XaSymbol *pending = NULL, *bound = NULL;
    XrTypePoolPin pin = {0}; TypeWitness witness = {0};
    bool producer_closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) status = session_open(resources, &session);
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_analyzer_storage_open(state, &analyzer);
    XrTypePool *pool = analyzer.type_pool;
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xa_symbol_registry_type_pool(analyzer.symbol_registry) == pool);
        view = xa_scope_new(analyzer.symbol_registry, XA_SCOPE_GLOBAL, NULL);
        if (!view) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_type_pool_pin(state, pool, &pin);
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); producer_closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        pending = xa_symbol_new(analyzer.symbol_registry, "fn", XA_SYM_FUNCTION);
        if (!pending) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        status = xa_scope_add_symbol(analyzer.global_scope, pending);
        if (status == XR_COMPILE_RESOURCE_OK) { bound = pending; pending = NULL; }
    }
    if (status == XR_COMPILE_RESOURCE_OK && !xa_scope_set_alias(view, "borrowed", bound))
        status = xr_compile_state_status(state);
    if (status == XR_COMPILE_RESOURCE_OK) {
        status = build_types(pool, &witness);
        if (status != XR_COMPILE_RESOURCE_OK) CHECK(!witness.function && !witness.name);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        bound->links.type = witness.function;
        verify_types(witness);
        xa_analyzer_storage_close(&analyzer);
        xr_type_pool_unpin(&pin);
        CHECK(!pin.pool && xa_symbol_registry_type_pool(view->registry) == pool);
        XaSymbol *found = xa_scope_lookup(view, "borrowed");
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(found == bound && !found->scope && found->id == 1 && found->links.type == witness.function);
            verify_types(witness);
            XrType *later = xr_pool_alloc_type(pool, XR_KIND_BOOL);
            if (!later) status = xr_compile_state_status(state);
            else CHECK(later->id == 4 && later->kind == XR_KIND_BOOL);
        }
    }
    if (state && status != XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats before = stats(resources); size_t attempts = attempt_count;
        XrTypePoolPin retry = {0}; XaSymbolRegistry *unchanged = NULL;
        CHECK(xr_type_pool_pin(state, (XrTypePool *)(uintptr_t) 1, &retry) == status);
        CHECK(xa_symbol_registry_new(state, (XrTypePool *)(uintptr_t) 1, &unchanged) == status);
        if (pool) CHECK(xr_type_pool_reset(pool) == status);
        XrCompileResourceStats after = stats(resources);
        CHECK(!retry.pool && !unchanged && attempts == attempt_count && before.allocated_bytes == after.allocated_bytes && before.live_bytes == after.live_bytes && before.work == after.work);
    }
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (pending) xa_symbol_free(pending);
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xr_type_pool_unpin(&pin); xr_type_pool_unpin(&pin);
    xa_analyzer_storage_close(&analyzer); xa_scope_free(view);
    xr_compile_session_free(session);
    if (!producer_closed) xr_compile_resources_release(resources);
    CHECK(attempts == attempt_count && !physical_live && !allocation_count);
    return status;
}
static void matrix(bool dead) {
    reset(); boundary_count = 0; record_work = true;
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(dead, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    record_work = false; size_t sites = attempt_count, cuts = boundary_count;
    CHECK(sites && cuts);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = exact;
        uint64_t *limit = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *limit = (uint64_t) ((int64_t) *limit + delta);
        reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &limits, &observed) == (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i = 0; i < sites; ++i) {
        reset(); fail_at = i; XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset(); XrCompileResourceLimits limits = ample; limits.work = boundaries[i] - 1;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("registry/type-pool dead%d: %zu actual allocation faults/%zu actual work cuts; axes/sticky/atomic/physical0 PASS\n", dead, sites, cuts);
}

static void pin_only_lifetime_and_reset(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompileState *state = NULL;
        XrTypePool *pool = NULL; XrTypePoolPin pin = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources, &state) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_type_pool_open(state, &pool) == XR_COMPILE_RESOURCE_OK);
        TypeWitness witness = {0}; CHECK(build_types(pool, &witness) == XR_COMPILE_RESOURCE_OK);
        if (mode == 0) {
            CHECK(xr_type_pool_reset(pool) == XR_COMPILE_RESOURCE_OK && pool->next_type_id == 1);
            XrType *fresh = xr_pool_alloc_type(pool, XR_KIND_BOOL); CHECK(fresh && fresh->id == 1);
        } else {
            CHECK(xr_type_pool_pin(state, pool, &pin) == XR_COMPILE_RESOURCE_OK);
            if (mode >= 2) { xr_type_pool_free(pool); xr_compile_state_release(state); xr_compile_resources_release(resources); }
            if (mode == 3) {
                XrTypePoolPin next = {0};
                CHECK(xr_type_pool_pin(pool->state, pool, &next) == XR_COMPILE_RESOURCE_OK);
                xr_type_pool_unpin(&pin); pin = next; next.pool = NULL;
                XrType *later = xr_pool_alloc_type(pool, XR_KIND_BOOL); CHECK(later && later->id == 4);
            }
            XrArena saved; memcpy(&saved, &pool->arena, sizeof(saved));
            uint32_t next_id = pool->next_type_id;
            XrCompileResourceStats before = stats(resources);
            CHECK(xr_type_pool_reset(pool) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            XrCompileResourceStats after = stats(resources);
            CHECK(!memcmp(&saved, &pool->arena, sizeof(saved)) && next_id == pool->next_type_id);
            CHECK(before.allocated_bytes == after.allocated_bytes && before.live_bytes == after.live_bytes && before.work == after.work);
            verify_types(witness);
        }
        if (mode < 2) { xr_type_pool_free(pool); xr_compile_state_release(state); xr_compile_resources_release(resources); }
        xr_type_pool_unpin(&pin); xr_type_pool_unpin(&pin);
        CHECK(!physical_live && !allocation_count);
    }
    puts("pin-only storage survives creator/state/ledger; later pin and growth; pinned/closed reset preserves original storage; unique creator reset PASS");
}
static void exact_state_and_nonempty_output_guards(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompileState *first = NULL, *second = NULL;
        XrTypePool *pool = NULL; XrTypePoolPin pin = {0}; XaSymbolRegistry *registry = NULL;
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources, &first) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources, &second) == XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_type_pool_open(second, &pool) == XR_COMPILE_RESOURCE_OK);
        if (mode == 0) CHECK(xr_type_pool_pin(first, pool, &pin) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && !pin.pool);
        else if (mode == 1) CHECK(xa_symbol_registry_new(first, pool, &registry) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && !registry);
        else if (mode == 2) {
            CHECK(xr_type_pool_pin(second, pool, &pin) == XR_COMPILE_RESOURCE_OK);
            CHECK(xr_type_pool_pin(second, (XrTypePool *)(uintptr_t) 1, &pin) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && pin.pool == pool);
        } else {
            CHECK(xa_symbol_registry_new(second, pool, &registry) == XR_COMPILE_RESOURCE_OK);
            XaSymbolRegistry *old = registry;
            CHECK(xa_symbol_registry_new(second, (XrTypePool *)(uintptr_t) 1, &registry) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && registry == old);
        }
        if (mode < 2) CHECK(xr_compile_state_status(second) == XR_COMPILE_RESOURCE_OK && xr_pool_alloc_type(pool, XR_KIND_INT));
        xr_type_pool_unpin(&pin); xa_symbol_registry_release(registry);
        xr_type_pool_free(pool); xr_compile_state_release(first); xr_compile_state_release(second); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    puts("same-ledger foreign state rejected without poisoning healthy pool; nonempty pin/registry outputs preserved PASS");
}
int main(void) {
    matrix(false); matrix(true); pin_only_lifetime_and_reset(); exact_state_and_nonempty_output_guards();
    return 0;
}
