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
 *   XrModuleGraph. Fills the topological module index, the semantic shared
 *   slot used by AOT, and the dense runtime export slot used by VM bytecode.
 *
 *   Used by both the AOT driver and the VM multi-module compilation path.
 */

#include "xi_import_resolve.h"
#include "../module/xmodule_graph.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../toolchain/xcompiler_session.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct XiResolvedExport {
    int mod_index;
    int shared_slot;
    int export_slot;
    XiFunc *function; /* exported function, NULL for value/class exports */
} XiResolvedExport;

typedef struct XiImportWork {
    const XrModuleGraph *graph;
    XrCompileState *state;
    XrModuleStatus status;
} XiImportWork;

static int compare_export_name(XiImportWork *work, const char *a, const char *b);

static bool import_name_equal(XiImportWork *work, const char *a, const char *b) {
    return a && b && compare_export_name(work, a, b) == 0 && work->status == XR_MODULE_OK;
}

static XrModuleStatus import_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_MODULE_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_MODULE_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_MODULE_OUT_OF_MEMORY : XR_MODULE_INVALID;
}

static bool import_accept(XiImportWork *work, XrModuleStatus status) {
    if (work->status != XR_MODULE_OK)
        return false;
    if (status == XR_MODULE_NOT_FOUND)
        status = XR_MODULE_OK;
    if (status == XR_MODULE_OK)
        status = import_resource_status(xr_compile_state_status(work->state));
    if (status == XR_MODULE_OK)
        return true;
    work->status = status;
    if (status == XR_MODULE_BUDGET || status == XR_MODULE_OUT_OF_MEMORY || status == XR_MODULE_INVALID)
        (void) xr_compile_state_fail(work->state, status == XR_MODULE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET :
            status == XR_MODULE_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    return false;
}

static bool import_step(XiImportWork *work, uint64_t units) {
    return work->status == XR_MODULE_OK &&
        import_accept(work, import_resource_status(xr_compile_state_work(work->state, units)));
}

static XiImportWork import_begin(const XrModuleGraph *graph) {
    XiImportWork work = {.graph = graph, .status = XR_MODULE_INVALID};
    if (!graph || !graph->compiler_session || !graph->resources)
        return work;
    work.state = xr_compile_session_compile_state(graph->compiler_session);
    work.status = import_resource_status(xr_compile_state_status(work.state));
    if (work.status == XR_MODULE_OK && graph->resources != xr_compile_state_resources(work.state))
        (void) import_accept(&work, XR_MODULE_INVALID);
    if (work.status == XR_MODULE_OK) {
        const XrOsIoPolicy policy = xr_compile_io_policy(graph->resources);
        if ((graph->id_index && !xr_hashmap_owned_uses_policy(graph->id_index, &policy)) ||
            (graph->resolver && graph->resolver->resources != graph->resources))
            (void) import_accept(&work, XR_MODULE_INVALID);
    }
    return work;
}

XR_FUNC XrModuleStatus xi_resolve_import_canonical(const XrModuleGraph *graph,
    const char *importer_path, const char *specifier, const char **output) {
    XiImportWork work = import_begin(graph);
    if (work.status != XR_MODULE_OK)
        return work.status;
    if (!importer_path || !specifier || !output)
        return import_accept(&work, XR_MODULE_INVALID), work.status;
    if (!import_step(&work, 1))
        return work.status;
    if (specifier[0] != '.') {
        *output = NULL;
        return XR_MODULE_OK;
    }
    int owner = -1, target = -1;
    if (!import_accept(&work, xr_compile_module_graph_find_source(graph, importer_path, &owner)))
        return work.status;
    if (owner < 0) {
        *output = NULL;
        return XR_MODULE_OK;
    }
    if (owner >= graph->spec_count || !graph->resolver)
        return import_accept(&work, XR_MODULE_INVALID), work.status;
    const XrModuleSpec *source = &graph->specs[owner];
    XrModuleId resolved = {0};
    XrModuleStatus status = xr_compile_module_resolver_resolve(graph->resolver, specifier,
        source->source_path, &source->authority, &resolved, NULL);
    if (import_accept(&work, status) && status == XR_MODULE_OK)
        (void) import_accept(&work, xr_compile_module_graph_find(graph, resolved.canonical, &target));
    xr_compile_module_id_cleanup(&resolved);
    if (work.status == XR_MODULE_OK)
        *output = target >= 0 && target < graph->spec_count ? graph->specs[target].canonical : NULL;
    return work.status;
}

static int graph_spec_to_topo(XiImportWork *work, int spec_index, int nmodules) {
    const XrModuleGraph *graph = work->graph;
    if (!graph || spec_index < 0)
        return -1;
    for (int topo = 0; topo < nmodules && import_step(work, 1); topo++) {
        if (graph->topo_order[topo] == spec_index)
            return topo;
    }
    return -1;
}

static int resolve_reexport_source_topo(XiImportWork *work, const XiModule *module,
                                        const char *specifier, int nmodules) {
    const XrModuleGraph *graph = work->graph;
    if (!module || !specifier || !import_step(work, 1))
        return -1;
    int spec_index = -1;
    if (specifier[0] == '.') {
        const char *canonical = NULL;
        if (!import_accept(work, xi_resolve_import_canonical(graph, module->path, specifier, &canonical)))
            return -1;
        if (canonical && !import_accept(work, xr_compile_module_graph_find(graph, canonical, &spec_index)))
            return -1;
    } else {
        if (!import_accept(work, xr_compile_module_graph_find_named_dependency(graph, module->path, specifier, &spec_index)))
            return -1;
    }
    return graph_spec_to_topo(work, spec_index, nmodules);
}

/* Resolve an exported name to its owning module and concrete shared slot.
 * Re-export-only facade modules intentionally own no duplicate shared slot:
 * AOT links directly to the original declaration, following selective,
 * aliased, and star re-export chains. */
static bool resolve_export_target(XiImportWork *work, XiModule **modules, int nmodules,
                                  int mod_index, const char *member_name, uint8_t *visiting,
                                  XiResolvedExport *out) {
    if (!import_step(work, 1) || !modules || !member_name || !visiting || !out || mod_index < 0 ||
        mod_index >= nmodules || visiting[mod_index])
        return false;
    XiModule *module = modules[mod_index];
    if (!module)
        return false;

    visiting[mod_index] = 1;

    /* A declaration owned by this module always wins over star re-exports. */
    for (uint16_t ei = 0; ei < module->nexports && import_step(work, 1); ei++) {
        const XiModuleExport *exp = &module->exports[ei];
        if (import_name_equal(work, exp->name, member_name)) {
            out->mod_index = mod_index;
            out->shared_slot = (int) exp->shared_slot;
            out->export_slot = (int) ei;
            out->function = exp->function;
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
    for (uint16_t ri = 0; ri < init->reexport_count && import_step(work, 1); ri++) {
        const XiReexportEntry *re = &init->reexports[ri];
        if (!re->name)
            continue;
        const char *exported_name = re->alias ? re->alias : re->name;
        if (!import_name_equal(work, exported_name, member_name))
            continue;
        int source_topo = resolve_reexport_source_topo(work, module, re->from_path, nmodules);
        if (source_topo >= 0 &&
            resolve_export_target(work, modules, nmodules, source_topo, re->name, visiting, out)) {
            visiting[mod_index] = 0;
            return true;
        }
    }

    for (uint16_t ri = 0; ri < init->reexport_count && import_step(work, 1); ri++) {
        const XiReexportEntry *re = &init->reexports[ri];
        if (re->name)
            continue;
        int source_topo = resolve_reexport_source_topo(work, module, re->from_path, nmodules);
        if (source_topo >= 0 && resolve_export_target(work, modules, nmodules, source_topo,
                                                      member_name, visiting, out)) {
            visiting[mod_index] = 0;
            return true;
        }
    }

    visiting[mod_index] = 0;
    return false;
}

/* ========== Single-Function Resolution ========== */

typedef struct XiExportNameCollector {
    const char **names;
    uint32_t count;
    uint32_t capacity;
    XiImportWork *work;
} XiExportNameCollector;

static bool collect_export_name(const char *key, void *value, void *userdata) {
    (void) value;
    XiExportNameCollector *collector = (XiExportNameCollector *) userdata;
    if (!collector || !key || collector->count >= collector->capacity) {
        if (collector)
            (void) import_accept(collector->work, XR_MODULE_INVALID);
        return false;
    }
    if (!import_step(collector->work, 1))
        return false;
    collector->names[collector->count++] = key;
    return true;
}

static int compare_export_name(XiImportWork *work, const char *a, const char *b) {
    for (size_t i = 0; import_step(work, 2); i++) {
        unsigned char left = (unsigned char) a[i], right = (unsigned char) b[i];
        if (left != right)
            return left < right ? -1 : 1;
        if (!left)
            return 0;
    }
    return 0;
}

static void sift_export_names(XiImportWork *work, const char **names, uint32_t count, uint32_t root) {
    while (root < count / 2u && import_step(work, 1)) {
        uint32_t child = root * 2u + 1u;
        if (child + 1u < count && compare_export_name(work, names[child], names[child + 1u]) < 0)
            child++;
        if (work->status != XR_MODULE_OK || compare_export_name(work, names[root], names[child]) >= 0)
            return;
        if (!import_step(work, 3))
            return;
        const char *swap = names[root]; names[root] = names[child]; names[child] = swap;
        root = child;
    }
}

static bool sort_export_names(XiImportWork *work, const char **names, uint32_t count) {
    for (uint32_t start = count / 2u; start && import_step(work, 1); start--)
        sift_export_names(work, names, count, start - 1u);
    for (uint32_t end = count; end > 1u && import_step(work, 3); end--) {
        const char *swap = names[0]; names[0] = names[end - 1u]; names[end - 1u] = swap;
        sift_export_names(work, names, end - 1u, 0);
    }
    return work->status == XR_MODULE_OK;
}

static void *import_scratch(XiImportWork *work, size_t count, size_t bytes) {
    if (work->status != XR_MODULE_OK)
        return NULL;
    void *memory = NULL;
    (void) import_accept(work, import_resource_status(xr_compile_state_calloc(work->state, count, bytes, &memory)));
    return memory;
}

static void *import_arena(XiImportWork *work, XiFunc *f, size_t bytes) {
    if (bytes > UINT32_MAX) {
        (void) import_accept(work, XR_MODULE_BUDGET);
        return NULL;
    }
    if (!import_step(work, bytes))
        return NULL;
    void *memory = xi_func_arena_alloc(f, (uint32_t) bytes);
    if (!memory)
        (void) import_accept(work, XR_MODULE_OUT_OF_MEMORY);
    return memory;
}

static char *copy_reexport_name(XiImportWork *work, XiFunc *f, const char *name) {
    if (work->status != XR_MODULE_OK)
        return NULL;
    size_t length = 0;
    if (!import_accept(work, import_resource_status(xr_compile_state_string_length(work->state, name, &length))))
        return NULL;
    if (length >= UINT32_MAX) {
        (void) import_accept(work, XR_MODULE_BUDGET);
        return NULL;
    }
    char *copy = import_arena(work, f, length + 1u);
    if (copy && !import_accept(work, import_resource_status(xr_compile_state_copy(work->state, copy, name, length + 1u))))
        return NULL;
    return copy;
}

static void resolve_func_reexports(XiFunc *f, XiImportWork *work, XiModule **modules,
                                   int nmodules) {
    const XrModuleGraph *graph = work->graph;
    if (!f || !f->module || !f->reexports)
        return;

    for (uint16_t ri = 0; ri < f->reexport_count && import_step(work, 1); ri++) {
        XiReexportEntry *re = &f->reexports[ri];
        int source_topo = resolve_reexport_source_topo(work, f->module, re->from_path, nmodules);
        if (source_topo < 0)
            continue;
        int source_spec_index = graph->topo_order[source_topo];
        XrModuleSpec *source_spec = &graph->specs[source_spec_index];

        re->resolution_attempted = true;
        if (re->name) {
            uint8_t *visiting = (uint8_t *) import_scratch(work, (size_t) nmodules, sizeof(uint8_t));
            if (!visiting)
                return;
            XiResolvedExport resolved = {-1, -1, -1, NULL};
            if (resolve_export_target(work, modules, nmodules, source_topo, re->name,
                                                  visiting, &resolved)) {
                re->resolved_mod_index = resolved.mod_index;
                re->resolved_export_slot = resolved.export_slot;
                re->resolution_complete = true;
            } else if (work->status == XR_MODULE_OK && source_spec->kind != XR_MOD_FILE) {
                /* External native/stdlib/package exports stay name-resolved at
                 * runtime while retaining the graph-derived source identity. */
                re->resolution_complete = true;
            }
            xr_compile_state_free(visiting);
            continue;
        }

        XrHashMap *exports = source_spec->export_symbols;
        if (!exports && source_spec->kind != XR_MOD_FILE) {
            /* Native/stdlib/package modules do not necessarily publish an
             * analyzer-owned export map.  Their star exports must retain the
             * runtime metadata path instead of being mistaken for a failed
             * file-graph resolution. */
            re->resolution_attempted = false;
            continue;
        }
        uint32_t count = xr_hashmap_count(exports);
        if (!exports)
            continue;
        if (count > UINT16_MAX) {
            (void) import_accept(work, XR_MODULE_BUDGET);
            return;
        }
        const XrOsIoPolicy policy = xr_compile_io_policy(graph->resources);
        if (!xr_hashmap_owned_uses_policy(exports, &policy)) {
            (void) import_accept(work, XR_MODULE_INVALID);
            return;
        }
        if (count == 0) {
            re->resolution_complete = true;
            continue;
        }

        const char **names = (const char **) import_scratch(work, count, sizeof(const char *));
        if (!names)
            return;
        XiResolvedReexport *members = (XiResolvedReexport *) import_arena(
            work, f, (size_t) count * sizeof(XiResolvedReexport));
        if (!members) {
            xr_compile_state_free(names);
            return;
        }
        XiExportNameCollector collector = {.names = names, .count = 0, .capacity = count, .work = work};
        XrOsIoStatus visited = xr_hashmap_owned_visit(exports, collect_export_name, &collector);
        if (!import_accept(work, xr_module_status_from_io(visited)) || collector.count != count ||
            !sort_export_names(work, names, count)) {
            if (work->status == XR_MODULE_OK)
                (void) import_accept(work, XR_MODULE_INVALID);
            xr_compile_state_free(names);
            return;
        }

        bool complete = true;
        for (uint32_t i = 0; i < count && import_step(work, 1); i++) {
            XiResolvedExport resolved = {-1, -1, -1, NULL};
            uint8_t *visiting = (uint8_t *) import_scratch(work, (size_t) nmodules, sizeof(uint8_t));
            char *name = visiting ? copy_reexport_name(work, f, names[i]) : NULL;
            if (!name || !visiting ||
                !resolve_export_target(work, modules, nmodules, source_topo, names[i], visiting,
                                       &resolved)) {
                complete = false;
                xr_compile_state_free(visiting);
                break;
            }
            xr_compile_state_free(visiting);
            members[i].export_name = name;
            members[i].mod_index = resolved.mod_index;
            members[i].export_slot = resolved.export_slot;
        }
        xr_compile_state_free(names);
        if (complete && work->status == XR_MODULE_OK) {
            re->resolved_members = members;
            re->resolved_member_count = (uint16_t) count;
            re->resolution_complete = true;
        }
    }
}

/* Walk a single XiFunc (non-recursive) and resolve XI_IMPORT_REF values. */
static void resolve_func_imports(XiFunc *f, XiImportWork *work, const char *importer_path,
                                 XiModule **modules, int nmodules) {
    const XrModuleGraph *graph = work->graph;
    if (!f)
        return;
    for (uint32_t b = 0; b < f->nblocks && import_step(work, 1); b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && import_step(work, 1); i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_IMPORT_REF)
                continue;
            XiImportRef *ref = (XiImportRef *) v->aux;
            if (!ref || !ref->module_path)
                continue;
            ref->resolution_attempted = true;
            /* An early (pre-ARC) pass runs while the driver's module array is
             * still filling, so a ref can carry a resolved index from that
             * pass while its module/function pointers are still missing.
             * Re-walk such a ref instead of skipping it, so a later pass can
             * complete the pointers without disturbing the index it already
             * published. */
            if (ref->resolved_mod_index >= 0 && ref->resolved_module)
                continue; /* fully resolved */

            int target_spec_idx = -1;
            if (ref->has_exact_target) {
                if (ref->exact_target_spec_index < 0 ||
                    ref->exact_target_spec_index >= graph->spec_count || !ref->member_name)
                    continue;
                target_spec_idx = ref->exact_target_spec_index;
            } else if (ref->module_path[0] == '.') {
                /* Relative import: resolve specifier to canonical path via graph */
                const char *canonical = NULL;
                if (!import_accept(work, xi_resolve_import_canonical(graph, importer_path, ref->module_path, &canonical)))
                    return;
                if (!canonical)
                    continue;
                if (!import_accept(work, xr_compile_module_graph_find(graph, canonical, &target_spec_idx)))
                    return;
            } else {
                /* Named stdlib/package import. Pure-C native stdlib modules (math, os,
                 * ...) ship no compiled source and are absent from the graph, so
                 * they stay unresolved here and are emitted as native calls
                 * downstream. A source-backed stdlib/package module is reached
                 * through the importer's resolver-admitted edge; its typed
                 * canonical identity is never compared with source spelling. */
                if (!import_accept(work, xr_compile_module_graph_find_named_dependency(
                    graph, importer_path, ref->module_path, &target_spec_idx)))
                    return;
            }
            if (target_spec_idx < 0)
                continue;

            /* Map spec index to topo position (= modules[] index) */
            int target_topo = graph_spec_to_topo(work, target_spec_idx, nmodules);
            if (target_topo < 0)
                continue;

            if (!ref->member_name) {
                /* Whole-module (namespace) import. The module pointer is what
                 * lets ARC resolve `mod.f(x)` to f's borrow signature; it stays
                 * NULL until that module has been compiled. */
                ref->resolved_mod_index = target_topo;
                ref->resolved_module = modules[target_topo];
                continue;
            }

            uint8_t *visiting = (uint8_t *) import_scratch(work, (size_t) nmodules, sizeof(uint8_t));
            if (!visiting)
                return;
            XiResolvedExport resolved = {-1, -1, -1, NULL};
            if (resolve_export_target(work, modules, nmodules, target_topo,
                                                  ref->member_name, visiting, &resolved)) {
                ref->resolved_mod_index = resolved.mod_index;
                ref->resolved_shared_slot = resolved.shared_slot;
                ref->resolved_export_slot = resolved.export_slot;
                ref->resolved_module = modules[resolved.mod_index];
                ref->resolved_func = resolved.function;
                if (!ref->resolved_func) {
                    XiModule *owner = modules[resolved.mod_index];
                    if (owner && owner->slot_funcs && resolved.shared_slot >= 0 &&
                        resolved.shared_slot < (int) owner->nslots)
                        ref->resolved_func = owner->slot_funcs[resolved.shared_slot];
                }
            }
            xr_compile_state_free(visiting);
        }
    }
}

/* ========== Recursive Resolution ========== */

XR_FUNC XrModuleStatus xi_resolve_imports(XiFunc *f, const XrModuleGraph *graph, const char *importer_path,
                                          XiModule **modules, int nmodules) {
    XiImportWork work = import_begin(graph);
    if (work.status != XR_MODULE_OK)
        return work.status;
    if (!f || nmodules < 0 || nmodules > graph->topo_count ||
        (nmodules && (!modules || !graph->topo_order)))
        return import_accept(&work, XR_MODULE_INVALID), work.status;
    if (!import_step(&work, 1))
        return work.status;
    resolve_func_reexports(f, &work, modules, nmodules);
    if (work.status == XR_MODULE_OK)
        resolve_func_imports(f, &work, importer_path, modules, nmodules);
    for (uint16_t c = 0; c < f->nchildren && import_step(&work, 1); c++)
        (void) import_accept(&work, xi_resolve_imports(f->children[c], graph, importer_path, modules, nmodules));
    return work.status;
}
