/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_graph.h - Independent graph view over immutable SemanticPlan data
 */

#ifndef XR_SEMANTIC_GRAPH_H
#define XR_SEMANTIC_GRAPH_H

#include "xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrSemanticGraph {
    uint32_t block_count;
    uint32_t edge_count;
    uint32_t *out_begin;
    uint32_t *out_edges;
    uint32_t *in_begin;
    uint32_t *in_edges;
    uint32_t *rpo_rank;
    uint32_t *immediate_dominator;
    uint8_t *reachable;
} XrSemanticGraph;

XR_FUNC bool xr_semantic_graph_build(const XrSemanticPlan *plan, XrSemanticGraph *graph,
                                     char *error, size_t error_size);
XR_FUNC void xr_semantic_graph_dispose(XrSemanticGraph *graph);
XR_FUNC bool xr_semantic_graph_has_edge(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                        uint32_t from, uint32_t to);
XR_FUNC bool xr_semantic_graph_is_reachable(const XrSemanticGraph *graph, uint32_t block);
XR_FUNC bool xr_semantic_graph_dominates(const XrSemanticGraph *graph, uint32_t dominator,
                                         uint32_t block);

#endif  // XR_SEMANTIC_GRAPH_H
