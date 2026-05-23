/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_import_resolve.h - Graph-based XI_IMPORT_REF resolution
 *
 * KEY CONCEPT:
 *   After lowering, XI_IMPORT_REF values carry module_path and
 *   member_name but have resolved_mod_index = -1.  This utility
 *   walks the Xi IR and fills resolved_mod_index + resolved_shared_slot
 *   using the XrModuleGraph, enabling downstream emission of
 *   OP_LOAD_MODULE_SLOT instead of OP_IMPORT + OP_GETPROP.
 */

#ifndef XI_IMPORT_RESOLVE_H
#define XI_IMPORT_RESOLVE_H

#include "xi.h"

struct XrModuleGraph;

/* Resolve a relative import specifier against the importer's directory.
 * Returns the canonical path from the graph, or NULL if not found.
 * The returned string is owned by the graph (do not free). */
XR_FUNC const char *xi_resolve_import_canonical(const struct XrModuleGraph *graph,
                                                const char *importer_path, const char *specifier);

/* Recursively walk an XiFunc and all children, resolving XI_IMPORT_REF
 * values using the module graph.  modules[t] is the XiModule for topo
 * position t; nmodules = graph->topo_count. */
XR_FUNC void xi_resolve_imports(XiFunc *f, const struct XrModuleGraph *graph,
                                const char *importer_path, XiModule **modules, int nmodules);

#endif  // XI_IMPORT_RESOLVE_H
