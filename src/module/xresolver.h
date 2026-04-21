/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresolver.h - Dependency tree resolver
 *
 * KEY CONCEPT:
 *   Resolves transitive dependencies, detects version conflicts,
 *   and generates topologically sorted install order.
 *
 * RESOLUTION FLOW:
 *   1. Read direct deps from xray.toml
 *   2. Recursively fetch each dep's xray.toml
 *   3. Build complete dependency graph
 *   4. Detect cycles
 *   5. Resolve version conflicts
 *   6. Generate topo-sorted install order
 */

#ifndef XRESOLVER_H
#define XRESOLVER_H

#include "xsemver.h"
#include "xlockfile.h"
#include <stdbool.h>
#include "../base/xdefs.h"
#include "../base/xhashmap.h"

// A node in the dependency graph
typedef struct XrDepNode {
    char *name;                     // owner/name format
    char *constraint_str;
    XrVersionConstraint constraint;
    XrSemVer resolved_version;
    bool resolved;

    struct XrDepNode **dependencies;
    int dep_count;
    int dep_capacity;

    int depth;                      // Depth in dependency tree
    bool visited;                   // For traversal
    bool in_stack;                  // For cycle detection
} XrDepNode;

// Complete dependency graph
typedef struct XrDepGraph {
    XrDepNode **nodes;
    int node_count;
    int node_capacity;

    XrDepNode **roots;              // Direct dependencies
    int root_count;
    int root_capacity;

    XrHashMap *index;               // name -> XrDepNode* for O(1) lookup
} XrDepGraph;

// Resolution result with install order
typedef struct XrResolveResult {
    char **packages;                // In install order
    char **versions;
    int count;

    char *error;
    bool success;
} XrResolveResult;

typedef struct XrPackageInfo {
    char *name;
    char **versions;
    int version_count;
    char **deps;
    int dep_count;
} XrPackageInfo;

// Callback to fetch package info from registry
typedef XrPackageInfo* (*XrPackageInfoFn)(const char *name, void *user_data);

XR_FUNC void xr_package_info_free(XrPackageInfo *info);

/* ========== Dependency Graph API ========== */

XR_FUNC XrDepGraph* xr_depgraph_new(void);
XR_FUNC void xr_depgraph_free(XrDepGraph *graph);
XR_FUNC bool xr_depgraph_add_root(XrDepGraph *graph, const char *name,
                          const char *constraint);
XR_FUNC XrDepNode* xr_depgraph_find(XrDepGraph *graph, const char *name);

/* ========== Resolution API ========== */

// lockfile is optional (for pinning versions)
XR_FUNC XrResolveResult* xr_resolve_dependencies(XrDepGraph *graph,
                                         XrPackageInfoFn get_info,
                                         const XrLockfile *lockfile,
                                         void *user_data);
XR_FUNC void xr_resolve_result_free(XrResolveResult *result);

/* ========== Utility API ========== */

XR_FUNC bool xr_depgraph_has_cycle(XrDepGraph *graph, char **cycle_path);
XR_FUNC bool xr_depgraph_topo_sort(XrDepGraph *graph, XrDepNode ***order, int *count);
XR_FUNC void xr_depgraph_print(const XrDepGraph *graph);

#endif // XRESOLVER_H
