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
#include "../base/xmalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct XiResolvedExport {
    int mod_index;
    int shared_slot;
} XiResolvedExport;

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

static int graph_spec_to_topo(const XrModuleGraph *graph, int spec_index, int nmodules) {
    if (!graph || spec_index < 0)
        return -1;
    for (int topo = 0; topo < nmodules; topo++) {
        if (graph->topo_order[topo] == spec_index)
            return topo;
    }
    return -1;
}

static int resolve_reexport_source_topo(const XrModuleGraph *graph, const XiModule *module,
                                        const char *specifier, int nmodules) {
    if (!graph || !module || !specifier)
        return -1;
    int spec_index = -1;
    if (specifier[0] == '.') {
        const char *canonical = xi_resolve_import_canonical(graph, module->path, specifier);
        if (canonical)
            spec_index = xr_module_graph_find(graph, canonical);
    } else {
        spec_index = xr_module_graph_find(graph, specifier);
    }
    return graph_spec_to_topo(graph, spec_index, nmodules);
}

/* Resolve an exported name to its owning module and concrete shared slot.
 * Re-export-only facade modules intentionally own no duplicate shared slot:
 * AOT links directly to the original declaration, following selective,
 * aliased, and star re-export chains. */
static bool resolve_export_target(const XrModuleGraph *graph, XiModule **modules, int nmodules,
                                  int mod_index, const char *member_name, uint8_t *visiting,
                                  XiResolvedExport *out) {
    if (!graph || !modules || !member_name || !visiting || !out || mod_index < 0 ||
        mod_index >= nmodules || visiting[mod_index])
        return false;
    XiModule *module = modules[mod_index];
    if (!module)
        return false;

    visiting[mod_index] = 1;

    /* A declaration owned by this module always wins over star re-exports. */
    for (uint16_t ei = 0; ei < module->nexports; ei++) {
        const XiModuleExport *exp = &module->exports[ei];
        if (exp->name && strcmp(exp->name, member_name) == 0) {
            out->mod_index = mod_index;
            out->shared_slot = (int) exp->shared_slot;
            visiting[mod_index] = 0;
            return true;
        }
    }

    XiFunc *init = module->init;
    if (!init || !init->reexports) {
        visiting[mod_index] = 0;
        return false;
    }

    /* Selective re-exports take precedence over star re-exports and may
     * rename the public binding. */
    for (uint16_t ri = 0; ri < init->reexport_count; ri++) {
        const XiReexportEntry *re = &init->reexports[ri];
        if (!re->name)
            continue;
        const char *exported_name = re->alias ? re->alias : re->name;
        if (!exported_name || strcmp(exported_name, member_name) != 0)
            continue;
        int source_topo = resolve_reexport_source_topo(graph, module, re->from_path, nmodules);
        if (source_topo >= 0 &&
            resolve_export_target(graph, modules, nmodules, source_topo, re->name, visiting, out)) {
            visiting[mod_index] = 0;
            return true;
        }
    }

    for (uint16_t ri = 0; ri < init->reexport_count; ri++) {
        const XiReexportEntry *re = &init->reexports[ri];
        if (re->name)
            continue;
        int source_topo = resolve_reexport_source_topo(graph, module, re->from_path, nmodules);
        if (source_topo >= 0 && resolve_export_target(graph, modules, nmodules, source_topo,
                                                      member_name, visiting, out)) {
            visiting[mod_index] = 0;
            return true;
        }
    }

    visiting[mod_index] = 0;
    return false;
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
            int target_topo = graph_spec_to_topo(graph, target_spec_idx, nmodules);
            if (target_topo < 0)
                continue;

            if (!ref->member_name) {
                ref->resolved_mod_index = target_topo;
                continue;
            }

            uint8_t *visiting = (uint8_t *) xr_calloc((size_t) nmodules, sizeof(uint8_t));
            XiResolvedExport resolved = {-1, -1};
            if (visiting && resolve_export_target(graph, modules, nmodules, target_topo,
                                                  ref->member_name, visiting, &resolved)) {
                ref->resolved_mod_index = resolved.mod_index;
                ref->resolved_shared_slot = resolved.shared_slot;
            }
            xr_free(visiting);
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
