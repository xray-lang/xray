/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_compile.c - Source file to bytecode file compiler helper
 */

#include "xproto_codec.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../ir/xi_module.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../runtime/value/xtype_internal.h"
#include "xmodule.h"
#include "xmodule_graph.h"
#include "xmodule_resolver.h"
#include "xray_vm.h"
#include <stdio.h>
#include <string.h>

static bool compile_module_status(XrCompileState *state, XrModuleStatus status) {
    if (status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY)
        xr_compile_state_fail(state, status == XR_MODULE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_MODULE_OK;
}

static bool stdlib_compile_authority(XrCompileState *state, const char *module_name,
    const char *source_file, XrModuleIdentityAuthority *authority, char **root_out) {
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return false;
    XrCompileResources *resources = xr_compile_state_resources(state);
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    char *source = NULL, *directory = NULL, *root = NULL, *identity = NULL, *logical = NULL;
    bool ok = compile_module_status(state, xr_module_status_from_io(xr_realpath_owned(&policy, source_file, &source))) &&
        compile_module_status(state, xr_module_status_from_io(xr_path_dirname_owned(&policy, source, &directory))) &&
        compile_module_status(state, xr_module_status_from_io(xr_path_dirname_owned(&policy, directory, &root)));
    XrModuleIdentityAuthority candidate = {XR_MODULE_IDENTITY_STDLIB, module_name, root};
    if (ok)
        ok = compile_module_status(state, xr_compile_module_identity_from_source(resources, &candidate, source, &identity, &logical));
    size_t name_length = 0, logical_length = 0;
    if (ok)
        ok = xr_compile_state_string_length(state, module_name, &name_length) == XR_COMPILE_RESOURCE_OK &&
             xr_compile_state_string_length(state, logical, &logical_length) == XR_COMPILE_RESOURCE_OK &&
             name_length <= (SIZE_MAX - 4) / 2 && logical_length == name_length * 2 + 4;
    if (ok)
        ok = xr_compile_state_work(state, logical_length) == XR_COMPILE_RESOURCE_OK &&
             memcmp(logical, module_name, name_length) == 0 && logical[name_length] == '/' &&
             memcmp(logical + name_length + 1, module_name, name_length) == 0 &&
             memcmp(logical + name_length * 2 + 1, ".xr", 3) == 0;
    if (ok) {
        *authority = candidate;
        *root_out = root;
        root = NULL;
    }
    xr_compile_resources_free(source);
    xr_compile_resources_free(directory);
    xr_compile_resources_free(root);
    xr_compile_resources_free(identity);
    xr_compile_resources_free(logical);
    return ok;
}

/* Serialize one compiled module and write it to output_file. */
static bool write_bytecode_file(XrVMRuntime *X, const char *stdlib_module_name, XrProto *proto,
                                int flags, const char *output_file) {
    size_t bc_size;
    XrBootstrapContainerError bc_error = XR_BOOTSTRAP_CONTAINER_OK;
    uint8_t *bc = stdlib_module_name
                      ? xr_bootstrap_container_write_stdlib(X, stdlib_module_name, proto, flags,
                                                           &bc_size, &bc_error)
                      : xr_bootstrap_container_write(X, proto, flags, &bc_size, &bc_error);
    if (!bc) {
        xr_log_warning("compile", "bytecode serialization failed: %s",
                       xr_bootstrap_container_error_string(bc_error));
        return false;
    }

    FILE *f = fopen(output_file, "wb");
    if (!f) {
        xr_free(bc);
        xr_log_warning("compile", "cannot create: %s", output_file);
        return false;
    }

    bool wrote_all = fwrite(bc, 1, bc_size, f) == bc_size;
    bool closed = fclose(f) == 0;
    xr_free(bc);
    if (!wrote_all || !closed) {
        xr_log_warning("compile", "cannot complete: %s", output_file);
        return false;
    }
    return true;
}

/* The graph owns the exact source snapshot and AST; compilation does not
 * reopen or reparse an entry after dependency admission. */
static bool compile_to_file_impl(const XrCompilerInput *input, const char *stdlib_name,
    const char *source_file, const char *output_file, int flags,
    const XrModuleIdentityAuthority *authority) {
    if (!input)
        return false;
    XrCompileState *state = xr_compile_session_compile_state(input->analysis.session);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return false;
    if (!input->analysis.runtime || !input->analysis.target || !authority)
        return false;
    XrCompileResources *resources = xr_compile_state_resources(state);
    XrTypePool *previous_pool = xr_type_get_current_pool();
    XrModuleResolver *resolver = NULL;
    XrModuleGraph *graph = NULL;
    XaAnalyzer *analyzer = NULL;
    XrCompiledModuleGraph compilation = {0};
    XrProto *entry = NULL;
    char *error = NULL;
    bool ok = false;
    XrModuleRegistry *registry = xr_isolate_get_module_registry(input->analysis.runtime);
    XrModuleResolverConfig config = {.stdlib_path = registry ? registry->stdlib_path : NULL};
    if (!compile_module_status(state, xr_compile_module_resolver_new(resources, &config, &resolver)) ||
        !compile_module_status(state, xr_compile_module_graph_new(resources, input->analysis.session, resolver, &graph)) ||
        !compile_module_status(state, xr_compile_module_graph_build(graph, source_file, authority, &error)) ||
        !compile_module_status(state, xr_compile_module_graph_topological_sort(graph)) || graph->has_cycle)
        goto done;
    graph->X = input->analysis.runtime;
    XaAnalyzerInput analysis = input->analysis;
    analysis.module_identity = graph->specs[graph->entry_index].canonical;
    analyzer = xa_analyzer_new(&analysis);
    if (!analyzer)
        goto done;
    xa_analyzer_set_graph(analyzer, graph);
    for (int i = 0; i < graph->topo_count; i++) {
        XrModuleSpec *spec = &graph->specs[graph->topo_order[i]];
        if (!spec->ast)
            continue;
        const char *file = spec->source_path ? spec->source_path : spec->canonical;
        xa_analyzer_analyze(analyzer, file, spec->ast);
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
            goto done;
        int errors = xa_analyzer_print_errors(analyzer, file);
        XrHashMap *exports = NULL;
        if (!errors && !xa_analyzer_collect_export_symbols_checked(analyzer, spec->ast, &exports))
            errors = 1;
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
            goto done;
        if (!errors && exports != spec->export_symbols) {
            xr_hashmap_owned_free(spec->export_symbols);
            spec->export_symbols = exports;
        }
        xa_analyzer_clear_diagnostics(analyzer);
        if (errors)
            goto done;
    }
    XrCompileGraphInput compile_graph = {.compiler = input, .analyzer = analyzer, .graph = graph};
    if (!xr_compile_module_graph_dependencies(&compile_graph, &compilation))
        goto done;
    compile_graph.modules = compilation.modules;
    compile_graph.module_count = compilation.count;
    int slot = -1;
    for (int i = 0; i < graph->topo_count; i++)
        if (graph->topo_order[i] == graph->entry_index)
            slot = i;
    if (slot < 0)
        goto done;
    entry = xr_compile_ast_in_graph(&compile_graph, graph->entry_index, &compilation.modules[slot]);
    if (entry && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK)
        ok = write_bytecode_file(input->analysis.runtime, stdlib_name, entry, flags, output_file);
done:
    xr_instruction_unit_free(entry);
    xr_compiled_module_graph_dispose(&compilation);
    if (analyzer)
        xa_analyzer_set_graph(analyzer, NULL);
    xa_analyzer_free(analyzer);
    xr_compile_module_graph_free(graph);
    xr_compile_module_resolver_free(resolver);
    xr_compile_resources_free(error);
    xr_type_set_current_pool(previous_pool, NULL);
    return ok && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK;
}

bool xr_compile_to_file(const XrCompilerInput *input, const char *source_file,
    const char *output_file, int flags, const XrModuleIdentityAuthority *authority) {
    return compile_to_file_impl(input, NULL, source_file, output_file, flags, authority);
}

bool xr_compile_stdlib_to_file(const XrCompilerInput *input, const char *canonical_module,
    const char *source_file, const char *output_file, int flags) {
    if (!input)
        return false;
    XrCompileState *state = xr_compile_session_compile_state(input->analysis.session);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return false;
    XrModuleIdentityAuthority authority = {0};
    char *root = NULL;
    bool ok = canonical_module && stdlib_compile_authority(state, canonical_module, source_file, &authority, &root) &&
        compile_to_file_impl(input, canonical_module, source_file, output_file, flags, &authority);
    xr_compile_resources_free(root);
    return ok;
}
