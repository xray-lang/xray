/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cache_invalidate.c - Deterministic facet invalidation and evidence
 *
 * KEY CONCEPT:
 *   Change masks and identities are derived from canonical summaries or a
 *   verified graph delta. Results retain every contributing parent/facet row
 *   and are independently checked before the graph transaction is published.
 */

#include "xr_cache_invalidate.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrResolvedEvent {
    XrModuleFacetMask changed_facets;
    XrFingerprint old_fingerprint;
    XrFingerprint new_fingerprint;
} XrResolvedEvent;

typedef struct XrPropagationState {
    XrStableId module_id;
    XrModuleFacetMask invalidated_facets;
    XrModuleFacetMask observed_facets;
} XrPropagationState;

static int compare_states(const void *left, const void *right) {
    const XrPropagationState *a = (const XrPropagationState *) left;
    const XrPropagationState *b = (const XrPropagationState *) right;
    return xr_stable_id_compare(a->module_id, b->module_id);
}

static int compare_evidence(const void *left, const void *right) {
    const XrInvalidationEvidence *a = (const XrInvalidationEvidence *) left;
    const XrInvalidationEvidence *b = (const XrInvalidationEvidence *) right;
    int order = xr_stable_id_compare(a->module_id, b->module_id);
    if (order)
        return order;
    order = xr_stable_id_compare(a->parent_module_id, b->parent_module_id);
    if (order)
        return order;
    if (a->observed_facet < b->observed_facet)
        return -1;
    if (a->observed_facet > b->observed_facet)
        return 1;
    if (a->invalidated_facets < b->invalidated_facets)
        return -1;
    return a->invalidated_facets > b->invalidated_facets ? 1 : 0;
}

static size_t find_state(const XrPropagationState *states, size_t count,
                         XrStableId module_id) {
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

static bool relation_is_empty(
    const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
        if (relation[facet] != 0)
            return false;
    return true;
}

static bool relation_is_valid(
    const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    if (relation_is_empty(relation))
        return false;
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
        if ((relation[facet] & ~XR_MODULE_FACET_ALL) != 0)
            return false;
    return true;
}

static bool relation_equal(
    const XrModuleFacetMask left[XR_MODULE_FACET_COUNT],
    const XrModuleFacetMask right[XR_MODULE_FACET_COUNT]) {
    return memcmp(left, right, XR_MODULE_FACET_COUNT * sizeof(*left)) == 0;
}

static bool summary_equal(const XrModuleSummary *left,
                          const XrModuleSummary *right) {
    return xr_module_summary_validate(left) && xr_module_summary_validate(right) &&
           xr_stable_id_equal(left->module_id, right->module_id) &&
           strcmp(left->canonical_key, right->canonical_key) == 0 &&
           left->present_facets == right->present_facets &&
           memcmp(left->facets, right->facets, sizeof(left->facets)) == 0;
}

static bool copy_graph(const XrDependencyGraph *source, XrDependencyGraph *out) {
    if (!out || !xr_dependency_graph_validate(source))
        return false;
    xr_dependency_graph_init(out);
    for (size_t i = 0; i < source->node_count; i++) {
        if (!xr_dependency_graph_add_node(out, &source->nodes[i])) {
            xr_dependency_graph_finalize(out);
            return false;
        }
    }
    for (size_t i = 0; i < source->edge_count; i++) {
        const XrDependencyEdge *edge = &source->edges[i];
        if (!xr_dependency_graph_add_edge(out, edge->consumer, edge->dependency,
                                          edge->relation)) {
            xr_dependency_graph_finalize(out);
            return false;
        }
    }
    return true;
}

static bool graphs_equal(const XrDependencyGraph *left,
                         const XrDependencyGraph *right) {
    if (!xr_dependency_graph_validate(left) || !xr_dependency_graph_validate(right) ||
        left->node_count != right->node_count || left->edge_count != right->edge_count) {
        return false;
    }
    for (size_t i = 0; i < left->node_count; i++) {
        const XrModuleSummary *other =
            xr_dependency_graph_find_node(right, left->nodes[i].module_id);
        if (!other || !summary_equal(&left->nodes[i], other))
            return false;
    }
    for (size_t i = 0; i < left->edge_count; i++) {
        const XrDependencyEdge *edge = &left->edges[i];
        const XrDependencyEdge *other = xr_dependency_graph_find_edge(
            right, edge->consumer, edge->dependency);
        if (!other || !relation_equal(edge->relation, other->relation))
            return false;
    }
    return true;
}

static void absent_summary_fingerprint(XrStableId module_id, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-module-summary-absent-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, module_id.bytes, sizeof(module_id.bytes));
    xr_sha256_final(&ctx, out->bytes);
}

static bool validate_delta(const XrDependencyGraph *graph,
                           const XrInvalidationEvent *event,
                           XrModuleFacetMask *out_changed) {
    const XrDependencyGraphDelta *delta = event->graph_delta;
    if (!delta || !delta->rows || delta->row_count == 0 ||
        delta->row_count > XR_INVALIDATION_MAX_DELTA_ROWS)
        return false;

    XrModuleFacetMask changed = 0;
    for (size_t i = 0; i < delta->row_count; i++) {
        const XrDependencyGraphDeltaRow *row = &delta->rows[i];
        bool old_empty = relation_is_empty(row->old_relation);
        bool new_empty = relation_is_empty(row->new_relation);
        if (!xr_stable_id_equal(row->consumer, event->root_id) ||
            (old_empty && new_empty) ||
            (!old_empty && !relation_is_valid(row->old_relation)) ||
            (!new_empty && !relation_is_valid(row->new_relation)) ||
            relation_equal(row->old_relation, row->new_relation)) {
            return false;
        }
        if (i != 0 &&
            xr_stable_id_compare(delta->rows[i - 1u].dependency,
                                 row->dependency) >= 0) {
            return false;
        }

        const XrDependencyEdge *current = xr_dependency_graph_find_edge(
            graph, row->consumer, row->dependency);
        if ((old_empty && current) ||
            (!old_empty && (!current || !relation_equal(current->relation,
                                                        row->old_relation)))) {
            return false;
        }
        for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
            if (row->old_relation[facet] != row->new_relation[facet])
                changed |= row->old_relation[facet] | row->new_relation[facet];
        }
    }
    if (!changed)
        return false;
    *out_changed = changed;
    return true;
}

static bool apply_delta(XrDependencyGraph *graph,
                        const XrDependencyGraphDelta *delta) {
    for (size_t i = 0; i < delta->row_count; i++) {
        const XrDependencyGraphDeltaRow *row = &delta->rows[i];
        bool old_empty = relation_is_empty(row->old_relation);
        bool new_empty = relation_is_empty(row->new_relation);
        bool ok;
        if (old_empty) {
            ok = xr_dependency_graph_add_edge(graph, row->consumer, row->dependency,
                                              row->new_relation);
        } else if (new_empty) {
            ok = xr_dependency_graph_remove_edge(graph, row->consumer,
                                                 row->dependency);
        } else {
            ok = xr_dependency_graph_add_edge(graph, row->consumer, row->dependency,
                                              row->new_relation);
        }
        if (!ok)
            return false;
    }
    return xr_dependency_graph_validate(graph);
}

static bool event_shape_is_valid(const XrDependencyGraph *graph,
                                 const XrInvalidationEvent *event,
                                 XrResolvedEvent *resolved) {
    if (!graph || !event || !resolved ||
        event->reason == XR_INVALIDATION_DEPENDENCY)
        return false;
    const XrModuleSummary *root =
        xr_dependency_graph_find_node(graph, event->root_id);
    memset(resolved, 0, sizeof(*resolved));

    switch (event->reason) {
    case XR_INVALIDATION_SUMMARY_CHANGED:
        if (event->graph_delta || !root ||
            !xr_module_summary_validate(event->replacement_summary) ||
            !xr_stable_id_equal(event->root_id,
                                event->replacement_summary->module_id)) {
            return false;
        }
        resolved->changed_facets =
            xr_module_summary_changed_facets(root, event->replacement_summary);
        return resolved->changed_facets != 0 &&
               xr_module_summary_fingerprint(root, &resolved->old_fingerprint) &&
               xr_module_summary_fingerprint(event->replacement_summary,
                                             &resolved->new_fingerprint);
    case XR_INVALIDATION_MODULE_ADDED:
        if (event->graph_delta || root ||
            !xr_module_summary_validate(event->replacement_summary) ||
            !xr_stable_id_equal(event->root_id,
                                event->replacement_summary->module_id) ||
            event->replacement_summary->present_facets == 0) {
            return false;
        }
        resolved->changed_facets = event->replacement_summary->present_facets;
        absent_summary_fingerprint(event->root_id, &resolved->old_fingerprint);
        return xr_module_summary_fingerprint(event->replacement_summary,
                                             &resolved->new_fingerprint);
    case XR_INVALIDATION_MODULE_DELETED:
        if (event->replacement_summary || event->graph_delta || !root ||
            root->present_facets == 0)
            return false;
        resolved->changed_facets = root->present_facets;
        if (!xr_module_summary_fingerprint(root, &resolved->old_fingerprint))
            return false;
        absent_summary_fingerprint(event->root_id, &resolved->new_fingerprint);
        return true;
    case XR_INVALIDATION_MODULE_RENAMED:
        if (event->graph_delta || !root ||
            !xr_module_summary_validate(event->replacement_summary) ||
            xr_stable_id_equal(event->root_id,
                               event->replacement_summary->module_id) ||
            xr_dependency_graph_find_node(graph,
                                          event->replacement_summary->module_id)) {
            return false;
        }
        resolved->changed_facets =
            root->present_facets | event->replacement_summary->present_facets;
        return resolved->changed_facets != 0 &&
               xr_module_summary_fingerprint(root, &resolved->old_fingerprint) &&
               xr_module_summary_fingerprint(event->replacement_summary,
                                             &resolved->new_fingerprint);
    case XR_INVALIDATION_GRAPH_CHANGED:
        if (event->replacement_summary || !root ||
            !validate_delta(graph, event, &resolved->changed_facets)) {
            return false;
        }
        return xr_dependency_graph_fingerprint(graph,
                                               &resolved->old_fingerprint);
    case XR_INVALIDATION_DEPENDENCY:
        return false;
    }
    return false;
}

static bool build_next_graph(const XrDependencyGraph *graph,
                             const XrInvalidationEvent *event,
                             XrDependencyGraph *out,
                             XrResolvedEvent *resolved) {
    if (!xr_dependency_graph_validate(graph) ||
        !event_shape_is_valid(graph, event, resolved) || !copy_graph(graph, out)) {
        return false;
    }

    bool ok = false;
    switch (event->reason) {
    case XR_INVALIDATION_SUMMARY_CHANGED:
        ok = xr_dependency_graph_replace_node(out, event->replacement_summary);
        break;
    case XR_INVALIDATION_MODULE_ADDED:
        ok = xr_dependency_graph_add_node(out, event->replacement_summary);
        break;
    case XR_INVALIDATION_MODULE_DELETED:
        ok = xr_dependency_graph_remove_node(out, event->root_id);
        break;
    case XR_INVALIDATION_MODULE_RENAMED:
        ok = xr_dependency_graph_rename_node(out, event->root_id,
                                             event->replacement_summary);
        break;
    case XR_INVALIDATION_GRAPH_CHANGED:
        ok = apply_delta(out, event->graph_delta);
        break;
    case XR_INVALIDATION_DEPENDENCY:
        break;
    }
    if (!ok || !xr_dependency_graph_validate(out)) {
        xr_dependency_graph_finalize(out);
        return false;
    }
    if (event->reason == XR_INVALIDATION_GRAPH_CHANGED &&
        !xr_dependency_graph_fingerprint(out, &resolved->new_fingerprint)) {
        xr_dependency_graph_finalize(out);
        return false;
    }
    return true;
}

static bool initialize_states(const XrDependencyGraph *graph, XrStableId root_id,
                              XrPropagationState **out_states,
                              size_t *out_count) {
    bool root_exists = xr_dependency_graph_find_node(graph, root_id) != NULL;
    size_t count = graph->node_count + (root_exists ? 0u : 1u);
    if (count == 0 || count > XR_DEPENDENCY_GRAPH_MAX_NODES + 1u ||
        count > SIZE_MAX / sizeof(XrPropagationState)) {
        return false;
    }
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

static bool propagate_with_evidence(const XrDependencyGraph *graph,
                                    XrPropagationState *states,
                                    size_t state_count,
                                    XrInvalidationEvidence **out_evidence,
                                    size_t *out_evidence_count) {
    if (graph->edge_count > XR_INVALIDATION_MAX_EVIDENCE_ROWS /
                                XR_MODULE_FACET_COUNT)
        return false;
    size_t capacity = graph->edge_count * XR_MODULE_FACET_COUNT;
    XrInvalidationEvidence *evidence = NULL;
    uint8_t *seen = NULL;
    if (capacity) {
        evidence = (XrInvalidationEvidence *) xr_malloc(capacity * sizeof(*evidence));
        seen = (uint8_t *) xr_malloc(capacity);
        if (!evidence || !seen) {
            xr_free(seen);
            xr_free(evidence);
            return false;
        }
        memset(seen, 0, capacity);
    }

    size_t evidence_count = 0;
    size_t iteration = 0;
    size_t iteration_limit = state_count * XR_MODULE_FACET_COUNT + 1u;
    bool changed;
    do {
        changed = false;
        for (size_t edge_index = 0; edge_index < graph->edge_count; edge_index++) {
            const XrDependencyEdge *edge = &graph->edges[edge_index];
            size_t source_index = find_state(states, state_count, edge->dependency);
            size_t consumer_index = find_state(states, state_count, edge->consumer);
            if (source_index == SIZE_MAX || consumer_index == SIZE_MAX)
                goto fail;
            XrPropagationState *source = &states[source_index];
            XrPropagationState *consumer = &states[consumer_index];
            for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
                XrModuleFacetMask bit = XR_MODULE_FACET_BIT(facet);
                if ((source->invalidated_facets & bit) == 0 ||
                    edge->relation[facet] == 0)
                    continue;
                size_t evidence_index = edge_index * XR_MODULE_FACET_COUNT + facet;
                if (!seen[evidence_index]) {
                    seen[evidence_index] = 1;
                    evidence[evidence_count++] = (XrInvalidationEvidence) {
                        .module_id = edge->consumer,
                        .parent_module_id = edge->dependency,
                        .observed_facet = bit,
                        .invalidated_facets = edge->relation[facet],
                    };
                }
                consumer->observed_facets |= bit;
                XrModuleFacetMask added =
                    edge->relation[facet] & ~consumer->invalidated_facets;
                if (added) {
                    consumer->invalidated_facets |= added;
                    changed = true;
                }
            }
        }
        iteration++;
        if (changed && iteration > iteration_limit)
            goto fail;
    } while (changed);

    if (evidence_count > 1u)
        qsort(evidence, evidence_count, sizeof(*evidence), compare_evidence);
    xr_free(seen);
    *out_evidence = evidence;
    *out_evidence_count = evidence_count;
    return true;

fail:
    xr_free(seen);
    xr_free(evidence);
    return false;
}

static bool materialize_result(const XrInvalidationEvent *event,
                               const XrResolvedEvent *resolved,
                               const XrPropagationState *states,
                               size_t state_count,
                               XrInvalidationEvidence *evidence,
                               size_t evidence_count,
                               XrInvalidationResult *result) {
    size_t record_count = 0;
    for (size_t i = 0; i < state_count; i++)
        if (states[i].invalidated_facets)
            record_count++;
    if (record_count == 0 || record_count > XR_DEPENDENCY_GRAPH_MAX_NODES + 1u ||
        record_count > SIZE_MAX / sizeof(XrInvalidationRecord)) {
        return false;
    }

    XrInvalidationRecord *records =
        (XrInvalidationRecord *) xr_malloc(record_count * sizeof(*records));
    if (!records)
        return false;
    size_t record_cursor = 0;
    size_t evidence_cursor = 0;
    for (size_t i = 0; i < state_count; i++) {
        if (!states[i].invalidated_facets)
            continue;
        while (evidence_cursor < evidence_count &&
               xr_stable_id_compare(evidence[evidence_cursor].module_id,
                                    states[i].module_id) < 0) {
            xr_free(records);
            return false;
        }
        size_t start = evidence_cursor;
        while (evidence_cursor < evidence_count &&
               xr_stable_id_equal(evidence[evidence_cursor].module_id,
                                  states[i].module_id)) {
            evidence_cursor++;
        }
        bool is_root = xr_stable_id_equal(states[i].module_id, event->root_id);
        records[record_cursor++] = (XrInvalidationRecord) {
            .module_id = states[i].module_id,
            .invalidated_facets = states[i].invalidated_facets,
            .observed_facets = states[i].observed_facets,
            .direct_reason = is_root ? event->reason : XR_INVALIDATION_DEPENDENCY,
            .evidence_start = start,
            .evidence_count = evidence_cursor - start,
        };
    }
    if (evidence_cursor != evidence_count) {
        xr_free(records);
        return false;
    }
    result->root_id = event->root_id;
    result->root_old_fingerprint = resolved->old_fingerprint;
    result->root_new_fingerprint = resolved->new_fingerprint;
    result->records = records;
    result->record_count = record_count;
    result->evidence = evidence;
    result->evidence_count = evidence_count;
    return true;
}

static bool compute_independent_closure(const XrDependencyGraph *graph,
                                        XrStableId root_id,
                                        XrModuleFacetMask changed_facets,
                                        XrPropagationState **out_states,
                                        size_t *out_count) {
    XrPropagationState *states = NULL;
    size_t state_count = 0;
    if (!initialize_states(graph, root_id, &states, &state_count))
        return false;
    size_t root_index = find_state(states, state_count, root_id);
    if (root_index == SIZE_MAX) {
        xr_free(states);
        return false;
    }
    states[root_index].invalidated_facets = changed_facets;
    states[root_index].observed_facets = changed_facets;

    size_t iteration = 0;
    size_t iteration_limit = state_count * XR_MODULE_FACET_COUNT + 1u;
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < graph->edge_count; i++) {
            const XrDependencyEdge *edge = &graph->edges[i];
            size_t source = find_state(states, state_count, edge->dependency);
            size_t consumer = find_state(states, state_count, edge->consumer);
            if (source == SIZE_MAX || consumer == SIZE_MAX) {
                xr_free(states);
                return false;
            }
            for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
                XrModuleFacetMask bit = XR_MODULE_FACET_BIT(facet);
                if ((states[source].invalidated_facets & bit) == 0 ||
                    edge->relation[facet] == 0)
                    continue;
                states[consumer].observed_facets |= bit;
                XrModuleFacetMask added =
                    edge->relation[facet] & ~states[consumer].invalidated_facets;
                states[consumer].invalidated_facets |= edge->relation[facet];
                changed = changed || added != 0;
            }
        }
        iteration++;
        if (changed && iteration > iteration_limit) {
            xr_free(states);
            return false;
        }
    } while (changed);
    *out_states = states;
    *out_count = state_count;
    return true;
}

static bool build_expected_evidence(const XrDependencyGraph *graph,
                                    const XrPropagationState *states,
                                    size_t state_count,
                                    XrInvalidationEvidence **out_evidence,
                                    size_t *out_count) {
    size_t capacity = graph->edge_count * XR_MODULE_FACET_COUNT;
    XrInvalidationEvidence *evidence = NULL;
    if (capacity) {
        evidence = (XrInvalidationEvidence *) xr_malloc(capacity * sizeof(*evidence));
        if (!evidence)
            return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < graph->edge_count; i++) {
        const XrDependencyEdge *edge = &graph->edges[i];
        size_t source = find_state(states, state_count, edge->dependency);
        if (source == SIZE_MAX) {
            xr_free(evidence);
            return false;
        }
        for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
            XrModuleFacetMask bit = XR_MODULE_FACET_BIT(facet);
            if ((states[source].invalidated_facets & bit) == 0 ||
                edge->relation[facet] == 0)
                continue;
            evidence[count++] = (XrInvalidationEvidence) {
                .module_id = edge->consumer,
                .parent_module_id = edge->dependency,
                .observed_facet = bit,
                .invalidated_facets = edge->relation[facet],
            };
        }
    }
    if (count > 1u)
        qsort(evidence, count, sizeof(*evidence), compare_evidence);
    *out_evidence = evidence;
    *out_count = count;
    return true;
}

static bool verify_evidence(const XrInvalidationResult *result,
                            const XrInvalidationEvidence *expected,
                            size_t expected_count) {
    if (result->evidence_count != expected_count ||
        result->evidence_count > XR_INVALIDATION_MAX_EVIDENCE_ROWS ||
        (result->evidence_count != 0 && !result->evidence)) {
        return false;
    }
    for (size_t i = 0; i < expected_count; i++) {
        const XrInvalidationEvidence *actual = &result->evidence[i];
        if (i != 0 && compare_evidence(&result->evidence[i - 1u], actual) >= 0)
            return false;
        if (!xr_stable_id_equal(actual->module_id, expected[i].module_id) ||
            !xr_stable_id_equal(actual->parent_module_id,
                                expected[i].parent_module_id) ||
            actual->observed_facet != expected[i].observed_facet ||
            actual->invalidated_facets != expected[i].invalidated_facets) {
            return false;
        }
    }
    return true;
}

static bool verify_records(const XrInvalidationEvent *event,
                           const XrPropagationState *states,
                           size_t state_count,
                           const XrInvalidationResult *result) {
    size_t expected_records = 0;
    for (size_t i = 0; i < state_count; i++)
        if (states[i].invalidated_facets)
            expected_records++;
    if (result->record_count != expected_records || result->record_count == 0 ||
        result->record_count > XR_DEPENDENCY_GRAPH_MAX_NODES + 1u ||
        !result->records)
        return false;

    size_t record_index = 0;
    size_t evidence_cursor = 0;
    for (size_t i = 0; i < state_count; i++) {
        if (!states[i].invalidated_facets)
            continue;
        const XrInvalidationRecord *record = &result->records[record_index];
        if (record_index != 0 &&
            xr_stable_id_compare(result->records[record_index - 1u].module_id,
                                 record->module_id) >= 0)
            return false;
        bool is_root = xr_stable_id_equal(states[i].module_id, event->root_id);
        size_t expected_start = evidence_cursor;
        while (evidence_cursor < result->evidence_count &&
               xr_stable_id_equal(result->evidence[evidence_cursor].module_id,
                                  states[i].module_id)) {
            evidence_cursor++;
        }
        if (!xr_stable_id_equal(record->module_id, states[i].module_id) ||
            record->invalidated_facets != states[i].invalidated_facets ||
            record->observed_facets != states[i].observed_facets ||
            record->direct_reason !=
                (is_root ? event->reason : XR_INVALIDATION_DEPENDENCY) ||
            record->evidence_start != expected_start ||
            record->evidence_count != evidence_cursor - expected_start) {
            return false;
        }
        record_index++;
    }
    return record_index == result->record_count &&
           evidence_cursor == result->evidence_count;
}

bool xr_cache_invalidation_verify(const XrDependencyGraph *before,
                                  const XrInvalidationEvent *event,
                                  const XrDependencyGraph *after,
                                  const XrInvalidationResult *result) {
    if (!before || !event || !after || !result ||
        !xr_dependency_graph_validate(before) ||
        !xr_dependency_graph_validate(after)) {
        return false;
    }

    XrDependencyGraph expected_graph;
    XrResolvedEvent resolved;
    if (!build_next_graph(before, event, &expected_graph, &resolved))
        return false;
    bool graph_ok = graphs_equal(&expected_graph, after);
    xr_dependency_graph_finalize(&expected_graph);
    if (!graph_ok || !xr_stable_id_equal(result->root_id, event->root_id) ||
        !xr_fingerprint_equal(result->root_old_fingerprint,
                              resolved.old_fingerprint) ||
        !xr_fingerprint_equal(result->root_new_fingerprint,
                              resolved.new_fingerprint)) {
        return false;
    }

    XrPropagationState *states = NULL;
    size_t state_count = 0;
    if (!compute_independent_closure(before, event->root_id,
                                     resolved.changed_facets, &states,
                                     &state_count)) {
        return false;
    }
    XrInvalidationEvidence *evidence = NULL;
    size_t evidence_count = 0;
    if (!build_expected_evidence(before, states, state_count, &evidence,
                                 &evidence_count)) {
        xr_free(states);
        return false;
    }
    bool ok = verify_evidence(result, evidence, evidence_count) &&
              verify_records(event, states, state_count, result);
    xr_free(evidence);
    xr_free(states);
    return ok;
}

bool xr_cache_invalidate_apply(XrDependencyGraph *graph,
                               const XrInvalidationEvent *event,
                               XrInvalidationResult *out_result) {
    if (!out_result)
        return false;
    memset(out_result, 0, sizeof(*out_result));
    if (!xr_dependency_graph_validate(graph))
        return false;

    XrDependencyGraph next_graph;
    XrResolvedEvent resolved;
    if (!build_next_graph(graph, event, &next_graph, &resolved))
        return false;
    XrPropagationState *states = NULL;
    size_t state_count = 0;
    if (!initialize_states(graph, event->root_id, &states, &state_count)) {
        xr_dependency_graph_finalize(&next_graph);
        return false;
    }
    size_t root_index = find_state(states, state_count, event->root_id);
    if (root_index == SIZE_MAX) {
        xr_free(states);
        xr_dependency_graph_finalize(&next_graph);
        return false;
    }
    states[root_index].invalidated_facets = resolved.changed_facets;
    states[root_index].observed_facets = resolved.changed_facets;

    XrInvalidationEvidence *evidence = NULL;
    size_t evidence_count = 0;
    bool ok = propagate_with_evidence(graph, states, state_count, &evidence,
                                      &evidence_count) &&
              materialize_result(event, &resolved, states, state_count, evidence,
                                 evidence_count, out_result);
    xr_free(states);
    if (!ok) {
        xr_free(evidence);
        xr_dependency_graph_finalize(&next_graph);
        return false;
    }
    if (!xr_cache_invalidation_verify(graph, event, &next_graph, out_result)) {
        xr_invalidation_result_finalize(out_result);
        xr_dependency_graph_finalize(&next_graph);
        return false;
    }

    XrDependencyGraph previous = *graph;
    *graph = next_graph;
    xr_dependency_graph_finalize(&previous);
    return true;
}

void xr_invalidation_result_finalize(XrInvalidationResult *result) {
    if (!result)
        return;
    xr_free(result->evidence);
    xr_free(result->records);
    memset(result, 0, sizeof(*result));
}

const XrInvalidationRecord *xr_invalidation_result_find(
    const XrInvalidationResult *result, XrStableId module_id) {
    if (!result)
        return NULL;
    size_t low = 0;
    size_t high = result->record_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = xr_stable_id_compare(result->records[middle].module_id,
                                         module_id);
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

const XrInvalidationRecord *xr_invalidation_result_at(
    const XrInvalidationResult *result, size_t index) {
    return result && index < result->record_count ? &result->records[index]
                                                   : NULL;
}

const XrInvalidationEvidence *xr_invalidation_evidence_at(
    const XrInvalidationResult *result, size_t index) {
    return result && index < result->evidence_count ? &result->evidence[index]
                                                     : NULL;
}

typedef struct XrExplanationSearchState {
    size_t record_index;
    unsigned facet_index;
    size_t prior_state;
    size_t evidence_index;
} XrExplanationSearchState;

static bool single_facet_mask(XrModuleFacetMask mask) {
    return mask != 0 && (mask & ~XR_MODULE_FACET_ALL) == 0 &&
           (mask & (mask - UINT64_C(1))) == 0;
}

static unsigned single_facet_index(XrModuleFacetMask mask) {
    unsigned index = 0;
    while ((mask >> index) != UINT64_C(1))
        index++;
    return index;
}

static bool invalidation_result_is_explainable(
    const XrInvalidationResult *result) {
    if (!result || !result->records || result->record_count == 0 ||
        result->record_count > XR_DEPENDENCY_GRAPH_MAX_NODES + 1u ||
        (result->evidence_count != 0 && !result->evidence) ||
        result->evidence_count > XR_INVALIDATION_MAX_EVIDENCE_ROWS) {
        return false;
    }

    size_t evidence_cursor = 0;
    bool found_root = false;
    for (size_t i = 0; i < result->record_count; i++) {
        const XrInvalidationRecord *record = &result->records[i];
        if (record->invalidated_facets == 0 ||
            (record->invalidated_facets & ~XR_MODULE_FACET_ALL) != 0 ||
            (record->observed_facets & ~XR_MODULE_FACET_ALL) != 0 ||
            record->evidence_start != evidence_cursor ||
            record->evidence_count > result->evidence_count - evidence_cursor ||
            (i != 0 && xr_stable_id_compare(result->records[i - 1u].module_id,
                                             record->module_id) >= 0)) {
            return false;
        }
        bool is_root = xr_stable_id_equal(record->module_id, result->root_id);
        if ((is_root && (found_root || record->direct_reason <
                                          XR_INVALIDATION_SUMMARY_CHANGED ||
                         record->direct_reason > XR_INVALIDATION_GRAPH_CHANGED)) ||
            (!is_root && record->direct_reason != XR_INVALIDATION_DEPENDENCY)) {
            return false;
        }
        found_root |= is_root;
        for (size_t j = 0; j < record->evidence_count; j++) {
            const XrInvalidationEvidence *evidence =
                &result->evidence[evidence_cursor + j];
            const XrInvalidationRecord *parent = xr_invalidation_result_find(
                result, evidence->parent_module_id);
            if (!xr_stable_id_equal(evidence->module_id, record->module_id) ||
                !single_facet_mask(evidence->observed_facet) ||
                evidence->invalidated_facets == 0 ||
                (evidence->invalidated_facets &
                 ~record->invalidated_facets) != 0 ||
                !parent ||
                (parent->invalidated_facets & evidence->observed_facet) == 0) {
                return false;
            }
        }
        evidence_cursor += record->evidence_count;
    }
    return found_root && evidence_cursor == result->evidence_count;
}

static bool build_explanation_steps(const XrInvalidationResult *result,
                                    size_t subject_record,
                                    unsigned subject_facet,
                                    XrInvalidationExplanation *out) {
    if (result->record_count > SIZE_MAX / XR_MODULE_FACET_COUNT)
        return false;
    size_t state_capacity = result->record_count * XR_MODULE_FACET_COUNT;
    XrExplanationSearchState *states =
        (XrExplanationSearchState *) xr_malloc(state_capacity * sizeof(*states));
    uint8_t *visited = (uint8_t *) xr_malloc(state_capacity);
    if (!states || !visited) {
        xr_free(visited);
        xr_free(states);
        return false;
    }
    memset(visited, 0, state_capacity);
    states[0] = (XrExplanationSearchState) {
        .record_index = subject_record,
        .facet_index = subject_facet,
        .prior_state = SIZE_MAX,
        .evidence_index = SIZE_MAX,
    };
    visited[subject_record * XR_MODULE_FACET_COUNT + subject_facet] = 1;
    size_t state_count = 1;
    size_t root_state = SIZE_MAX;

    for (size_t cursor = 0; cursor < state_count; cursor++) {
        const XrExplanationSearchState *state = &states[cursor];
        const XrInvalidationRecord *record =
            &result->records[state->record_index];
        XrModuleFacetMask facet = XR_MODULE_FACET_BIT(state->facet_index);
        if (xr_stable_id_equal(record->module_id, result->root_id)) {
            root_state = cursor;
            break;
        }
        for (size_t i = 0; i < record->evidence_count; i++) {
            size_t evidence_index = record->evidence_start + i;
            const XrInvalidationEvidence *evidence =
                &result->evidence[evidence_index];
            if ((evidence->invalidated_facets & facet) == 0)
                continue;
            const XrInvalidationRecord *parent = xr_invalidation_result_find(
                result, evidence->parent_module_id);
            size_t parent_record = (size_t) (parent - result->records);
            unsigned parent_facet = single_facet_index(evidence->observed_facet);
            size_t slot = parent_record * XR_MODULE_FACET_COUNT + parent_facet;
            if (visited[slot])
                continue;
            visited[slot] = 1;
            states[state_count++] = (XrExplanationSearchState) {
                .record_index = parent_record,
                .facet_index = parent_facet,
                .prior_state = cursor,
                .evidence_index = evidence_index,
            };
        }
    }

    if (root_state == SIZE_MAX) {
        xr_free(visited);
        xr_free(states);
        return false;
    }
    size_t step_count = 0;
    for (size_t state = root_state; states[state].prior_state != SIZE_MAX;
         state = states[state].prior_state) {
        step_count++;
    }
    XrInvalidationExplanationStep *steps = NULL;
    if (step_count) {
        steps = (XrInvalidationExplanationStep *) xr_malloc(step_count * sizeof(*steps));
        if (!steps) {
            xr_free(visited);
            xr_free(states);
            return false;
        }
        size_t position = step_count;
        for (size_t state = root_state; states[state].prior_state != SIZE_MAX;
             state = states[state].prior_state) {
            size_t prior = states[state].prior_state;
            const XrInvalidationEvidence *evidence =
                &result->evidence[states[state].evidence_index];
            steps[--position] = (XrInvalidationExplanationStep) {
                .module_id = evidence->module_id,
                .parent_module_id = evidence->parent_module_id,
                .invalidated_facet =
                    XR_MODULE_FACET_BIT(states[prior].facet_index),
                .observed_facet = evidence->observed_facet,
            };
        }
    }
    out->steps = steps;
    out->step_count = step_count;
    xr_free(visited);
    xr_free(states);
    return true;
}

bool xr_invalidation_explain(const XrInvalidationResult *result,
                             XrStableId subject_id,
                             XrModuleFacetMask subject_facet,
                             XrInvalidationExplanation *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!single_facet_mask(subject_facet) ||
        !invalidation_result_is_explainable(result)) {
        return false;
    }
    const XrInvalidationRecord *subject =
        xr_invalidation_result_find(result, subject_id);
    const XrInvalidationRecord *root =
        xr_invalidation_result_find(result, result->root_id);
    if (!subject || !root ||
        (subject->invalidated_facets & subject_facet) == 0) {
        return false;
    }
    if (!build_explanation_steps(result,
                                 (size_t) (subject - result->records),
                                 single_facet_index(subject_facet), out)) {
        return false;
    }
    out->root_id = result->root_id;
    out->subject_id = subject_id;
    out->root_old_fingerprint = result->root_old_fingerprint;
    out->root_new_fingerprint = result->root_new_fingerprint;
    out->subject_facet = subject_facet;
    out->root_reason = root->direct_reason;
    return true;
}

void xr_invalidation_explanation_finalize(
    XrInvalidationExplanation *explanation) {
    if (!explanation)
        return;
    xr_free(explanation->steps);
    memset(explanation, 0, sizeof(*explanation));
}
