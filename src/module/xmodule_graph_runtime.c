/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "xmodule_graph_runtime.h"
#include "xmodule.h"
#include "../base/xmalloc.h"

XR_FUNC bool xr_module_graph_preload(XrVMRuntime *X, const XrModuleGraph *g, XrModule ***out_table) {
    if (out_table)
        *out_table = NULL;
    if (!X || !g || !out_table || g->topo_count <= 0 || !g->topo_order)
        return false;

    XrModule **table = xr_calloc((size_t) g->topo_count, sizeof(XrModule *));
    if (!table)
        return false;

    for (int ti = 0; ti < g->topo_count; ti++) {
        int idx = g->topo_order[ti];
        if (idx == g->entry_index)
            continue;
        const XrModuleSpec *spec = &g->specs[idx];
        if (!spec->source_path)
            continue;
        const char *import_name = xr_module_spec_import_name(spec);
        if (!import_name) {
            xr_free(table);
            return false;
        }
        XrValue value = xr_module_import(X, import_name);
        if (XR_IS_NULL(value)) {
            xr_free(table);
            return false;
        }
        table[ti] = xr_value_to_module(value);
        if (!table[ti]) {
            xr_free(table);
            return false;
        }
    }

    *out_table = table;
    return true;
}
