/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_graph.c - Independent graph view over immutable SemanticPlan data
 */

#include "xr_semantic_graph.h"
#include "xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

void xr_semantic_graph_dispose(XrSemanticGraph *graph) {
    if (!graph)
        return;
    xr_free(graph->out_begin);
    xr_free(graph->out_edges);
    xr_free(graph->in_begin);
    xr_free(graph->in_edges);
    xr_free(graph->rpo_rank);
    xr_free(graph->immediate_dominator);
    xr_free(graph->reachable);
    memset(graph, 0, sizeof(*graph));
}

static uint32_t intersect(const XrSemanticGraph *graph, uint32_t left, uint32_t right) {
    while (left != right) {
        while (graph->rpo_rank[left] > graph->rpo_rank[right])
            left = graph->immediate_dominator[left];
        while (graph->rpo_rank[right] > graph->rpo_rank[left])
            right = graph->immediate_dominator[right];
    }
    return left;
}

static bool build_function_dominators(const XrSemanticPlan *plan, XrSemanticGraph *graph,
                                      const XrSemanticFunctionRecord *function,
                                      uint32_t *node_stack, uint32_t *cursor_stack,
                                      uint32_t *postorder, char *error, size_t error_size) {
    if (function->block_count == 0)
        return report(error, error_size, "XR_SEM_0016", "function has no entry block");
    uint32_t entry = function->block_begin;
    uint32_t depth = 0;
    uint32_t postorder_count = 0;
    graph->reachable[entry] = 1;
    node_stack[depth] = entry;
    cursor_stack[depth++] = graph->out_begin[entry];
    while (depth > 0) {
        uint32_t block = node_stack[depth - 1];
        uint32_t cursor = cursor_stack[depth - 1];
        uint32_t end = graph->out_begin[block + 1];
        if (cursor < end) {
            cursor_stack[depth - 1]++;
            uint32_t edge_index = graph->out_edges[cursor];
            uint32_t next = plan->edges[edge_index].to_block;
            if (!graph->reachable[next]) {
                graph->reachable[next] = 1;
                node_stack[depth] = next;
                cursor_stack[depth++] = graph->out_begin[next];
            }
            continue;
        }
        postorder[postorder_count++] = block;
        depth--;
    }

    for (uint32_t i = 0; i < postorder_count; i++) {
        uint32_t block = postorder[postorder_count - i - 1];
        graph->rpo_rank[block] = i;
    }
    graph->immediate_dominator[entry] = entry;
    bool changed = true;
    uint32_t iteration_count = 0;
    while (changed) {
        if (++iteration_count > postorder_count + 1)
            return report(error, error_size, "XR_EXEC_5003",
                          "dominator iteration budget exhausted");
        changed = false;
        for (uint32_t r = 1; r < postorder_count; r++) {
            uint32_t block = postorder[postorder_count - r - 1];
            uint32_t new_idom = XR_SEMANTIC_INDEX_NONE;
            for (uint32_t cursor = graph->in_begin[block]; cursor < graph->in_begin[block + 1];
                 cursor++) {
                uint32_t edge_index = graph->in_edges[cursor];
                uint32_t predecessor = plan->edges[edge_index].from_block;
                if (!graph->reachable[predecessor] ||
                    graph->immediate_dominator[predecessor] == XR_SEMANTIC_INDEX_NONE)
                    continue;
                new_idom = new_idom == XR_SEMANTIC_INDEX_NONE
                               ? predecessor
                               : intersect(graph, predecessor, new_idom);
            }
            if (new_idom == XR_SEMANTIC_INDEX_NONE)
                return report(error, error_size, "XR_SEM_0016",
                              "reachable block has no reachable semantic predecessor");
            if (graph->immediate_dominator[block] != new_idom) {
                graph->immediate_dominator[block] = new_idom;
                changed = true;
            }
        }
    }
    return true;
}

bool xr_semantic_graph_build(const XrSemanticPlan *plan, XrSemanticGraph *graph, char *error,
                             size_t error_size) {
    if (!plan || !graph)
        return report(error, error_size, "XR_SEM_0016", "semantic graph input is invalid");
    memset(graph, 0, sizeof(*graph));
    graph->block_count = plan->block_count;
    graph->edge_count = plan->edge_count;
    graph->out_begin =
        (uint32_t *) xr_calloc((size_t) plan->block_count + 1, sizeof(*graph->out_begin));
    graph->in_begin =
        (uint32_t *) xr_calloc((size_t) plan->block_count + 1, sizeof(*graph->in_begin));
    graph->out_edges =
        (uint32_t *) xr_malloc((size_t) plan->edge_count * sizeof(*graph->out_edges));
    graph->in_edges = (uint32_t *) xr_malloc((size_t) plan->edge_count * sizeof(*graph->in_edges));
    graph->rpo_rank = (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*graph->rpo_rank));
    graph->immediate_dominator =
        (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*graph->immediate_dominator));
    graph->reachable = (uint8_t *) xr_calloc(plan->block_count, sizeof(*graph->reachable));
    uint32_t *node_stack = (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*node_stack));
    uint32_t *cursor_stack =
        (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*cursor_stack));
    uint32_t *postorder = (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*postorder));
    if ((plan->block_count && (!graph->out_begin || !graph->in_begin || !graph->rpo_rank ||
                               !graph->immediate_dominator || !graph->reachable || !node_stack ||
                               !cursor_stack || !postorder)) ||
        (plan->edge_count && (!graph->out_edges || !graph->in_edges))) {
        xr_free(node_stack);
        xr_free(cursor_stack);
        xr_free(postorder);
        xr_semantic_graph_dispose(graph);
        return report(error, error_size, "XR_EXEC_5003", "semantic graph budget exhausted");
    }
    for (uint32_t b = 0; b < plan->block_count; b++) {
        graph->rpo_rank[b] = XR_SEMANTIC_INDEX_NONE;
        graph->immediate_dominator[b] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t e = 0; e < plan->edge_count; e++) {
        const XrSemanticEdgeRecord *edge = &plan->edges[e];
        if (edge->from_block >= plan->block_count || edge->to_block >= plan->block_count) {
            xr_free(node_stack);
            xr_free(cursor_stack);
            xr_free(postorder);
            xr_semantic_graph_dispose(graph);
            return report(error, error_size, "XR_SEM_0010", "semantic graph edge index is invalid");
        }
        graph->out_begin[edge->from_block + 1]++;
        graph->in_begin[edge->to_block + 1]++;
    }
    for (uint32_t b = 1; b <= plan->block_count; b++) {
        graph->out_begin[b] += graph->out_begin[b - 1];
        graph->in_begin[b] += graph->in_begin[b - 1];
    }
    uint32_t *out_cursor = (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*out_cursor));
    uint32_t *in_cursor = (uint32_t *) xr_malloc((size_t) plan->block_count * sizeof(*in_cursor));
    if (plan->block_count && (!out_cursor || !in_cursor)) {
        xr_free(out_cursor);
        xr_free(in_cursor);
        xr_free(node_stack);
        xr_free(cursor_stack);
        xr_free(postorder);
        xr_semantic_graph_dispose(graph);
        return report(error, error_size, "XR_EXEC_5003", "semantic graph budget exhausted");
    }
    if (plan->block_count) {
        memcpy(out_cursor, graph->out_begin, (size_t) plan->block_count * sizeof(*out_cursor));
        memcpy(in_cursor, graph->in_begin, (size_t) plan->block_count * sizeof(*in_cursor));
    }
    for (uint32_t e = 0; e < plan->edge_count; e++) {
        const XrSemanticEdgeRecord *edge = &plan->edges[e];
        graph->out_edges[out_cursor[edge->from_block]++] = e;
        graph->in_edges[in_cursor[edge->to_block]++] = e;
    }
    xr_free(out_cursor);
    xr_free(in_cursor);

    bool valid = true;
    for (uint32_t f = 0; valid && f < plan->function_count; f++)
        valid = build_function_dominators(plan, graph, &plan->functions[f], node_stack,
                                          cursor_stack, postorder, error, error_size);
    xr_free(node_stack);
    xr_free(cursor_stack);
    xr_free(postorder);
    if (!valid)
        xr_semantic_graph_dispose(graph);
    return valid;
}

bool xr_semantic_graph_has_edge(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                uint32_t from, uint32_t to) {
    if (!plan || !graph || from >= graph->block_count || to >= graph->block_count)
        return false;
    for (uint32_t cursor = graph->out_begin[from]; cursor < graph->out_begin[from + 1]; cursor++) {
        if (plan->edges[graph->out_edges[cursor]].to_block == to)
            return true;
    }
    return false;
}

bool xr_semantic_graph_is_reachable(const XrSemanticGraph *graph, uint32_t block) {
    return graph && block < graph->block_count && graph->reachable[block] != 0;
}

bool xr_semantic_graph_dominates(const XrSemanticGraph *graph, uint32_t dominator, uint32_t block) {
    if (!graph || dominator >= graph->block_count || block >= graph->block_count ||
        !graph->reachable[dominator] || !graph->reachable[block])
        return false;
    uint32_t cursor = block;
    while (cursor != dominator) {
        uint32_t parent = graph->immediate_dominator[cursor];
        if (parent == XR_SEMANTIC_INDEX_NONE || parent == cursor)
            return false;
        cursor = parent;
    }
    return true;
}
