/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_invalidate.c - Deterministic facet invalidation and reason chains
 */

#include "xr_cache_invalidate.h"

#include "../base/xmalloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrPropagationState {
    XrStableId module_id;
    XrModuleFacetMask invalidated_facets;
    XrModuleFacetMask observed_facets;
    bool has_parent;
    XrStableId parent_module_id;
} XrPropagationState;

static int compare_states(const void *left, const void *right) {
    const XrPropagationState *a = (const XrPropagationState *) left;
    const XrPropagationState *b = (const XrPropagationState *) right;
    return xr_stable_id_compare(a->module_id, b->module_id);
}

static size_t find_state(const XrPropagationState *states, size_t count, XrStableId module_id) {
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = xr_stable_id_compare(states[middle].module_id, module_id);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low < count && xr_stable_id_equal(states[low].module_id, module_id))
        return low;
    return SIZE_MAX;
}

static bool event_is_valid(const XrDependencyGraph *graph, const XrInvalidationEvent *event) {
    if (!graph || !event || event->reason == XR_INVALIDATION_DEPENDENCY ||
        (event->changed_facets & ~XR_MODULE_FACET_ALL) != 0) {
        return false;
    }

    const XrModuleSummary *root = xr_dependency_graph_find_node(graph, event->root_id);
    switch (event->reason) {
    case XR_INVALIDATION_SUMMARY_CHANGED:
        return root && xr_module_summary_validate(event->replacement_summary) &&
               xr_stable_id_equal(event->root_id, event->replacement_summary->module_id);
    case XR_INVALIDATION_MODULE_ADDED:
        return !root && xr_module_summary_validate(event->replacement_summary) &&
               xr_stable_id_equal(event->root_id, event->replacement_summary->module_id);
    case XR_INVALIDATION_MODULE_DELETED:
        return root != NULL;
    case XR_INVALIDATION_MODULE_RENAMED:
        return root && xr_module_summary_validate(event->replacement_summary) &&
               !xr_stable_id_equal(event->root_id, event->replacement_summary->module_id) &&
               !xr_dependency_graph_find_node(graph, event->replacement_summary->module_id);
    case XR_INVALIDATION_GRAPH_CHANGED:
        return root && event->changed_facets != 0;
    case XR_INVALIDATION_DEPENDENCY:
        return false;
    }
    return false;
}

static XrModuleFacetMask event_changed_facets(const XrDependencyGraph *graph,
                                              const XrInvalidationEvent *event) {
    if (event->changed_facets)
        return event->changed_facets;
    const XrModuleSummary *root = xr_dependency_graph_find_node(graph, event->root_id);
    if (event->reason == XR_INVALIDATION_SUMMARY_CHANGED)
        return xr_module_summary_changed_facets(root, event->replacement_summary);
    if (event->reason == XR_INVALIDATION_MODULE_ADDED) {
        XrModuleFacetMask present = event->replacement_summary->present_facets;
        return present ? present : XR_MODULE_FACET_ALL;
    }
    return root && root->present_facets ? root->present_facets : XR_MODULE_FACET_ALL;
}

static bool initialize_states(const XrDependencyGraph *graph, XrStableId root_id,
                              XrPropagationState **out_states, size_t *out_count) {
    bool root_exists = xr_dependency_graph_find_node(graph, root_id) != NULL;
    size_t count = graph->node_count + (root_exists ? 0u : 1u);
    if (count > SIZE_MAX / sizeof(XrPropagationState))
        return false;
    XrPropagationState *states =
        (XrPropagationState *) xr_malloc(count * sizeof(*states));
    if (!states)
        return false;
    memset(states, 0, count * sizeof(*states));
    for (size_t i = 0; i < graph->node_count; i++)
        states[i].module_id = graph->nodes[i].module_id;
    if (!root_exists)
        states[count - 1u].module_id = root_id;
    qsort(states, count, sizeof(*states), compare_states);
    *out_states = states;
    *out_count = count;
    return true;
}

static void propagate_invalidations(const XrDependencyGraph *graph, XrPropagationState *states,
                                    size_t state_count) {
    bool changed;
    do {
        changed = false;
        for (size_t state_index = 0; state_index < state_count; state_index++) {
            XrPropagationState *source = &states[state_index];
            if (!source->invalidated_facets)
                continue;
            for (size_t edge_index = 0; edge_index < graph->edge_count; edge_index++) {
                const XrDependencyEdge *edge = &graph->edges[edge_index];
                if (!xr_stable_id_equal(edge->dependency, source->module_id))
                    continue;
                XrModuleFacetMask observed = source->invalidated_facets & edge->observed_facets;
                if (!observed)
                    continue;
                size_t consumer_index = find_state(states, state_count, edge->consumer);
                if (consumer_index == SIZE_MAX)
                    continue;
                XrPropagationState *consumer = &states[consumer_index];
                XrModuleFacetMask added =
                    edge->propagated_facets & ~consumer->invalidated_facets;
                if (!added)
                    continue;
                if (!consumer->invalidated_facets) {
                    consumer->has_parent = true;
                    consumer->parent_module_id = source->module_id;
                    consumer->observed_facets = observed;
                } else if (consumer->has_parent &&
                           xr_stable_id_equal(consumer->parent_module_id, source->module_id)) {
                    consumer->observed_facets |= observed;
                }
                consumer->invalidated_facets |= added;
                changed = true;
            }
        }
    } while (changed);
}

static bool materialize_result(const XrInvalidationEvent *event,
                               const XrPropagationState *states, size_t state_count,
                               XrInvalidationResult *result) {
    size_t record_count = 0;
    for (size_t i = 0; i < state_count; i++) {
        if (states[i].invalidated_facets)
            record_count++;
    }
    XrInvalidationRecord *records = NULL;
    if (record_count) {
        if (record_count > SIZE_MAX / sizeof(*records))
            return false;
        records = (XrInvalidationRecord *) xr_malloc(record_count * sizeof(*records));
        if (!records)
            return false;
    }

    size_t cursor = 0;
    for (size_t i = 0; i < state_count; i++) {
        if (!states[i].invalidated_facets)
            continue;
        bool is_root = xr_stable_id_equal(states[i].module_id, event->root_id);
        records[cursor++] = (XrInvalidationRecord) {
            .module_id = states[i].module_id,
            .invalidated_facets = states[i].invalidated_facets,
            .observed_facets = is_root ? event->changed_facets : states[i].observed_facets,
            .direct_reason = is_root ? event->reason : XR_INVALIDATION_DEPENDENCY,
            .has_parent = is_root ? false : states[i].has_parent,
            .parent_module_id = states[i].parent_module_id,
        };
    }
    result->root_id = event->root_id;
    result->root_old_fingerprint = event->old_fingerprint;
    result->root_new_fingerprint = event->new_fingerprint;
    result->records = records;
    result->record_count = record_count;
    return true;
}

static bool apply_graph_mutation(XrDependencyGraph *graph, const XrInvalidationEvent *event) {
    switch (event->reason) {
    case XR_INVALIDATION_SUMMARY_CHANGED:
        return xr_dependency_graph_replace_node(graph, event->replacement_summary);
    case XR_INVALIDATION_MODULE_ADDED:
        return xr_dependency_graph_add_node(graph, event->replacement_summary);
    case XR_INVALIDATION_MODULE_DELETED:
        return xr_dependency_graph_remove_node(graph, event->root_id);
    case XR_INVALIDATION_MODULE_RENAMED:
        return xr_dependency_graph_rename_node(graph, event->root_id,
                                               event->replacement_summary);
    case XR_INVALIDATION_GRAPH_CHANGED:
        return true;
    case XR_INVALIDATION_DEPENDENCY:
        return false;
    }
    return false;
}

bool xr_cache_invalidate_apply(XrDependencyGraph *graph, const XrInvalidationEvent *event,
                               XrInvalidationResult *out_result) {
    if (!out_result)
        return false;
    memset(out_result, 0, sizeof(*out_result));
    if (!xr_dependency_graph_validate(graph) || !event_is_valid(graph, event))
        return false;

    XrPropagationState *states = NULL;
    size_t state_count = 0;
    if (!initialize_states(graph, event->root_id, &states, &state_count))
        return false;
    size_t root_index = find_state(states, state_count, event->root_id);
    if (root_index == SIZE_MAX) {
        xr_free(states);
        return false;
    }

    XrInvalidationEvent resolved = *event;
    resolved.changed_facets = event_changed_facets(graph, event);
    states[root_index].invalidated_facets = resolved.changed_facets;
    states[root_index].observed_facets = resolved.changed_facets;
    propagate_invalidations(graph, states, state_count);

    bool ok = materialize_result(&resolved, states, state_count, out_result);
    xr_free(states);
    if (!ok)
        return false;
    if (!apply_graph_mutation(graph, &resolved)) {
        xr_invalidation_result_finalize(out_result);
        return false;
    }
    return xr_dependency_graph_validate(graph);
}

void xr_invalidation_result_finalize(XrInvalidationResult *result) {
    if (!result)
        return;
    xr_free(result->records);
    memset(result, 0, sizeof(*result));
}

const XrInvalidationRecord *xr_invalidation_result_find(const XrInvalidationResult *result,
                                                       XrStableId module_id) {
    if (!result)
        return NULL;
    size_t low = 0;
    size_t high = result->record_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = xr_stable_id_compare(result->records[middle].module_id, module_id);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low < result->record_count &&
        xr_stable_id_equal(result->records[low].module_id, module_id)) {
        return &result->records[low];
    }
    return NULL;
}

const XrInvalidationRecord *xr_invalidation_result_at(const XrInvalidationResult *result,
                                                      size_t index) {
    return result && index < result->record_count ? &result->records[index] : NULL;
}
