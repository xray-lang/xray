/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_dependencies_owner.inc.c - Transactional dependency facts on the snapshot ledger
 */
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_dependencies_copy(
    XrXirSourceSnapshot *snapshot, const XrXirSourceDependencies *source) {
    if (!snapshot || !source || snapshot->dependencies_ready) return XR_XIR_BAD_STRUCTURE;
    SourceQueryCopy copy = {snapshot, XR_XIR_OK};
    if (!query_work(&copy, sizeof(*source))) return copy.status;
    const XrXirSourceView *view = &snapshot->view;
    if (!source->count || source->count > view->module_count || !source->entries ||
        source->entry >= source->count || !source->entry_path) return XR_XIR_BAD_STRUCTURE;
    uint32_t *positions = xir_compile_calloc(&snapshot->context, view->module_count,
        sizeof(*positions), &copy.status);
    if (!positions) return copy.status;
    for (uint32_t i = 0; i < source->count && query_work(&copy, sizeof(source->entries[i])); ++i) {
        const XrXirSourceDependency *row = &source->entries[i];
        if (row->module >= view->module_count || positions[row->module] ||
            (unsigned)row->kind > XR_XIR_DEPENDENCY_MEMORY || !row->path ||
            (!!row->imports != !!row->import_count)) { copy.status = XR_XIR_BAD_STRUCTURE; break; }
        positions[row->module] = i + 1;
    }
    for (uint32_t i = 0; i < source->count && query_work(&copy, 1); ++i) {
        const XrXirSourceDependency *row = &source->entries[i];
        for (uint32_t j = 0; j < row->import_count && query_work(&copy, sizeof(uint32_t)); ++j) {
            uint32_t target = row->imports[j];
            if (target >= view->module_count || !positions[target] || positions[target] > i) {
                copy.status = XR_XIR_BAD_STRUCTURE; break;
            }
        }
    }
    xr_compile_resources_free(positions);
    if (copy.status != XR_XIR_OK) return copy.status;
    SourceQueryMemory *previous = snapshot->memory;
    XrXirSourceDependencies owned = *source;
    XrXirSourceDependency *rows = query_copy(&copy, source->entries, source->count, sizeof(*rows));
    owned.entries = rows;
    owned.entry_path = query_string(&copy, source->entry_path);
    for (uint32_t i = 0; rows && i < source->count && query_work(&copy, 1); ++i) {
        rows[i].path = query_string(&copy, source->entries[i].path);
        rows[i].imports = query_copy(&copy, source->entries[i].imports,
            source->entries[i].import_count, sizeof(uint32_t));
    }
    (void)query_work(&copy, sizeof(owned) + sizeof(snapshot->dependencies_ready));
    if (copy.status != XR_XIR_OK) {
        while (snapshot->memory != previous) {
            SourceQueryMemory *next = snapshot->memory->next;
            xr_compile_resources_free(snapshot->memory); snapshot->memory = next;
        }
        return copy.status;
    }
    snapshot->dependencies = owned; snapshot->dependencies_ready = true;
    return XR_XIR_OK;
}
XR_FUNC const XrXirSourceDependencies *xr_xir_compile_source_snapshot_dependencies(
    const XrXirSourceSnapshot *snapshot) {
    return snapshot && snapshot->dependencies_ready ? &snapshot->dependencies : NULL;
}
