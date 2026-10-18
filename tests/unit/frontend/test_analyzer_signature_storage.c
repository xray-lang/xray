/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_signature_storage.c - Real owned nonempty metadata transactions
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "runtime/value/xtype_pool.h"
#include "frontend/analyzer/xanalyzer_signature.h"
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
static XrCompileResourceStatus seed_signature(XaSymbolLinks *links, XrType *leaf,
                                             XrType *array, AstNode *fallback) {
    XrCompileState *state = links->resource_state;
    char name[] = "value"; const char *names[] = {name, NULL}; XrType *types[] = {leaf, array};
    XaSymbolLinks before; memcpy(&before, links, sizeof(before));
    XrCompileResourceStatus status = xa_symbol_links_set_function_sig(links, types, names, 2, array);
    memset(name, '!', sizeof(name) - 1); types[0] = NULL;
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->param_count == 2 && links->param_types[0] == leaf && links->param_types[1] == array);
    CHECK(!strcmp(links->param_names[0], "value") && !links->param_names[1]);
    char a[] = "First", b[] = "Second"; const char *generic_names[] = {a, b};
    XrType *first[] = {leaf, NULL}, *second[] = {array};
    XrType **constraints[] = {first, second}; int counts[] = {2, 1};
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_type_params(links, generic_names, constraints, counts, 2);
    memset(a, '?', sizeof(a) - 1); memset(b, '?', sizeof(b) - 1); first[0] = NULL; second[0] = NULL;
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    int count = 0;
    CHECK(xa_symbol_links_get_type_param_count(links) == 2 && !strcmp(xa_symbol_links_get_type_param_name(links, 0), "First"));
    CHECK(!strcmp(xa_symbol_links_get_type_param_name(links, 1), "Second"));
    XrType **got = xa_symbol_links_get_type_param_constraints(links, 0, &count);
    CHECK(got && count == 2 && got[0] == leaf && !got[1]);
    got = xa_symbol_links_get_type_param_constraints(links, 1, &count);
    CHECK(got && count == 1 && got[0] == array);
    AstNode *defaults[] = {fallback, NULL};
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_param_defaults(links, defaults, 2);
    if (status == XR_COMPILE_RESOURCE_OK)
        CHECK(links->param_defaults != defaults && links->param_defaults[0] == fallback && !links->param_defaults[1]);
    if (status != XR_COMPILE_RESOURCE_OK) unchanged(&before, links);
    CHECK(xr_compile_state_status(state) == status);
    return status;
}
static XrCompileResourceStatus self_alias_replacements(XaSymbolLinks *links, AstNode *fallback) {
    XaSymbolLinks before;
    memcpy(&before, links, sizeof(before));
    XrCompileResourceStatus status = xa_symbol_links_set_param_defaults(links, links->param_defaults, links->param_count);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->param_defaults && links->param_defaults[0] == fallback && !links->param_defaults[1]);
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_function_sig(links, links->param_types, links->param_names,
                                              links->param_count, links->return_type);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(!links->param_defaults && links->param_count == 2 && !strcmp(links->param_names[0], "value"));
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_type_params(links, links->type_param_names, links->type_param_constraints,
                                             links->type_param_constraint_counts, links->type_param_count);
    if (status != XR_COMPILE_RESOURCE_OK) { unchanged(&before, links); return status; }
    CHECK(links->type_param_count == 2 && links->type_param_constraint_counts[0] == 2 && !links->type_param_constraints[0][1]);
    AstNode *defaults[] = {fallback, NULL};
    memcpy(&before, links, sizeof(before));
    status = xa_symbol_links_set_param_defaults(links, defaults, 2);
    if (status != XR_COMPILE_RESOURCE_OK) unchanged(&before, links);
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
            "fn identity<T>(value: T) -> T { return value; }\nconst fallback = 7;\n", &program);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(parsed == XR_PARSE_OK && program && program->as.program.count == 2);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        pending = xa_symbol_new(analyzer.symbol_registry, "signature", XA_SYM_FUNCTION);
        if (!pending) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        status = xa_scope_add_symbol(analyzer.global_scope, pending);
        if (status == XR_COMPILE_RESOURCE_OK) { symbol = pending; pending = NULL; CHECK(symbol->id == 1 && symbol->links.resource_state == state); }
    }
    XrType *leaf = NULL, *array = NULL; AstNode *fallback = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (!leaf) status = xr_compile_state_status(state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        array = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_ARRAY);
        if (!array) status = xr_compile_state_status(state);
        else { array->container.element_type = leaf; CHECK(leaf->id == 1 && array->id == 2); }
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        FunctionDeclNode *fn = &program->as.program.statements[0]->as.function_decl;
        CHECK(fn->type_param_count == 1 && !strcmp(fn->type_params[0]->name, "T"));
        status = xa_symbol_links_set_generic_declarations(&symbol->links, fn->type_params, fn->type_param_count);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(symbol->links.type_param_count == 1 && !strcmp(symbol->links.type_param_names[0], "T"));
        fallback = program->as.program.statements[1]->as.var_decl.initializer;
        CHECK(fallback && fallback->node_id && fallback->line == 2);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = seed_signature(&symbol->links, leaf, array, fallback);
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); producer_closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = self_alias_replacements(&symbol->links, fallback);
    if (status == XR_COMPILE_RESOURCE_OK) {
        int count = 0;
        CHECK(xa_symbol_links_get_param_types(&symbol->links, &count) == symbol->links.param_types && count == 2);
        CHECK(xa_symbol_links_get_param_names(&symbol->links, &count) == symbol->links.param_names && count == 2);
        CHECK(xa_symbol_links_get_return_type(&symbol->links) == array);
        CHECK(array->container.element_type == leaf && leaf->kind == XR_KIND_INT && fallback->line == 2);
    }
    if (symbol && status != XR_COMPILE_RESOURCE_OK) {
        XaSymbolLinks before; memcpy(&before, &symbol->links, sizeof(before));
        size_t attempts = attempt_count; XrCompileResourceStats old = stats(resources);
        CHECK(xa_symbol_links_set_function_sig(&symbol->links, (XrType **)(uintptr_t)1,
            (const char **)(uintptr_t)1, 3, (XrType *)(uintptr_t)1) == status);
        CHECK(xa_symbol_links_set_type_params(&symbol->links, (const char **)(uintptr_t)1,
            (XrType ***)(uintptr_t)1, (const int *)(uintptr_t)1, 3) == status);
        CHECK(xa_symbol_links_set_param_defaults(&symbol->links, (AstNode **)(uintptr_t)1, 2) == status);
        CHECK(xa_symbol_links_set_generic_declarations(&symbol->links, (XrGenericParam **)(uintptr_t)1, 3) == status);
        unchanged(&before, &symbol->links);
        XrCompileResourceStats now = stats(resources);
        CHECK(attempts == attempt_count && old.allocated_bytes == now.allocated_bytes && old.live_bytes == now.live_bytes && old.work == now.work);
    }
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (pending) xa_symbol_free(pending);
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xa_analyzer_storage_close(&analyzer);
    /* The caller's real syntax owner stays alive through every borrowed
     * default use and metadata destruction; no AST pin is being simulated. */
    xr_program_destroy(program);
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
    printf("signature arrays dead%d: %zu actual allocation faults/%zu actual work cuts; axes/sticky/atomic/physical0 PASS\n", dead, sites, cuts);
}

static void invalid_and_empty_inputs(void) {
    for (unsigned mode = 0; mode < 7; ++mode) {
        reset(); XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
        XaAnalyzer analyzer = {0};
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        CHECK(session_open(resources, &session) == XR_COMPILE_RESOURCE_OK);
        XrCompileState *state = xr_compile_session_compile_state(session);
        CHECK(xa_analyzer_storage_open(state, &analyzer) == XR_COMPILE_RESOURCE_OK);
        XaSymbol *symbol = xa_symbol_new(analyzer.symbol_registry, "owner", XA_SYM_FUNCTION);
        CHECK(symbol && xa_scope_add_symbol(analyzer.global_scope, symbol) == XR_COMPILE_RESOURCE_OK);
        XrType *leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT); CHECK(leaf);
        XrType *types[] = {leaf}; const char *names[] = {"kept"};
        CHECK(xa_symbol_links_set_function_sig(&symbol->links, types, names, 1, leaf) == XR_COMPILE_RESOURCE_OK);
        XaSymbolLinks before; memcpy(&before, &symbol->links, sizeof(before));
        XrCompileResourceStatus status;
        if (mode == 0) status = xa_symbol_links_set_function_sig(&symbol->links, NULL, NULL, -1, NULL);
        else if (mode == 1) status = xa_symbol_links_set_type_params(&symbol->links, NULL, NULL, NULL, 1);
        else if (mode == 2) { int count[] = {-1}; status = xa_symbol_links_set_type_params(&symbol->links, names, NULL, count, 1); }
        else if (mode == 3) { int count[] = {1}; status = xa_symbol_links_set_type_params(&symbol->links, names, NULL, count, 1); }
        else if (mode == 4) { AstNode *defaults[] = {NULL, NULL}; status = xa_symbol_links_set_param_defaults(&symbol->links, defaults, 2); }
        else if (mode == 5) status = xa_symbol_links_set_generic_declarations(&symbol->links, NULL, 1);
        else {
            CHECK(xa_symbol_links_set_function_sig(&symbol->links, NULL, NULL, 3, NULL) == XR_COMPILE_RESOURCE_OK);
            CHECK(symbol->links.param_count == 3 && !symbol->links.param_types && !symbol->links.param_names);
            CHECK(xa_symbol_links_set_function_sig(&symbol->links, NULL, NULL, 0, NULL) == XR_COMPILE_RESOURCE_OK);
            CHECK(xa_symbol_links_set_type_params(&symbol->links, NULL, NULL, NULL, 0) == XR_COMPILE_RESOURCE_OK);
            status = xa_symbol_links_set_param_defaults(&symbol->links, NULL, 0);
        }
        if (mode < 6) { CHECK(status == XR_COMPILE_RESOURCE_BAD_ARGUMENT); unchanged(&before, &symbol->links); }
        else CHECK(status == XR_COMPILE_RESOURCE_OK && !symbol->links.param_count && !symbol->links.type_param_count);
        xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session); xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    CHECK(xa_symbol_links_set_function_sig(NULL, (XrType **)(uintptr_t)1, NULL, 1, NULL) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    puts("invalid counts/constraints/default alignment preserve original contents; nullable optional function arrays and empty resets; null authority rejected PASS");
}
int main(void) {
    matrix(false); matrix(true); invalid_and_empty_inputs();
    return 0;
}
