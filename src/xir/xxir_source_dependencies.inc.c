/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_dependencies.inc.c - Capture the same checked graph before producer destruction
 */
static XrXirStatus source_dependencies_publish(SourceContext *ctx, XrXirSourceSnapshot *snapshot) {
    const XrModuleGraph *graph = ctx->graph;
    if (!graph || graph->spec_count <= 0 || graph->topo_count != graph->spec_count ||
        !graph->topo_order || graph->entry_index < 0 || graph->entry_index >= graph->spec_count ||
        graph->has_cycle) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = (uint32_t)graph->spec_count;
    XrXirSourceDependency *rows = source_alloc(ctx, count, sizeof(*rows));
    if (!rows) return ctx->diagnostic.status;
    XrXirSourceDependencies facts = {rows, count, UINT32_MAX, NULL};
    const XrModuleSpec *entry = &graph->specs[graph->entry_index];
    facts.entry_path = entry->source_path ? entry->source_path : entry->canonical;
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, NULL)) return ctx->diagnostic.status;
        int index = graph->topo_order[i];
        if (index < 0 || index >= graph->spec_count) return XR_XIR_BAD_STRUCTURE;
        const XrModuleSpec *spec = &graph->specs[index];
        XrXirSourceDependencyKind kind;
        switch (spec->kind) {
        case XR_MOD_STDLIB: kind = XR_XIR_DEPENDENCY_STDLIB; break;
        case XR_MOD_FILE: kind = XR_XIR_DEPENDENCY_FILE; break;
        case XR_MOD_PACKAGE: kind = XR_XIR_DEPENDENCY_PACKAGE; break;
        case XR_MOD_MEMORY: kind = XR_XIR_DEPENDENCY_MEMORY; break;
        default: return XR_XIR_BAD_STRUCTURE;
        }
        if (spec->dep_count < 0 || (spec->dep_count && !spec->dep_indices)) return XR_XIR_BAD_STRUCTURE;
        uint32_t *imports = spec->dep_count ? source_alloc(ctx, (uint32_t)spec->dep_count, sizeof(*imports)) : NULL;
        if (spec->dep_count && !imports) return ctx->diagnostic.status;
        for (int j = 0; j < spec->dep_count; ++j) {
            if (!source_work(ctx, NULL)) return ctx->diagnostic.status;
            if (spec->dep_indices[j] < 0 || spec->dep_indices[j] >= graph->spec_count)
                return XR_XIR_BAD_STRUCTURE;
            imports[j] = (uint32_t)spec->dep_indices[j];
        }
        const char *path = xr_module_spec_import_name(spec);
        rows[i] = (XrXirSourceDependency){(uint32_t)index, kind,
            path ? path : spec->canonical, imports, (uint32_t)spec->dep_count};
        if (index == graph->entry_index) facts.entry = i;
    }
    return xr_xir_compile_source_snapshot_dependencies_copy(snapshot, &facts);
}
