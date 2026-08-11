/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_dependency_graph.c - Facet-aware incremental module dependencies
 */

#include "xr_dependency_graph.h"

#include "../base/xmalloc.h"

#include <stdint.h>
#include <string.h>

static size_t find_node_index(const XrDependencyGraph *graph, XrStableId module_id) {
    if (!graph)
        return SIZE_MAX;
    for (size_t i = 0; i < graph->node_count; i++) {
        if (xr_stable_id_equal(graph->nodes[i].module_id, module_id))
            return i;
    }
    return SIZE_MAX;
}

static size_t find_edge_index(const XrDependencyGraph *graph, XrStableId consumer,
                              XrStableId dependency) {
    if (!graph)
        return SIZE_MAX;
    for (size_t i = 0; i < graph->edge_count; i++) {
        const XrDependencyEdge *edge = &graph->edges[i];
        if (xr_stable_id_equal(edge->consumer, consumer) &&
            xr_stable_id_equal(edge->dependency, dependency)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool relation_is_valid(const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    if (!relation)
        return false;
    XrModuleFacetMask mapped = 0;
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        if ((relation[facet] & ~XR_MODULE_FACET_ALL) != 0)
            return false;
        mapped |= relation[facet];
    }
    return mapped != 0;
}

static bool reserve_nodes(XrDependencyGraph *graph, size_t needed) {
    if (needed <= graph->node_capacity)
        return true;
    size_t capacity = graph->node_capacity ? graph->node_capacity * 2u : 8u;
    if (capacity < needed)
        capacity = needed;
    if (capacity > SIZE_MAX / sizeof(*graph->nodes))
        return false;
    XrModuleSummary *resized =
        (XrModuleSummary *) xr_realloc(graph->nodes, capacity * sizeof(*graph->nodes));
    if (!resized)
        return false;
    graph->nodes = resized;
    graph->node_capacity = capacity;
    return true;
}

static bool reserve_edges(XrDependencyGraph *graph, size_t needed) {
    if (needed <= graph->edge_capacity)
        return true;
    size_t capacity = graph->edge_capacity ? graph->edge_capacity * 2u : 8u;
    if (capacity < needed)
        capacity = needed;
    if (capacity > SIZE_MAX / sizeof(*graph->edges))
        return false;
    XrDependencyEdge *resized =
        (XrDependencyEdge *) xr_realloc(graph->edges, capacity * sizeof(*graph->edges));
    if (!resized)
        return false;
    graph->edges = resized;
    graph->edge_capacity = capacity;
    return true;
}

static void remove_edge_at(XrDependencyGraph *graph, size_t index) {
    if (index + 1u < graph->edge_count) {
        memmove(&graph->edges[index], &graph->edges[index + 1u],
                (graph->edge_count - index - 1u) * sizeof(*graph->edges));
    }
    graph->edge_count--;
}

void xr_dependency_graph_init(XrDependencyGraph *graph) {
    if (graph)
        memset(graph, 0, sizeof(*graph));
}

void xr_dependency_graph_finalize(XrDependencyGraph *graph) {
    if (!graph)
        return;
    for (size_t i = 0; i < graph->node_count; i++)
        xr_module_summary_finalize(&graph->nodes[i]);
    xr_free(graph->edges);
    xr_free(graph->nodes);
    memset(graph, 0, sizeof(*graph));
}

bool xr_dependency_graph_add_node(XrDependencyGraph *graph, const XrModuleSummary *summary) {
    if (!graph || !xr_module_summary_validate(summary) ||
        find_node_index(graph, summary->module_id) != SIZE_MAX ||
        !reserve_nodes(graph, graph->node_count + 1u)) {
        return false;
    }
    if (!xr_module_summary_copy(&graph->nodes[graph->node_count], summary))
        return false;
    graph->node_count++;
    return true;
}

bool xr_dependency_graph_replace_node(XrDependencyGraph *graph, const XrModuleSummary *summary) {
    if (!graph || !xr_module_summary_validate(summary))
        return false;
    size_t index = find_node_index(graph, summary->module_id);
    if (index == SIZE_MAX)
        return false;

    XrModuleSummary copy;
    if (!xr_module_summary_copy(&copy, summary))
        return false;
    xr_module_summary_finalize(&graph->nodes[index]);
    graph->nodes[index] = copy;
    return true;
}

bool xr_dependency_graph_remove_node(XrDependencyGraph *graph, XrStableId module_id) {
    if (!graph)
        return false;
    size_t index = find_node_index(graph, module_id);
    if (index == SIZE_MAX)
        return false;

    for (size_t i = graph->edge_count; i > 0; i--) {
        const XrDependencyEdge *edge = &graph->edges[i - 1u];
        if (xr_stable_id_equal(edge->consumer, module_id) ||
            xr_stable_id_equal(edge->dependency, module_id)) {
            remove_edge_at(graph, i - 1u);
        }
    }
    xr_module_summary_finalize(&graph->nodes[index]);
    if (index + 1u < graph->node_count) {
        memmove(&graph->nodes[index], &graph->nodes[index + 1u],
                (graph->node_count - index - 1u) * sizeof(*graph->nodes));
    }
    graph->node_count--;
    return true;
}

bool xr_dependency_graph_rename_node(XrDependencyGraph *graph, XrStableId old_id,
                                     const XrModuleSummary *replacement) {
    if (!graph || !xr_module_summary_validate(replacement))
        return false;
    size_t old_index = find_node_index(graph, old_id);
    size_t replacement_index = find_node_index(graph, replacement->module_id);
    if (old_index == SIZE_MAX ||
        (replacement_index != SIZE_MAX && replacement_index != old_index)) {
        return false;
    }

    XrModuleSummary copy;
    if (!xr_module_summary_copy(&copy, replacement))
        return false;
    xr_module_summary_finalize(&graph->nodes[old_index]);
    graph->nodes[old_index] = copy;
    for (size_t i = 0; i < graph->edge_count; i++) {
        if (xr_stable_id_equal(graph->edges[i].consumer, old_id))
            graph->edges[i].consumer = replacement->module_id;
        if (xr_stable_id_equal(graph->edges[i].dependency, old_id))
            graph->edges[i].dependency = replacement->module_id;
    }
    return true;
}

const XrModuleSummary *xr_dependency_graph_find_node(const XrDependencyGraph *graph,
                                                     XrStableId module_id) {
    size_t index = find_node_index(graph, module_id);
    return index == SIZE_MAX ? NULL : &graph->nodes[index];
}

bool xr_dependency_graph_add_edge(XrDependencyGraph *graph, XrStableId consumer,
                                  XrStableId dependency,
                                  const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    if (!graph || !relation_is_valid(relation) ||
        find_node_index(graph, consumer) == SIZE_MAX ||
        find_node_index(graph, dependency) == SIZE_MAX) {
        return false;
    }

    size_t existing = find_edge_index(graph, consumer, dependency);
    if (existing != SIZE_MAX) {
        memcpy(graph->edges[existing].relation, relation, sizeof(graph->edges[existing].relation));
        return true;
    }
    if (!reserve_edges(graph, graph->edge_count + 1u))
        return false;
    graph->edges[graph->edge_count++] = (XrDependencyEdge) {
        .consumer = consumer,
        .dependency = dependency,
        .relation = {0},
    };
    memcpy(graph->edges[graph->edge_count - 1u].relation, relation,
           sizeof(graph->edges[graph->edge_count - 1u].relation));
    return true;
}

bool xr_dependency_graph_remove_edge(XrDependencyGraph *graph, XrStableId consumer,
                                     XrStableId dependency) {
    if (!graph)
        return false;
    size_t index = find_edge_index(graph, consumer, dependency);
    if (index == SIZE_MAX)
        return false;
    remove_edge_at(graph, index);
    return true;
}

size_t xr_dependency_graph_remove_consumer_edges(XrDependencyGraph *graph, XrStableId consumer) {
    if (!graph)
        return 0;
    size_t removed = 0;
    for (size_t i = graph->edge_count; i > 0; i--) {
        if (xr_stable_id_equal(graph->edges[i - 1u].consumer, consumer)) {
            remove_edge_at(graph, i - 1u);
            removed++;
        }
    }
    return removed;
}

const XrModuleSummary *xr_dependency_graph_node_at(const XrDependencyGraph *graph,
                                                   size_t index) {
    return graph && index < graph->node_count ? &graph->nodes[index] : NULL;
}

const XrDependencyEdge *xr_dependency_graph_edge_at(const XrDependencyGraph *graph,
                                                    size_t index) {
    return graph && index < graph->edge_count ? &graph->edges[index] : NULL;
}

bool xr_dependency_graph_validate(const XrDependencyGraph *graph) {
    if (!graph || graph->node_count > graph->node_capacity ||
        graph->edge_count > graph->edge_capacity)
        return false;
    for (size_t i = 0; i < graph->node_count; i++) {
        if (!xr_module_summary_validate(&graph->nodes[i]))
            return false;
        for (size_t j = i + 1u; j < graph->node_count; j++) {
            if (xr_stable_id_equal(graph->nodes[i].module_id, graph->nodes[j].module_id))
                return false;
        }
    }
    for (size_t i = 0; i < graph->edge_count; i++) {
        const XrDependencyEdge *edge = &graph->edges[i];
        if (!relation_is_valid(edge->relation) ||
            find_node_index(graph, edge->consumer) == SIZE_MAX ||
            find_node_index(graph, edge->dependency) == SIZE_MAX) {
            return false;
        }
        for (size_t j = i + 1u; j < graph->edge_count; j++) {
            if (xr_stable_id_equal(edge->consumer, graph->edges[j].consumer) &&
                xr_stable_id_equal(edge->dependency, graph->edges[j].dependency)) {
                return false;
            }
        }
    }
    return true;
}
