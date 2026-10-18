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
 *   walks the Xi IR and fills resolved_mod_index plus the semantic shared
 *   slot and dense VM export slot.  The VM emitter consumes the dense export
 *   slot for OP_LOAD_MODULE_SLOT; AOT consumers retain the semantic slot.
 */

#ifndef XI_IMPORT_RESOLVE_H
#define XI_IMPORT_RESOLVE_H

#include "xi.h"
#include "xi_module.h"
#include "../module/xmodule_identity.h"

struct XrModuleGraph;

/* Resolve a relative import specifier against the importer's directory.
 * OK publishes a borrowed graph canonical key, or NULL when absent. Failure
 * preserves output and retains the original parser state's resource cause. */
XR_FUNC XrModuleStatus xi_resolve_import_canonical(const struct XrModuleGraph *graph,
    const char *importer_path, const char *specifier, const char **output);

/* Recursively walk an XiFunc and all children, resolving XI_IMPORT_REF
 * values using the module graph.  modules[t] is the XiModule for topo
 * position t; nmodules <= graph->topo_count. Callers must stop compilation on
 * failure; this pass does not roll back earlier metadata rows in the IR. */
XR_FUNC XrModuleStatus xi_resolve_imports(XiFunc *f, const struct XrModuleGraph *graph,
    const char *importer_path, XiModule **modules, int nmodules);

#endif  // XI_IMPORT_RESOLVE_H
