/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_graph_copy.inc.c - Owned graph metadata with an explicit syntax borrow
 */
static bool graph_copy_catalog_binding(ModuleWork *work, const XrModuleGraph *graph,
    const XrModuleSpec *source) {
    size_t count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources_v2(
        graph->resolver->config.catalog, &count);
    for (size_t i = 0; i < count && module_work(work, 1); ++i) {
        const XrModuleResourceBinding *binding = &bindings[i];
        if (binding != source->resource) continue;
        return binding->checked && !source->ast &&
            module_equal(work, binding->canonical, source->canonical) &&
            module_equal(work, binding->logical_path, source->logical_path) &&
            module_equal(work, binding->source_locator, source->source_path) &&
            binding->authority.kind == source->authority.kind &&
            module_equal(work, binding->authority.namespace_id, source->authority.namespace_id) &&
            module_equal(work, binding->authority.physical_root, source->authority.physical_root);
    }
    return false;
}

static bool graph_copy_source_locator(ModuleWork *work, const XrModuleSpec *source) {
    if (source->authority.kind == XR_MODULE_IDENTITY_MEMORY && source->source_path)
        return module_status(work, XR_MODULE_INVALID);
    if (source->embedded_source && source->authority.physical_root)
        return module_status(work, XR_MODULE_INVALID);
    if (!source->authority.physical_root) return true;
    char *canonical = NULL, *logical = NULL;
    module_status(work, xr_compile_module_identity_from_source(work->resources,
        &source->authority, source->source_path, &canonical, &logical));
    if (work->status == XR_MODULE_OK &&
        (!module_equal(work, canonical, source->canonical) ||
         !module_equal(work, logical, source->logical_path)))
        module_status(work, XR_MODULE_INVALID);
    xr_compile_resources_free(canonical);
    xr_compile_resources_free(logical);
    return work->status == XR_MODULE_OK;
}

static bool graph_copy_spec(ModuleWork *work, XrModuleGraph *receiver,
    const XrModuleSpec *source, int ordinal) {
    if (!source->canonical || source->kind != authority_kind(&source->authority) ||
        source->status < XR_MODSPEC_RESOLVED || source->status > XR_MODSPEC_LOWERED || source->dep_count < 0 ||
        source->dep_count > GRAPH_MAX_MODULES || source->dep_count > source->dep_capacity ||
        (source->dep_count && !source->dep_indices))
        return module_status(work, XR_MODULE_INVALID);
    char *canonical = NULL;
    module_status(work, xr_compile_module_identity_from_logical(receiver->resources,
        &source->authority, source->logical_path, &canonical));
    if (work->status == XR_MODULE_OK && !module_equal(work, canonical, source->canonical))
        module_status(work, XR_MODULE_INVALID);
    xr_compile_resources_free(canonical);
    int occupied = -1;
    if (work->status == XR_MODULE_OK)
        module_status(work, xr_compile_module_graph_find(receiver, source->canonical, &occupied));
    if (work->status == XR_MODULE_OK && occupied >= 0) module_status(work, XR_MODULE_INVALID);
    if (work->status != XR_MODULE_OK) return false;
    if (source->representation == XR_MODULE_SOURCE) {
        if (!graph_copy_source_locator(work, source)) return false;
        if (!source->ast || source->ast->type != AST_PROGRAM ||
            source->ast->as.program.count < 0 || source->ast->as.program.capacity < source->ast->as.program.count ||
            (source->ast->as.program.count && !source->ast->as.program.statements) ||
            !xr_compiler_arena_matches_state(source->ast->as.program.arena,
                xr_compile_session_compile_state(receiver->compiler_session)))
            return module_status(work, XR_MODULE_INVALID);
    } else if (source->representation != XR_MODULE_CHECKED_LIBRARY ||
        !graph_copy_catalog_binding(work, receiver, source)) {
        module_status(work, XR_MODULE_INVALID);
        return false;
    }
    int index = graph_add_spec(work, receiver, source->canonical, source->logical_path,
        source->source_path, source->kind, &source->authority);
    if (work->status != XR_MODULE_OK) return false;
    if (index != ordinal) return module_status(work, XR_MODULE_INVALID);
    XrModuleSpec *copy = &receiver->specs[index];
    copy->representation = source->representation;
    copy->resource = source->resource;
    copy->embedded_source = source->embedded_source;
    copy->source_content_fingerprint = source->source_content_fingerprint;
    copy->status = XR_MODSPEC_RESOLVED;
    copy->ast = source->ast;
    copy->ast_borrowed = source->ast != NULL;
    return true;
}

XR_FUNC XrModuleStatus xr_compile_module_graph_copy_syntax(XrModuleGraph *receiver,
    const XrModuleGraph *producer) {
    if (!receiver || !producer || receiver == producer || !receiver->resources ||
        receiver->resources != producer->resources || receiver->spec_count ||
        receiver->entry_index >= 0 || !receiver->resolver || !producer->resolver ||
        !receiver->compiler_session ||
        receiver->resolver->resources != receiver->resources || producer->resolver->resources != receiver->resources ||
        xr_compile_session_resources(receiver->compiler_session) != receiver->resources ||
        receiver->compiler_session != producer->compiler_session ||
        receiver->resolver->config.catalog != producer->resolver->config.catalog ||
        receiver->resolver->config.lockfile != producer->resolver->config.lockfile ||
        producer->resolution_status != XR_MODULE_OK || producer->spec_count <= 0 ||
        producer->spec_count > GRAPH_MAX_MODULES || producer->spec_count > producer->spec_capacity || !producer->specs ||
        producer->entry_index < 0 || producer->entry_index >= producer->spec_count)
        return XR_MODULE_INVALID;
    ModuleWork work = {receiver->resources, receiver->resolution_status};
    if (!module_equal(&work, receiver->resolver->config.stdlib_path,
        producer->resolver->config.stdlib_path)) module_status(&work, XR_MODULE_INVALID);
    for (int i = 0; i < producer->spec_count && module_work(&work, 1); ++i)
        if (!graph_copy_spec(&work, receiver, &producer->specs[i], i)) break;
    for (int i = 0; i < producer->spec_count && work.status == XR_MODULE_OK; ++i) {
        const XrModuleSpec *source = &producer->specs[i];
        for (int j = 0; j < source->dep_count && module_work(&work, 1); ++j) {
            int target = source->dep_indices[j];
            if (target < 0 || target >= producer->spec_count) {
                module_status(&work, XR_MODULE_INVALID); break;
            }
            spec_add_dep(&work, &receiver->specs[i], target);
        }
    }
    if (work.status == XR_MODULE_OK) {
        receiver->entry_index = producer->entry_index;
        module_status(&work, xr_compile_module_graph_topological_sort(receiver));
    }
    return work.status == XR_MODULE_OK ? XR_MODULE_OK : graph_failure(receiver, work.status);
}
