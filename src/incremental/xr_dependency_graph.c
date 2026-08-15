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
#include "../base/xsha256.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrNodeRef {
    const XrModuleSummary *summary;
} XrNodeRef;

typedef struct XrEdgeRef {
    const XrDependencyEdge *edge;
} XrEdgeRef;

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

static bool relation_matches_nodes(
    const XrDependencyGraph *graph, XrStableId consumer, XrStableId dependency,
    const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    size_t consumer_index = find_node_index(graph, consumer);
    size_t dependency_index = find_node_index(graph, dependency);
    if (consumer_index == SIZE_MAX || dependency_index == SIZE_MAX)
        return false;
    const XrModuleSummary *consumer_summary = &graph->nodes[consumer_index];
    const XrModuleSummary *dependency_summary = &graph->nodes[dependency_index];
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        XrModuleFacetMask bit = XR_MODULE_FACET_BIT(facet);
        if (relation[facet] != 0 && (dependency_summary->present_facets & bit) == 0)
            return false;
        if ((relation[facet] & ~consumer_summary->present_facets) != 0)
            return false;
    }
    return true;
}

static int compare_node_refs(const void *left, const void *right) {
    const XrNodeRef *a = (const XrNodeRef *) left;
    const XrNodeRef *b = (const XrNodeRef *) right;
    return xr_stable_id_compare(a->summary->module_id, b->summary->module_id);
}

static int compare_edge_refs(const void *left, const void *right) {
    const XrEdgeRef *a = (const XrEdgeRef *) left;
    const XrEdgeRef *b = (const XrEdgeRef *) right;
    int order = xr_stable_id_compare(a->edge->consumer, b->edge->consumer);
    return order ? order : xr_stable_id_compare(a->edge->dependency, b->edge->dependency);
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static bool reserve_nodes(XrDependencyGraph *graph, size_t needed) {
    if (needed > XR_DEPENDENCY_GRAPH_MAX_NODES)
        return false;
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
    if (needed > XR_DEPENDENCY_GRAPH_MAX_EDGES)
        return false;
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
    XrModuleSummary previous = graph->nodes[index];
    graph->nodes[index] = copy;
    if (!xr_dependency_graph_validate(graph)) {
        graph->nodes[index] = previous;
        xr_module_summary_finalize(&copy);
        return false;
    }
    xr_module_summary_finalize(&previous);
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

    for (size_t i = 0; i < graph->edge_count; i++) {
        const XrDependencyEdge *edge = &graph->edges[i];
        if (xr_stable_id_equal(edge->consumer, old_id)) {
            for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
                if ((edge->relation[facet] & ~replacement->present_facets) != 0)
                    return false;
        }
        if (xr_stable_id_equal(edge->dependency, old_id)) {
            for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
                if (edge->relation[facet] != 0 &&
                    (replacement->present_facets & XR_MODULE_FACET_BIT(facet)) == 0) {
                    return false;
                }
        }
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
        !relation_matches_nodes(graph, consumer, dependency, relation)) {
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

const XrDependencyEdge *xr_dependency_graph_find_edge(const XrDependencyGraph *graph,
                                                      XrStableId consumer,
                                                      XrStableId dependency) {
    size_t index = find_edge_index(graph, consumer, dependency);
    return index == SIZE_MAX ? NULL : &graph->edges[index];
}

bool xr_dependency_graph_module_resolution_fingerprint(
    const XrDependencyGraph *graph, XrStableId consumer, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-module-resolution-v1\0";
    if (!out || !xr_dependency_graph_validate(graph) ||
        !xr_dependency_graph_find_node(graph, consumer)) {
        return false;
    }

    size_t edge_count = 0;
    for (size_t i = 0; i < graph->edge_count; i++) {
        if (xr_stable_id_equal(graph->edges[i].consumer, consumer))
            edge_count++;
    }
    XrEdgeRef *edges = NULL;
    if (edge_count != 0) {
        if (edge_count > SIZE_MAX / sizeof(*edges))
            return false;
        edges = (XrEdgeRef *) xr_malloc(edge_count * sizeof(*edges));
        if (!edges)
            return false;
        size_t cursor = 0;
        for (size_t i = 0; i < graph->edge_count; i++) {
            if (xr_stable_id_equal(graph->edges[i].consumer, consumer))
                edges[cursor++].edge = &graph->edges[i];
        }
        qsort(edges, edge_count, sizeof(*edges), compare_edge_refs);
    }

    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, consumer.bytes, sizeof(consumer.bytes));
    hash_u64(&ctx, edge_count);
    for (size_t i = 0; i < edge_count; i++) {
        const XrDependencyEdge *edge = edges[i].edge;
        xr_sha256_update(&ctx, edge->dependency.bytes,
                         sizeof(edge->dependency.bytes));
        for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
            hash_u64(&ctx, edge->relation[facet]);
    }
    xr_sha256_final(&ctx, out->bytes);
    xr_free(edges);
    return true;
}

bool xr_dependency_graph_fingerprint(const XrDependencyGraph *graph, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-dependency-graph-v1\0";
    if (!out || !xr_dependency_graph_validate(graph))
        return false;

    XrNodeRef *nodes = NULL;
    XrEdgeRef *edges = NULL;
    if (graph->node_count) {
        nodes = (XrNodeRef *) xr_malloc(graph->node_count * sizeof(*nodes));
        if (!nodes)
            return false;
        for (size_t i = 0; i < graph->node_count; i++)
            nodes[i].summary = &graph->nodes[i];
        qsort(nodes, graph->node_count, sizeof(*nodes), compare_node_refs);
    }
    if (graph->edge_count) {
        edges = (XrEdgeRef *) xr_malloc(graph->edge_count * sizeof(*edges));
        if (!edges) {
            xr_free(nodes);
            return false;
        }
        for (size_t i = 0; i < graph->edge_count; i++)
            edges[i].edge = &graph->edges[i];
        qsort(edges, graph->edge_count, sizeof(*edges), compare_edge_refs);
    }

    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, graph->node_count);
    hash_u64(&ctx, graph->edge_count);
    for (size_t i = 0; i < graph->node_count; i++) {
        XrFingerprint summary_fingerprint;
        if (!xr_module_summary_fingerprint(nodes[i].summary, &summary_fingerprint)) {
            xr_free(edges);
            xr_free(nodes);
            return false;
        }
        xr_sha256_update(&ctx, nodes[i].summary->module_id.bytes,
                         sizeof(nodes[i].summary->module_id.bytes));
        xr_sha256_update(&ctx, summary_fingerprint.bytes,
                         sizeof(summary_fingerprint.bytes));
    }
    for (size_t i = 0; i < graph->edge_count; i++) {
        const XrDependencyEdge *edge = edges[i].edge;
        xr_sha256_update(&ctx, edge->consumer.bytes, sizeof(edge->consumer.bytes));
        xr_sha256_update(&ctx, edge->dependency.bytes, sizeof(edge->dependency.bytes));
        for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
            hash_u64(&ctx, edge->relation[facet]);
    }
    xr_sha256_final(&ctx, out->bytes);
    xr_free(edges);
    xr_free(nodes);
    return true;
}

bool xr_dependency_graph_validate(const XrDependencyGraph *graph) {
    if (!graph || graph->node_count > graph->node_capacity ||
        graph->edge_count > graph->edge_capacity ||
        graph->node_count > XR_DEPENDENCY_GRAPH_MAX_NODES ||
        graph->edge_count > XR_DEPENDENCY_GRAPH_MAX_EDGES ||
        graph->node_capacity > XR_DEPENDENCY_GRAPH_MAX_NODES ||
        graph->edge_capacity > XR_DEPENDENCY_GRAPH_MAX_EDGES ||
        (graph->node_count != 0 && !graph->nodes) ||
        (graph->edge_count != 0 && !graph->edges))
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
            !relation_matches_nodes(graph, edge->consumer, edge->dependency,
                                    edge->relation)) {
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
