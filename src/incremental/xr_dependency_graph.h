/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_dependency_graph.h - Facet-aware incremental module dependencies
 *
 * KEY CONCEPT:
 *   An edge states which dependency facets a consumer observes and which
 *   consumer facets become stale when that observation changes.
 */

#ifndef XR_DEPENDENCY_GRAPH_H
#define XR_DEPENDENCY_GRAPH_H

#include "xr_module_summary.h"

#include <stdbool.h>
#include <stddef.h>

#define XR_DEPENDENCY_GRAPH_MAX_NODES 4096u
#define XR_DEPENDENCY_GRAPH_MAX_EDGES 16384u

typedef struct XrDependencyEdge {
    XrStableId consumer;
    XrStableId dependency;
    /* Row i maps dependency facet i to the exact consumer facets made stale. */
    XrModuleFacetMask relation[XR_MODULE_FACET_COUNT];
} XrDependencyEdge;

typedef struct XrDependencyGraph {
    XrModuleSummary *nodes;
    size_t node_count;
    size_t node_capacity;
    XrDependencyEdge *edges;
    size_t edge_count;
    size_t edge_capacity;
} XrDependencyGraph;

XR_FUNC void xr_dependency_graph_init(XrDependencyGraph *graph);
XR_FUNC void xr_dependency_graph_finalize(XrDependencyGraph *graph);
XR_FUNC bool xr_dependency_graph_add_node(XrDependencyGraph *graph,
                                          const XrModuleSummary *summary);
XR_FUNC bool xr_dependency_graph_replace_node(XrDependencyGraph *graph,
                                              const XrModuleSummary *summary);
XR_FUNC bool xr_dependency_graph_remove_node(XrDependencyGraph *graph, XrStableId module_id);
XR_FUNC bool xr_dependency_graph_rename_node(XrDependencyGraph *graph, XrStableId old_id,
                                             const XrModuleSummary *replacement);
XR_FUNC const XrModuleSummary *xr_dependency_graph_find_node(const XrDependencyGraph *graph,
                                                             XrStableId module_id);
XR_FUNC bool xr_dependency_graph_add_edge(XrDependencyGraph *graph, XrStableId consumer,
                                          XrStableId dependency,
                                          const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]);
XR_FUNC bool xr_dependency_graph_remove_edge(XrDependencyGraph *graph, XrStableId consumer,
                                             XrStableId dependency);
XR_FUNC size_t xr_dependency_graph_remove_consumer_edges(XrDependencyGraph *graph,
                                                         XrStableId consumer);
XR_FUNC const XrModuleSummary *xr_dependency_graph_node_at(const XrDependencyGraph *graph,
                                                           size_t index);
XR_FUNC const XrDependencyEdge *xr_dependency_graph_edge_at(const XrDependencyGraph *graph,
                                                            size_t index);
XR_FUNC const XrDependencyEdge *xr_dependency_graph_find_edge(const XrDependencyGraph *graph,
                                                              XrStableId consumer,
                                                              XrStableId dependency);
/* Exact identity of one consumer's resolved dependency set. The digest binds
 * the consumer stable ID plus its canonically ordered dependency IDs and
 * facet relations; unrelated nodes and edges are deliberately excluded. */
XR_FUNC bool xr_dependency_graph_module_resolution_fingerprint(
    const XrDependencyGraph *graph, XrStableId consumer, XrFingerprint *out);
XR_FUNC bool xr_dependency_graph_fingerprint(const XrDependencyGraph *graph,
                                             XrFingerprint *out);
XR_FUNC bool xr_dependency_graph_validate(const XrDependencyGraph *graph);

#endif  // XR_DEPENDENCY_GRAPH_H
