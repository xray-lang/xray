/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_link_metadata.c - Actual auxiliary metadata ownership
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "runtime/value/xtype_pool.h"
#include "frontend/analyzer/xanalyzer_signature.h"
#include "frontend/analyzer/xanalyzer_link_metadata.h"
#include <limits.h>
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

static void check_view(const XaViewReturnContract *view, int count, bool elided) {
    CHECK(view->origin_count == count && view->checked && view->valid && view->was_elided == elided);
    CHECK(view->origins && view->origins[0].kind == (count == 3 ? XR_VIEW_ORIGIN_PARAM : XR_VIEW_ORIGIN_STATIC));
    if (count == 3) {
        CHECK(view->origins[0].param_ordinal == 1);
        CHECK(view->origins[1].kind == XR_VIEW_ORIGIN_RECEIVER && view->origins[1].param_ordinal == -1);
        CHECK(view->origins[2].kind == XR_VIEW_ORIGIN_STATIC && view->origins[2].param_ordinal == -1);
    } else CHECK(count == 1 && view->origins[0].param_ordinal == -1);
}
static XrCompileResourceStatus initial_aux(XaSymbolLinks *links, FunctionDeclNode *declaration,
    int line, XrType *leaf, XrType *array, XrCompileResources *resources) {
    XaSymbolLinks before; memcpy(&before, links, sizeof(before));
    XrAttribute *attrs[] = {NULL, declaration->attributes[0]};
    XrCompileResourceStatus status = xa_symbol_links_set_deprecated_attrs(links, attrs, 2);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->is_deprecated && !strcmp(links->deprecated_message, "use modern"));
    XrViewOrigin rows[] = {{.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 1},
        {.kind = XR_VIEW_ORIGIN_RECEIVER, .param_ordinal = -1},
        {.kind = XR_VIEW_ORIGIN_STATIC, .param_ordinal = -1}};
    XrType type_before; memcpy(&type_before, links->type, sizeof(type_before));
    status = xa_type_set_view_contract(links->resource_state, NULL, links->type, rows, 3, true);
    if (status != XR_COMPILE_RESOURCE_OK) {
        CHECK(!memcmp(&type_before, links->type, sizeof(type_before))); return status;
    }
    CHECK(links->type->function.view_origin_count == 3);
    XaViewReturnContract contract = {.origins = rows, .origin_count = 3, .was_elided = true, .checked = true, .valid = true};
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_view_contract(links, &contract); memset(rows, 0xa5, sizeof(rows));
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->return_view.origins != contract.origins); check_view(&links->return_view, 3, true);
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_ensure_inferred_params(links);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->inferred_param_count == 3 && links->inferred_param_types);
    for (int i = 0; i < 3; ++i) CHECK(!links->inferred_param_types[i]);
    links->inferred_param_types[0] = leaf; links->inferred_param_types[2] = array;
    memcpy(&before, links, sizeof(before));
    XrCompileResourceStats prior = stats(resources); size_t attempts = attempt_count;
    CHECK(xa_symbol_links_ensure_inferred_params(links) == XR_COMPILE_RESOURCE_OK); unchanged(&before, links);
    XrCompileResourceStats after = stats(resources);
    CHECK(prior.work == after.work && prior.allocated_bytes == after.allocated_bytes && prior.live_bytes == after.live_bytes && attempts == attempt_count);
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(&before, links, sizeof(before));
        status = xa_symbol_add_ref(links, (uint32_t) line + i, 2 + i, 7 + i, i != 0);
        if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    }
    int count = 0; XaRefLocation *ref = xa_symbol_get_refs(links, &count);
    CHECK(count == 2 && ref && ref->is_write && ref->line == (uint32_t) line + 1);
    CHECK(ref->column == 3 && ref->end_column == 8 && ref->next && !ref->next->is_write && !ref->next->next);
    return status;
}
static XrCompileResourceStatus replace_aux(XaSymbolLinks *links, XrType *leaf) {
    XaSymbolLinks before; memcpy(&before, links, sizeof(before));
    char input[] = "legacy message";
    XrCompileResourceStatus status = xa_symbol_links_set_deprecated(links, true, input);
    memset(input, '!', sizeof(input) - 1);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(!strcmp(links->deprecated_message, "legacy message"));
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_deprecated(links, false, links->deprecated_message + 7);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(!links->is_deprecated && !strcmp(links->deprecated_message, "message"));
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_view_contract(links, &links->return_view);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    check_view(&links->return_view, 3, true);
    XrType *one[] = {leaf}; XrType *type = xr_type_new_function(NULL, one, 1, leaf, false);
    if (!type) return xr_compile_state_status(links->resource_state);
    links->type = type;
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_function_sig(links, links->param_types, links->param_names, 1, leaf);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->param_count == 1 && !links->inferred_param_types && !links->inferred_param_count);
    CHECK(!links->return_view.origins && !links->return_view.origin_count && !links->return_view.checked && !links->return_view.valid);
    CHECK(!strcmp(links->deprecated_message, "message") && links->ref_count == 2);
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_ensure_inferred_params(links);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->inferred_param_count == 1 && !links->inferred_param_types[0]); links->inferred_param_types[0] = leaf;
    XrViewOrigin origin = {.kind = XR_VIEW_ORIGIN_STATIC, .param_ordinal = -1};
    XaViewReturnContract contract = {.origins = &origin, .origin_count = 1, .checked = true, .valid = true};
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_view_contract(links, &contract);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    check_view(&links->return_view, 1, false);
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_add_ref(links, 99, 0, UINT32_MAX, true);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    int count = 0; XaRefLocation *ref = xa_symbol_get_refs(links, &count);
    CHECK(count == 3 && ref->line == 99 && !ref->column && ref->end_column == UINT32_MAX);
    return status;
}
static XrCompileResourceStatus pipeline(bool dead, const XrCompileResourceLimits *limits,
                                      XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0}; AstNode *program = NULL; XaSymbol *pending = NULL, *symbol = NULL;
    bool producer_closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) status = session_open(resources, &session);
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_analyzer_storage_open(state, &analyzer);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrParseStatus parsed = xr_compile_parse(session,
            "@deprecated(\"use modern\")\nfn old(left: i64, middle: i64, right: i64) -> i64 { return left; }\n", &program);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(parsed == XR_PARSE_OK && program && program->as.program.count == 1);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        pending = xa_symbol_new(analyzer.symbol_registry, "metadata", XA_SYM_FUNCTION);
        if (!pending) status = xr_compile_state_status(state);
        else {
            status = xa_scope_add_symbol(analyzer.global_scope, pending);
            if (status == XR_COMPILE_RESOURCE_OK) { symbol = pending; pending = NULL; CHECK(symbol->id == 1 && symbol->links.resource_state == state); }
        }
    }
    XrType *leaf = NULL, *array = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        xr_type_set_current_pool(analyzer.type_pool, &analyzer.type_pool->next_type_id);
        leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (!leaf) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        array = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_ARRAY);
        if (!array) status = xr_compile_state_status(state);
        else { array->container.element_type = leaf; CHECK(leaf->id == 1 && array->id == 2); }
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrType *types[] = {leaf, array, leaf}; const char *names[] = {"left", "middle", "right"};
        XrType *type = xr_type_new_function(NULL, types, 3, leaf, false);
        if (!type) status = xr_compile_state_status(state);
        else { symbol->links.type = type; status = xa_symbol_links_set_function_sig(&symbol->links, types, names, 3, leaf); }
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        AstNode *node = program->as.program.statements[0]; FunctionDeclNode *fn = &node->as.function_decl;
        CHECK(node->node_id && fn->attr_count == 1 && fn->attributes[0]->kind == ATTR_DEPRECATED);
        status = initial_aux(&symbol->links, fn, node->line, leaf, array, resources);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        xr_program_destroy(program); program = NULL;
        CHECK(!strcmp(symbol->links.deprecated_message, "use modern"));
    }
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); producer_closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = replace_aux(&symbol->links, leaf);
    if (symbol && status != XR_COMPILE_RESOURCE_OK) {
        XaSymbolLinks before; memcpy(&before, &symbol->links, sizeof(before));
        size_t attempts = attempt_count; XrCompileResourceStats prior = stats(resources);
        CHECK(xa_symbol_links_set_view_contract(&symbol->links, (const XaViewReturnContract *)(uintptr_t)1) == status);
        CHECK(xa_symbol_links_ensure_inferred_params(&symbol->links) == status);
        CHECK(xa_symbol_links_set_deprecated(&symbol->links, true, (const char *)(uintptr_t)1) == status);
        CHECK(xa_symbol_links_set_deprecated_attrs(&symbol->links, (XrAttribute **)(uintptr_t)1, 2) == status);
        CHECK(xa_symbol_add_ref(&symbol->links, 1, 2, 3, false) == status);
        CHECK(xa_type_set_view_contract(state, NULL, (XrType *)(uintptr_t)1, (const XrViewOrigin *)(uintptr_t)1, 2, true) == status);
        unchanged(&before, &symbol->links);
        int count = -1; CHECK(xa_symbol_get_refs(&symbol->links, &count) == before.references && count == before.ref_count);
        XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && prior.allocated_bytes == after.allocated_bytes && prior.live_bytes == after.live_bytes && prior.work == after.work);
    }
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (pending) xa_symbol_free(pending);
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xr_type_set_current_pool(NULL, NULL);
    xa_analyzer_storage_close(&analyzer); xr_program_destroy(program); xr_compile_session_free(session);
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
    printf("aux metadata dead%d: %zu actual allocation faults/%zu actual work cuts; axes/sticky/atomic/physical0 PASS\n", dead, sites, cuts);
}

static void guards_and_empty(void) {
    for (int mode = 0; mode < 7; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL; XaAnalyzer analyzer = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
        XaSymbol *symbol = xa_symbol_new(analyzer.symbol_registry, "owner", XA_SYM_FUNCTION);
        CHECK(symbol && xa_scope_add_symbol(analyzer.global_scope, symbol) == XR_COMPILE_RESOURCE_OK);
        XrType *leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT); CHECK(leaf);
        XrType *types[] = {leaf}; const char *names[] = {"kept"};
        CHECK(xa_symbol_links_set_function_sig(&symbol->links, types, names, 1, leaf) == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_symbol_links_ensure_inferred_params(&symbol->links) == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_symbol_links_set_deprecated(&symbol->links, true, "kept") == XR_COMPILE_RESOURCE_OK);
        CHECK(xa_symbol_add_ref(&symbol->links, 7, 9, 12, false) == XR_COMPILE_RESOURCE_OK);
        if (mode == 4) symbol->links.inferred_param_count = 2;
        if (mode == 5) symbol->links.ref_count = INT_MAX;
        XaSymbolLinks before; memcpy(&before, &symbol->links, sizeof(before));
        XrCompileResourceStatus status;
        if (mode < 2) {
            XaViewReturnContract bad = {.origin_count = mode ? 1 : -1};
            status = xa_symbol_links_set_view_contract(&symbol->links, &bad);
        } else if (mode < 4) status = xa_symbol_links_set_deprecated_attrs(&symbol->links, NULL, mode == 2 ? -1 : 1);
        else if (mode == 4) status = xa_symbol_links_ensure_inferred_params(&symbol->links);
        else if (mode == 5) status = xa_symbol_add_ref(&symbol->links, 0, 0, 0, false);
        else {
            CHECK(xa_symbol_links_set_view_contract(&symbol->links, NULL) == XR_COMPILE_RESOURCE_OK);
            CHECK(!symbol->links.return_view.origins && !symbol->links.return_view.origin_count && !symbol->links.return_view.checked);
            CHECK(xa_symbol_links_set_deprecated_attrs(&symbol->links, NULL, 0) == XR_COMPILE_RESOURCE_OK);
            CHECK(!symbol->links.deprecated_message && !symbol->links.is_deprecated);
            CHECK(xa_symbol_links_set_function_sig(&symbol->links, NULL, NULL, 0, NULL) == XR_COMPILE_RESOURCE_OK);
            status = xa_symbol_links_ensure_inferred_params(&symbol->links);
            CHECK(!symbol->links.inferred_param_types && !symbol->links.inferred_param_count);
        }
        if (mode < 6) { CHECK(status == XR_COMPILE_RESOURCE_BAD_ARGUMENT); unchanged(&before, &symbol->links); }
        else CHECK(status == XR_COMPILE_RESOURCE_OK);
        xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    int count = -1; CHECK(!xa_symbol_get_refs(NULL, &count) && count == 0);
    CHECK(xa_symbol_links_set_view_contract(NULL, (const XaViewReturnContract *)(uintptr_t)1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_ensure_inferred_params(NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_set_deprecated(NULL, true, (const char *)(uintptr_t)1) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_links_set_deprecated_attrs(NULL, (XrAttribute **)(uintptr_t)1, 2) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_symbol_add_ref(NULL, 1, 2, 3, true) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xa_type_set_view_contract(NULL, NULL, (XrType *)(uintptr_t)1, (XrViewOrigin *)(uintptr_t)1, 2, false) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    puts("invalid aux shapes/counter preserve contents; empty/idempotent/reset behavior and null authority guards PASS");
}
static void type_status_and_value_consumers(void) {
    for (int mode = 0; mode < 4; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL; XaAnalyzer analyzer = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
        /* These are the real no-isolate value helpers and their actual pool
         * selection protocol, not a substituted analyzer or VM constructor. */
        xr_type_set_current_pool(analyzer.type_pool, &analyzer.type_pool->next_type_id);
        XrType *leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT); CHECK(leaf);
        XrType *params[] = {leaf, leaf}; XrType *fn = xr_type_new_function(NULL, params, 2, leaf, false); CHECK(fn);
        XrViewOrigin rows[] = {{.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 1},
            {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 0}, {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 1}};
        CHECK(xa_type_set_view_contract(state, NULL, fn, rows, 3, true) == XR_COMPILE_RESOURCE_OK);
        CHECK(fn->function.view_origin_count == 2 && fn->function.view_origin_set[0].param_ordinal == 0 && fn->function.view_origin_set[1].param_ordinal == 1);
        XrType before; memcpy(&before, fn, sizeof(before));
        if (mode == 0) {
            XrViewOrigin invalid = {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = 2};
            CHECK(xr_type_function_set_view_origins(NULL, fn, &invalid, 1, false) == XR_VIEW_ORIGIN_BIND_INVALID);
            CHECK(!memcmp(&before, fn, sizeof(before)));
            XrType *copy = xr_type_copy(NULL, fn); CHECK(copy && xr_type_equals(copy, fn));
            XrType *parameter = xr_type_new_type_param(NULL, "T", 0); CHECK(parameter);
            XrType *generic_params[] = {parameter}; XrType *generic = xr_type_new_function(NULL, generic_params, 1, parameter, false); CHECK(generic);
            CHECK(xa_type_set_view_contract(state, NULL, generic, rows + 1, 1, false) == XR_COMPILE_RESOURCE_OK);
            const char *names[] = {"T"}; XrType *actual[] = {leaf};
            XrType *substituted = xr_type_substitute(NULL, generic, names, actual, 1);
            CHECK(substituted && substituted != generic);
            CHECK(substituted->function.params[0].type != parameter &&
                xr_type_equals(substituted->function.params[0].type, leaf));
            CHECK(substituted->function.return_type != parameter &&
                xr_type_equals(substituted->function.return_type, leaf));
            CHECK(substituted->function.view_origin_count == 1 && substituted->function.view_origin_set[0].param_ordinal == 0);
            fail_at = attempt_count;
            CHECK(xr_type_function_set_view_origins(NULL, fn, rows, 3, false) == XR_VIEW_ORIGIN_BIND_OUT_OF_MEMORY);
            CHECK(!memcmp(&before, fn, sizeof(before)) && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
            size_t attempts = attempt_count; XrCompileResourceStats prior = stats(resources);
            CHECK(xr_type_function_set_view_origins(NULL, (XrType *)(uintptr_t)1, (XrViewOrigin *)(uintptr_t)1, 3, false) == XR_VIEW_ORIGIN_BIND_OUT_OF_MEMORY);
            CHECK(attempts == attempt_count && stats(resources).work == prior.work);
            fail_at = SIZE_MAX;
        } else if (mode == 3) {
            XrCompilerSession *foreign_session = NULL;
            CHECK(session_open(resources, &foreign_session) == XR_COMPILE_RESOURCE_OK);
            XrCompileState *foreign = xr_compile_session_compile_state(foreign_session);
            CHECK(foreign != state);
            XrCompileResourceStats prior = stats(resources); size_t attempts = attempt_count;
            CHECK(xa_type_set_view_contract(foreign, NULL, fn, rows, 3, false) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            CHECK(xr_compile_state_status(foreign) == XR_COMPILE_RESOURCE_BAD_ARGUMENT && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
            CHECK(!memcmp(&before, fn, sizeof(before)) && attempts == attempt_count && stats(resources).work == prior.work);
            CHECK(xa_type_set_view_contract(state, NULL, fn, rows, 3, false) == XR_COMPILE_RESOURCE_OK);
            xr_compile_session_free(foreign_session);
        } else {
            if (mode == 2) fail_at = attempt_count;
            XrCompileResourceStatus status = xa_type_set_view_contract(state, NULL, fn, rows, mode == 1 ? -1 : 3, false);
            CHECK(status == (mode == 1 ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
            CHECK(xr_compile_state_status(state) == status && !memcmp(&before, fn, sizeof(before)));
            XrCompileResourceStats prior = stats(resources); size_t attempts = attempt_count;
            CHECK(xa_type_set_view_contract(state, NULL, (XrType *)(uintptr_t)1, (XrViewOrigin *)(uintptr_t)1, 3, false) == status);
            CHECK(stats(resources).work == prior.work && attempt_count == attempts);
        }
        xr_type_set_current_pool(NULL, NULL);
        xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    puts("actual type origin normalization/copy/substitution; owned scratch/copy/sort, typed INVALID vs OOM, atomic old type, exact-state adapter/first-cause/sticky PASS");
}
int main(void) {
    matrix(false); matrix(true); guards_and_empty(); type_status_and_value_consumers();
    return 0;
}
