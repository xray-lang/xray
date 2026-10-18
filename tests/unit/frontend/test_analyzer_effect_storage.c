/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_effect_storage.c - Real owned effect array transactions
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "runtime/value/xtype_pool.h"
#include "frontend/analyzer/xanalyzer_effect_storage.h"
#include "frontend/parser/xparse.h"
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

static void unchanged(const XaSymbolLinks *before, const XaSymbolLinks *after) {
    CHECK(!memcmp(before, after, sizeof(*before)));
}

static void make_effects(XaParamEffectSummary rows[3]) {
    memset(rows, 0, 3 * sizeof(*rows));
    rows[0].formal_mode = XR_PARAM_READ;
    rows[0].capability = XA_CAPABILITY_READONLY;
    rows[0].access = XA_PARAM_ACCESS_READ;
    rows[0].retain = XA_RETAIN_LOCAL_ALIAS;
    rows[0].returns = XA_RETURN_PROVENANCE_ALIAS;
    rows[0].complete = true;
    rows[1].formal_mode = XR_PARAM_REF;
    rows[1].capability = XA_CAPABILITY_EXCLUSIVE_WRITE;
    rows[1].access = XA_PARAM_ACCESS_READ | XA_PARAM_ACCESS_WRITE;
    rows[1].mutation_paths = XA_MUTATION_PATH_WILDCARD;
    rows[1].returns = XA_RETURN_PROVENANCE_BORROWED_PROJECTION;
    rows[1].complete = true;
    rows[2].formal_mode = XR_PARAM_MOVE;
    rows[2].capability = XA_CAPABILITY_UNIQUE_OWNER;
    rows[2].escapes = XA_ESCAPE_CLOSURE | XA_ESCAPE_CROSS_EXECUTION;
    rows[2].retain = XA_RETAIN_CONST_OR_SYNC;
    rows[2].storage_domain = XR_STORAGE_TRANSFERABLE;
}
static void expect_effects(const XaParamEffectSummary *actual, int count,
                           const XaParamEffectSummary *expected, int expected_count) {
    CHECK(count == expected_count && (!count || actual));
    if (count) CHECK(!memcmp(actual, expected, (size_t) count * sizeof(*actual)));
}
static XrCompileResourceStatus seed_effects(XaSymbolLinks *links) {
    XaParamEffectSummary input[3], expected[3]; make_effects(input); make_effects(expected);
    XaSymbolLinks before; memcpy(&before, links, sizeof(before));
    XrCompileResourceStatus status = xa_symbol_links_set_param_effect_summaries(links, input, 3);
    memset(input, 0xa5, sizeof(input));
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->param_effects != input);
    expect_effects(links->param_effects, links->param_effect_count, expected, 3);
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_return_function_effect_summary(links, links);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->return_fn_param_effects != links->param_effects && !links->return_fn_effect_mixed);
    expect_effects(links->return_fn_param_effects, links->return_fn_param_effect_count, expected, 3);
    links->return_fn_effect_mixed = true;
    links->return_fn_effect_scanned = true;
    links->return_fn_effect_scan_in_progress = true;
    return status;
}
static XrCompileResourceStatus replace_effects(XaSymbolLinks *dst, XaSymbolLinks *src) {
    XaParamEffectSummary expected[3]; make_effects(expected);
    XaSymbolLinks before;
    memcpy(&before, dst, sizeof(before));
    XrCompileResourceStatus status = xa_symbol_links_copy_param_effect_summaries(dst, src);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    CHECK(dst->param_effects != src->param_effects);
    expect_effects(dst->param_effects, dst->param_effect_count, expected, 3);
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_copy_param_effect_summaries(dst, dst);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    expect_effects(dst->param_effects, dst->param_effect_count, expected, 3);
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_set_return_function_effect_summary(dst, dst);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    CHECK(!dst->return_fn_effect_mixed && dst->return_fn_effect_scanned && dst->return_fn_effect_scan_in_progress);
    expect_effects(dst->return_fn_param_effects, dst->return_fn_param_effect_count, expected, 3);
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_copy_return_function_effect_summary(dst, src);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    CHECK(dst->return_fn_param_effects != src->return_fn_param_effects);
    CHECK(dst->return_fn_effect_mixed && dst->return_fn_effect_scanned && !dst->return_fn_effect_scan_in_progress);
    expect_effects(dst->return_fn_param_effects, dst->return_fn_param_effect_count, expected, 3);
    dst->return_fn_effect_scan_in_progress = true;
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_copy_return_function_effect_summary(dst, dst);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    CHECK(dst->return_fn_effect_mixed && dst->return_fn_effect_scanned && !dst->return_fn_effect_scan_in_progress);
    expect_effects(dst->return_fn_param_effects, dst->return_fn_param_effect_count, expected, 3);
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_copy_return_function_effect_to_param_summaries(dst, dst);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    CHECK(dst->param_effects != dst->return_fn_param_effects);
    expect_effects(dst->param_effects, dst->param_effect_count, expected, 3);
    memcpy(&before, dst, sizeof(before));
    status = xa_symbol_links_set_param_effect_summaries(dst, dst->param_effects + 1, 2);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, dst); return status; }
    expect_effects(dst->param_effects, dst->param_effect_count, expected + 1, 2);
    expect_effects(src->param_effects, src->param_effect_count, expected, 3);
    return status;
}
static XrCompileResourceStatus pipeline(bool dead, const XrCompileResourceLimits *limits,
                                      XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0}; XaSymbol *source = NULL, *destination = NULL, *pending = NULL;
    bool producer_closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) status = session_open(resources, &session);
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_analyzer_storage_open(state, &analyzer);
    for (int i = 0; i < 2 && status == XR_COMPILE_RESOURCE_OK; ++i) {
        pending = xa_symbol_new(analyzer.symbol_registry, i ? "destination" : "source", XA_SYM_FUNCTION);
        if (!pending) { status = xr_compile_state_status(state); break; }
        status = xa_scope_add_symbol(analyzer.global_scope, pending);
        if (status == XR_COMPILE_RESOURCE_OK) {
            CHECK(pending->id == (uint32_t) i + 1 && pending->links.resource_state == state);
            if (i) destination = pending; else source = pending;
            pending = NULL;
        }
    }
    XrType *leaf = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (!leaf) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(leaf->id == 1); XrType *types[] = {leaf, leaf, leaf}; const char *names[] = {"read", "write", "move"};
        status = xa_symbol_links_set_function_sig(&source->links, types, names, 3, leaf);
        if (status == XR_COMPILE_RESOURCE_OK)
            status = xa_symbol_links_set_function_sig(&destination->links, types, names, 3, leaf);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = seed_effects(&source->links);
    if (status == XR_COMPILE_RESOURCE_OK) status = seed_effects(&destination->links);
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); producer_closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = replace_effects(&destination->links, &source->links);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(destination->links.param_types[0] == leaf && leaf->kind == XR_KIND_INT);
        CHECK(!strcmp(source->links.param_names[2], "move"));
    }
    XaSymbol *victim = destination ? destination : source;
    if (victim && status != XR_COMPILE_RESOURCE_OK) {
        XaSymbolLinks before; memcpy(&before, &victim->links, sizeof(before));
        size_t attempts = attempt_count; XrCompileResourceStats prior = stats(resources);
        const XaSymbolLinks *bad = (const XaSymbolLinks *)(uintptr_t)1;
        CHECK(xa_symbol_links_set_param_effect_summaries(&victim->links, (const XaParamEffectSummary *)(uintptr_t)1, 3) == status);
        CHECK(xa_symbol_links_copy_param_effect_summaries(&victim->links, bad) == status);
        CHECK(xa_symbol_links_set_return_function_effect_summary(&victim->links, bad) == status);
        CHECK(xa_symbol_links_copy_return_function_effect_summary(&victim->links, bad) == status);
        CHECK(xa_symbol_links_copy_return_function_effect_to_param_summaries(&victim->links, bad) == status);
        unchanged(&before, &victim->links); XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && prior.work == after.work && prior.allocated_bytes == after.allocated_bytes && prior.live_bytes == after.live_bytes);
    }
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (pending) xa_symbol_free(pending);
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session);
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
    printf("effect arrays dead%d: %zu actual allocation faults/%zu actual work cuts; axes/sticky/atomic/physical0 PASS\n", dead, sites, cuts);
}

static void invalid_and_empty_inputs(void) {
    for (int mode = 0; mode < 4; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL; XaAnalyzer analyzer = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
        XaSymbol *symbol = xa_symbol_new(analyzer.symbol_registry, "owner", XA_SYM_FUNCTION);
        CHECK(symbol && xa_scope_add_symbol(analyzer.global_scope, symbol) == XR_COMPILE_RESOURCE_OK);
        CHECK(seed_effects(&symbol->links) == XR_COMPILE_RESOURCE_OK);
        XaSymbolLinks before; memcpy(&before, &symbol->links, sizeof(before));
        XrCompileResourceStatus status;
        if (mode == 0) status = xa_symbol_links_set_param_effect_summaries(&symbol->links, NULL, -1);
        else if (mode == 1) status = xa_symbol_links_set_param_effect_summaries(&symbol->links, NULL, 1);
        else if (mode == 2) {
            CHECK(xa_symbol_links_copy_param_effect_summaries(&symbol->links, NULL) == XR_COMPILE_RESOURCE_OK);
            CHECK(!symbol->links.param_effects && !symbol->links.param_effect_count);
            status = xa_symbol_links_copy_return_function_effect_summary(&symbol->links, NULL);
            CHECK(!symbol->links.return_fn_param_effects && !symbol->links.return_fn_param_effect_count);
            CHECK(!symbol->links.return_fn_effect_mixed && !symbol->links.return_fn_effect_scanned && !symbol->links.return_fn_effect_scan_in_progress);
        } else {
            CHECK(xa_symbol_links_set_return_function_effect_summary(&symbol->links, NULL) == XR_COMPILE_RESOURCE_OK);
            CHECK(!symbol->links.return_fn_param_effects && !symbol->links.return_fn_param_effect_count && !symbol->links.return_fn_effect_mixed);
            CHECK(symbol->links.return_fn_effect_scanned && symbol->links.return_fn_effect_scan_in_progress);
            status = xa_symbol_links_copy_return_function_effect_to_param_summaries(&symbol->links, NULL);
            CHECK(!symbol->links.param_effects && !symbol->links.param_effect_count);
        }
        if (mode < 2) { CHECK(status == XR_COMPILE_RESOURCE_BAD_ARGUMENT); unchanged(&before, &symbol->links); }
        else CHECK(status == XR_COMPILE_RESOURCE_OK);
        xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    const XaSymbolLinks *bad = (const XaSymbolLinks *)(uintptr_t)1;
    CHECK(xa_symbol_links_copy_param_effect_summaries(NULL, bad) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_set_return_function_effect_summary(NULL, bad) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_copy_return_function_effect_summary(NULL, bad) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_copy_return_function_effect_to_param_summaries(NULL, bad) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    puts("invalid effect inputs preserve contents; empty sources clear arrays with original flag semantics; null authority rejected PASS");
}
int main(void) {
    matrix(false); matrix(true); invalid_and_empty_inputs();
    return 0;
}
