/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_exports.c - Actual export collection on an owned source graph
 */
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/analyzer/xanalyzer_exports.h"
#include "frontend/analyzer/xanalyzer_storage.h"
#include "module/xmodule_graph.h"
#include "frontend/parser/xast_nodes.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool recording;
static uint64_t boundaries[32768];
static size_t boundary_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (recording && status == XR_COMPILE_RESOURCE_OK && units) {
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
static XrCompileResourceStatus module_result(XrCompileState *state, XrModuleStatus status) {
    if (status == XR_MODULE_OK) return xr_compile_state_status(state);
    CHECK(status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY);
    return xr_compile_state_fail(state, status == XR_MODULE_BUDGET
        ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
}
static XrCompileResourceStatus io_result(XrCompileState *state, XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return xr_compile_state_status(state);
    CHECK(status == XR_OS_IO_BUDGET || status == XR_OS_IO_OUT_OF_MEMORY);
    return xr_compile_state_fail(state, status == XR_OS_IO_BUDGET
        ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
}
static XrCompileResourceStatus bind_symbol(XaAnalyzer *analyzer, XaScope *scope,
    const char *name, XaSymbolKind kind, XrType *type, bool native, XaSymbol **output) {
    XaSymbol *symbol = xa_symbol_new(analyzer->symbol_registry, name, kind);
    if (!symbol) return xr_compile_state_status(xa_symbol_registry_state(analyzer->symbol_registry));
    symbol->links.type = type;
    symbol->is_builtin = native; symbol->is_exported = native;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    if (kind == XA_SYM_CLASS) {
        symbol->links.class_info = xa_class_info_new(symbol->links.resource_state, name);
        symbol->links.owns_class_info = symbol->links.class_info != NULL;
        status = xr_compile_state_status(symbol->links.resource_state);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_scope_add_symbol(scope, symbol);
    if (status != XR_COMPILE_RESOURCE_OK) xa_symbol_free(symbol);
    else *output = symbol;
    return status;
}
static XrCompileResourceStatus collect(XaAnalyzer *analyzer, XrModuleSpec *spec,
    XaScope *scope, XrHashMap *guard, uint32_t expected) {
    analyzer->current_scope = scope; analyzer->current_file = spec->source_path;
    analyzer->current_module_identity = spec->canonical;
    XrHashMap *output = guard; bool prior_invalid = spec->export_symbols_invalid;
    bool result = xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &output);
    XrCompileResourceStatus status = xr_compile_state_status(xa_symbol_registry_state(analyzer->symbol_registry));
    if (!result) {
        CHECK(status != XR_COMPILE_RESOURCE_OK && output == guard);
        CHECK(spec->export_symbols_invalid == prior_invalid && !spec->export_symbols);
        return status;
    }
    CHECK(status == XR_COMPILE_RESOURCE_OK && output && output != guard);
    CHECK(xr_hashmap_count(output) == expected && !spec->export_symbols_invalid);
    spec->export_symbols = output;
    return status;
}
static void reject_poison(XaAnalyzer *analyzer, XrModuleSpec *spec,
    XaScope *scope, XrHashMap *guard) {
    analyzer->current_scope = scope; analyzer->current_file = spec->source_path;
    analyzer->current_module_identity = spec->canonical;
    XrHashMap *output = guard;
    CHECK(!xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &output));
    CHECK(output == guard && spec->export_symbols_invalid && !spec->export_symbols);
    CHECK(xr_compile_state_status(xa_symbol_registry_state(analyzer->symbol_registry)) == XR_COMPILE_RESOURCE_OK);
    CHECK(analyzer->diagnostic_count > 0); xa_analyzer_clear_diagnostics(analyzer);
}
static XrCompileResourceStatus fail_recollection(XaAnalyzer *analyzer, XrModuleSpec *spec,
    XaScope *scope, XrHashMap *guard) {
    analyzer->current_scope = scope; analyzer->current_file = spec->source_path;
    analyzer->current_module_identity = spec->canonical;
    XrHashMap *prior = spec->export_symbols; CHECK(prior && prior->capacity == 16);
    XrHashMap header = *prior; XrHashMapEntry entries[16]; memcpy(entries, prior->entries, sizeof(entries));
    bool invalid = spec->export_symbols_invalid; XrHashMap *output = guard;
    fail_at = attempt_count;
    CHECK(!xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &output));
    CHECK(output == guard && spec->export_symbols == prior && spec->export_symbols_invalid == invalid);
    CHECK(!memcmp(&header, prior, sizeof(header)) && !memcmp(entries, prior->entries, sizeof(entries)));
    CHECK(xr_compile_state_status(xa_symbol_registry_state(analyzer->symbol_registry)) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
}
static XrCompileResourceStatus lookup(XrCompileState *state, XrHashMap *map,
    const char *name, XaSymbol *expected) {
    void *value = (void *)(uintptr_t)1;
    XrCompileResourceStatus status = io_result(state, xr_hashmap_owned_get(map, name, &value));
    if (status == XR_COMPILE_RESOURCE_OK) CHECK(value == expected);
    return status;
}
static XrCompileResourceStatus pipeline(bool dead, bool observe, int negative,
    const XrCompileResourceLimits *limits, XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XaAnalyzer analyzer = {0}; XrModuleResolver *resolver = NULL; XrModuleGraph *graph = NULL;
    XrHashMap *guard = NULL; bool closed = false;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrCompilerSessionStatus opened = xr_compile_session_new(resources, &session);
        CHECK(opened == XR_COMPILER_SESSION_OK || opened == XR_COMPILER_SESSION_BUDGET || opened == XR_COMPILER_SESSION_OUT_OF_MEMORY);
        status = opened == XR_COMPILER_SESSION_OK ? XR_COMPILE_RESOURCE_OK :
            opened == XR_COMPILER_SESSION_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    XrCompileState *state = session ? xr_compile_session_compile_state(session) : NULL;
    analyzer.compiler_session = session;
    if (status == XR_COMPILE_RESOURCE_OK) status = xa_analyzer_storage_open(state, &analyzer);
    XrModuleResolverConfig config = {0};
    if (status == XR_COMPILE_RESOURCE_OK) status = module_result(state, xr_compile_module_resolver_new(resources, &config, &resolver));
    const char *root_source = "export { actual as renamed } from \"./export_base\"\nexport * from \"./export_star\"\nexport fn own() -> i64 { return 3; }\n";
    const char *base_source = "export fn actual() -> i64 { return 7; }\nfn hidden() -> i64 { return 9; }\n";
    const char *star_source = "export * from \"./export_base\"\n";
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TEST_EXPORT_ROOT};
    char root_path[] = XR_TEST_EXPORT_ROOT "/export_root.xr";
    XrModuleOverlayInput inputs[] = {
        {authority, "export_root.xr", root_path, root_source, strlen(root_source)},
        {authority, "export_base.xr", XR_TEST_EXPORT_ROOT "/export_base.xr", base_source, strlen(base_source)},
        {authority, "export_star.xr", XR_TEST_EXPORT_ROOT "/export_star.xr", star_source, strlen(star_source)}};
    if (status == XR_COMPILE_RESOURCE_OK) status = module_result(state, xr_compile_module_resolver_set_overlay(resolver, inputs, 3));
    if (status == XR_COMPILE_RESOURCE_OK) status = module_result(state, xr_compile_module_graph_new(resources, session, resolver, &graph));
    char *error = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) status = module_result(state, xr_compile_module_graph_build_logical_source(graph, &authority,
        "export_root.xr", root_path, root_source, &error));
    xr_compile_resources_free(error);
    XrModuleSpec *base = NULL, *star = NULL, *root = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(graph->spec_count == 3);
        for (int i = 0; i < graph->spec_count; ++i) {
            if (!strcmp(graph->specs[i].logical_path, "export_base.xr")) base = &graph->specs[i];
            if (!strcmp(graph->specs[i].logical_path, "export_star.xr")) star = &graph->specs[i];
            if (!strcmp(graph->specs[i].logical_path, "export_root.xr")) root = &graph->specs[i];
        }
        CHECK(base && star && root && base->ast && star->ast && root->ast);
        analyzer.graph = graph;
    }
    XaScope *base_scope = NULL, *star_scope = NULL, *root_scope = NULL;
    XaSymbol *actual = NULL, *own = NULL, *native = NULL, *box = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        base_scope = xa_scope_new(analyzer.symbol_registry, XA_SCOPE_GLOBAL, analyzer.global_scope);
        if (base_scope) star_scope = xa_scope_new(analyzer.symbol_registry, XA_SCOPE_GLOBAL, analyzer.global_scope);
        if (star_scope) root_scope = xa_scope_new(analyzer.symbol_registry, XA_SCOPE_GLOBAL, analyzer.global_scope);
        status = xr_compile_state_status(state);
    }
    XrType *leaf = NULL, *function = NULL;
    if (status == XR_COMPILE_RESOURCE_OK) {
        xr_type_set_current_pool(analyzer.type_pool, &analyzer.type_pool->next_type_id);
        leaf = xr_pool_alloc_type(analyzer.type_pool, XR_KIND_INT);
        if (leaf) function = xr_type_new_function(NULL, NULL, 0, leaf, false);
        status = xr_compile_state_status(state);
        if (status == XR_COMPILE_RESOURCE_OK) CHECK(leaf && function && base_scope && star_scope && root_scope);
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = bind_symbol(&analyzer, base_scope, "actual", XA_SYM_FUNCTION, function, false, &actual);
    if (status == XR_COMPILE_RESOURCE_OK) status = bind_symbol(&analyzer, root_scope, "own", XA_SYM_FUNCTION, function, false, &own);
    if (status == XR_COMPILE_RESOURCE_OK) status = bind_symbol(&analyzer, root_scope, "nativeCall", XA_SYM_FUNCTION, function, true, &native);
    if (status == XR_COMPILE_RESOURCE_OK) status = bind_symbol(&analyzer, root_scope, "NativeBox", XA_SYM_CLASS, NULL, true, &box);
    if (status == XR_COMPILE_RESOURCE_OK) {
        XrOsIoPolicy policy = xr_compile_io_policy(resources);
        status = io_result(state, xr_hashmap_owned_new(&policy, &guard));
    }
    if (status == XR_COMPILE_RESOURCE_OK && negative == 1) {
        actual->links.declared_type = xr_type_new_error(NULL); CHECK(actual->links.declared_type);
        reject_poison(&analyzer, base, base_scope, guard); actual->links.declared_type = NULL;
    }
    recording = observe;
    if (status == XR_COMPILE_RESOURCE_OK) status = collect(&analyzer, base, base_scope, guard, 1);
    if (status == XR_COMPILE_RESOURCE_OK && negative == 3) {
        base->export_symbols_invalid = true; reject_poison(&analyzer, star, star_scope, guard);
        base->export_symbols_invalid = false;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = collect(&analyzer, star, star_scope, guard, 1);
    if (status == XR_COMPILE_RESOURCE_OK && negative == 2) {
        native->links.declared_type = xr_type_new_error(NULL); CHECK(native->links.declared_type);
        reject_poison(&analyzer, root, root_scope, guard); native->links.declared_type = NULL;
    }
    if (status == XR_COMPILE_RESOURCE_OK && negative == 4) {
        char *path = root->source_path; root->source_path = NULL;
        reject_poison(&analyzer, root, root_scope, guard); root->source_path = path;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = collect(&analyzer, root, root_scope, guard, 5);
    if (status == XR_COMPILE_RESOURCE_OK && negative == 5) {
        native->links.declared_type = xr_type_new_error(NULL); CHECK(native->links.declared_type);
        reject_poison(&analyzer, root, root_scope, guard); native->links.declared_type = NULL;
        status = collect(&analyzer, root, root_scope, guard, 5);
    }
    if (status == XR_COMPILE_RESOURCE_OK && negative == 6)
        status = fail_recollection(&analyzer, root, root_scope, guard);
    recording = false;
    if (status == XR_COMPILE_RESOURCE_OK && dead) {
        xr_compile_session_free(session); session = NULL;
        xr_compile_resources_release(resources); closed = true;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "renamed", actual);
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "actual", actual);
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "own", own);
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "nativeCall", native);
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "NativeBox", box);
    if (status == XR_COMPILE_RESOURCE_OK) status = lookup(state, root->export_symbols, "hidden", NULL);
    if (state) CHECK(xr_compile_state_status(state) == status);
    if (analyzer.symbol_registry && status != XR_COMPILE_RESOURCE_OK) {
        size_t attempts = attempt_count; XrCompileResourceStats before = stats(resources);
        XrHashMap *output = guard;
        CHECK(!xa_analyzer_collect_export_symbols_checked(&analyzer, (AstNode *)(uintptr_t)1, &output) && output == guard);
        CHECK(!xa_graph_spec_for_ast(&analyzer, (AstNode *)(uintptr_t)1));
        CHECK(!xa_graph_spec_for_identity(&analyzer, (const char *)(uintptr_t)1));
        XrCompileResourceStats after = stats(resources);
        CHECK(attempts == attempt_count && before.work == after.work && before.live_bytes == after.live_bytes && before.allocated_bytes == after.allocated_bytes);
    }
    if (resources) *observed = stats(resources);
    size_t attempts = attempt_count;
    xr_hashmap_owned_free(guard); xr_compile_module_graph_free(graph); xr_compile_module_resolver_free(resolver);
    xr_type_set_current_pool(NULL, NULL); xa_analyzer_clear_diagnostics(&analyzer);
    xa_analyzer_storage_close(&analyzer); xr_compile_session_free(session);
    if (!closed) xr_compile_resources_release(resources);
    CHECK(attempts == attempt_count && !physical_live && !allocation_count);
    return status;
}
static void matrix(bool dead) {
    reset(); boundary_count = 0; XrCompileResourceStats baseline = {0};
    CHECK(pipeline(dead, true, 0, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count, cuts = boundary_count; CHECK(sites && cuts);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = exact;
        uint64_t *limit = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *limit = (uint64_t)((int64_t)*limit + delta); reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, 0, &limits, &observed) == (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i = 0; i < sites; ++i) {
        reset(); fail_at = i; XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, 0, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset(); XrCompileResourceLimits limits = ample; limits.work = boundaries[i] - 1;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, 0, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("actual exports dead%d: %zu full-pipeline allocation faults/%zu observed collector work cuts; axes/sticky/output-preserved/physical0 PASS\n", dead, sites, cuts);
}
int main(void) {
    xr_type_global_init();
    CHECK(xr_type_new_error(NULL)->kind == XR_KIND_ERROR);
    matrix(false); matrix(true);
    for (int mode = 1; mode <= 6; ++mode) {
        reset(); XrCompileResourceStats observed = {0};
        CHECK(pipeline(false, false, mode, &ample, &observed) == (mode == 6
            ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_OK));
    }
    puts("actual exports: direct/native ErrorType, poisoned source, missing authority, semantic repair and existing-owner resource preservation PASS");
    return 0;
}
