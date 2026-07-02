/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_import_resolve.c - Graph-based XI_IMPORT_REF resolution
 *
 * KEY CONCEPT:
 *   Walks Xi IR functions and resolves import references against the
 *   XrModuleGraph.  Fills resolved_mod_index (topo position) and
 *   resolved_shared_slot (export slot in target module) so the emitter
 *   can generate OP_LOAD_MODULE_SLOT (selective) or OP_LOAD_MODULE (whole-module).
 *
 *   Used by both the AOT driver and the VM multi-module compilation path.
 */

#include "xi_import_resolve.h"
#include "../module/xmodule_graph.h"
#include "../base/xchecks.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#endif

/* ========== Canonical Path Resolution ========== */

XR_FUNC const char *xi_resolve_import_canonical(const XrModuleGraph *graph,
                                                const char *importer_path, const char *specifier) {
    XR_DCHECK(graph != NULL, "xi_resolve_import_canonical: NULL graph");
    if (!specifier || specifier[0] != '.')
        return NULL; /* stdlib/package imports not resolved here */
    if (!importer_path)
        return NULL;

    /* Derive importer directory */
    char dir[PATH_MAX];
    strncpy(dir, importer_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash)
        *(slash + 1) = '\0';
    else
        dir[0] = '\0';

    /* Build candidate path: dir + specifier + ".xr" */
    char candidate[PATH_MAX];
    const char *rel = specifier;
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    snprintf(candidate, sizeof(candidate), "%s%s.xr", dir, rel);

    /* Normalize with realpath (resolves symlinks) */
    char resolved[PATH_MAX];
    if (realpath(candidate, resolved)) {
        int idx = xr_module_graph_find(graph, resolved);
        if (idx >= 0)
            return graph->specs[idx].canonical;
    }

    /* Try without realpath (file may have been parsed with original path) */
    int idx = xr_module_graph_find(graph, candidate);
    if (idx >= 0)
        return graph->specs[idx].canonical;

    return NULL;
}

/* ========== Single-Function Resolution ========== */

/* Walk a single XiFunc (non-recursive) and resolve XI_IMPORT_REF values. */
static void resolve_func_imports(XiFunc *f, const XrModuleGraph *graph, const char *importer_path,
                                 XiModule **modules, int nmodules) {
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_IMPORT_REF)
                continue;
            XiImportRef *ref = (XiImportRef *) v->aux;
            if (!ref || !ref->module_path)
                continue;
            if (ref->resolved_mod_index >= 0)
                continue; /* already resolved */

            int target_spec_idx = -1;
            if (ref->module_path[0] == '.') {
                /* Relative import: resolve specifier to canonical path via graph */
                const char *canonical =
                    xi_resolve_import_canonical(graph, importer_path, ref->module_path);
                if (!canonical)
                    continue;
                target_spec_idx = xr_module_graph_find(graph, canonical);
            } else {
                /* Bare (stdlib) import.  Pure-C native stdlib modules (math, os,
                 * ...) ship no compiled source and are absent from the graph, so
                 * they stay unresolved here and are emitted as native calls
                 * downstream.  A pure-Xray stdlib module (e.g. `sync`) is
                 * compiled into the bundle as a real graph module keyed by its
                 * bare name, so resolve it here to link cross-module class /
                 * function references exactly like any other module. */
                target_spec_idx = xr_module_graph_find(graph, ref->module_path);
            }
            if (target_spec_idx < 0)
                continue;

            /* Map spec index to topo position (= modules[] index) */
            int target_topo = -1;
            for (int t = 0; t < nmodules; t++) {
                if (graph->topo_order[t] == target_spec_idx) {
                    target_topo = t;
                    break;
                }
            }
            if (target_topo < 0)
                continue;

            ref->resolved_mod_index = target_topo;

            /* For selective imports, find the export slot */
            if (ref->member_name && modules[target_topo]) {
                XiModule *tmod = modules[target_topo];
                for (uint16_t ei = 0; ei < tmod->nexports; ei++) {
                    if (tmod->exports[ei].name &&
                        strcmp(tmod->exports[ei].name, ref->member_name) == 0) {
                        ref->resolved_shared_slot = (int) tmod->exports[ei].shared_slot;
                        break;
                    }
                }
            }
        }
    }
}

/* ========== Recursive Resolution ========== */

XR_FUNC void xi_resolve_imports(XiFunc *f, const XrModuleGraph *graph, const char *importer_path,
                                XiModule **modules, int nmodules) {
    if (!f)
        return;
    resolve_func_imports(f, graph, importer_path, modules, nmodules);
    for (uint16_t c = 0; c < f->nchildren; c++)
        xi_resolve_imports(f->children[c], graph, importer_path, modules, nmodules);
}
