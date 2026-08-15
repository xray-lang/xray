/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dependency_graph.c - Deterministic module invalidation tests
 *
 * KEY CONCEPT:
 *   Fixtures assert both the affected and unaffected sets. A successful build
 *   is not evidence that an incremental invalidation set was precise.
 */

#include "../test_framework.h"

#include "incremental/xr_cache_invalidate.h"

#include <string.h>

typedef struct TestGraphIds {
    XrStableId root;
    XrStableId body;
    XrStableId signature;
    XrStableId layout;
    XrStableId leaf;
    XrStableId untouched;
} TestGraphIds;

static XrFingerprint test_fingerprint(uint8_t seed) {
    XrFingerprint fingerprint;
    for (size_t i = 0; i < sizeof(fingerprint.bytes); i++)
        fingerprint.bytes[i] = (uint8_t) (seed + (uint8_t) i);
    return fingerprint;
}

static bool init_test_summary(XrModuleSummary *summary, const char *key, uint8_t seed) {
    if (!xr_module_summary_init(summary, key))
        return false;
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++) {
        if (!xr_module_summary_set_fingerprint(summary, (XrModuleSummaryFacet) facet,
                                               test_fingerprint((uint8_t) (seed + facet)))) {
            xr_module_summary_finalize(summary);
            return false;
        }
    }
    return true;
}

static bool add_test_module(XrDependencyGraph *graph, const char *key, uint8_t seed,
                            XrStableId *out_id) {
    XrModuleSummary summary;
    if (!init_test_summary(&summary, key, seed))
        return false;
    *out_id = summary.module_id;
    bool added = xr_dependency_graph_add_node(graph, &summary);
    xr_module_summary_finalize(&summary);
    return added;
}

static bool build_facet_graph(XrDependencyGraph *graph, TestGraphIds *ids, bool reverse_edges) {
    xr_dependency_graph_init(graph);
    if (!add_test_module(graph, "pkg/root", 1, &ids->root) ||
        !add_test_module(graph, "pkg/body", 20, &ids->body) ||
        !add_test_module(graph, "pkg/signature", 40, &ids->signature) ||
        !add_test_module(graph, "pkg/layout", 60, &ids->layout) ||
        !add_test_module(graph, "pkg/leaf", 80, &ids->leaf) ||
        !add_test_module(graph, "pkg/untouched", 100, &ids->untouched)) {
        xr_dependency_graph_finalize(graph);
        return false;
    }

    typedef struct TestEdge {
        XrStableId consumer;
        XrStableId dependency;
        XrModuleFacetMask observed;
        XrModuleFacetMask propagated;
    } TestEdge;
    TestEdge edges[] = {
        {.consumer = ids->leaf,
         .dependency = ids->body,
         .observed = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
         .propagated = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE)},
        {.consumer = ids->layout,
         .dependency = ids->root,
         .observed = XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
         .propagated = XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT)},
        {.consumer = ids->signature,
         .dependency = ids->root,
         .observed = XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE),
         .propagated = XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE)},
        {.consumer = ids->body,
         .dependency = ids->root,
         .observed = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
         .propagated = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE)},
        {.consumer = ids->untouched,
         .dependency = ids->root,
         .observed = XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY),
         .propagated = XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY)},
    };
    size_t edge_count = sizeof(edges) / sizeof(edges[0]);
    for (size_t i = 0; i < edge_count; i++) {
        size_t index = reverse_edges ? edge_count - i - 1u : i;
        XrModuleFacetMask relation[XR_MODULE_FACET_COUNT] = {0};
        for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
            if ((edges[index].observed & XR_MODULE_FACET_BIT(facet)) != 0)
                relation[facet] = edges[index].propagated;
        if (!xr_dependency_graph_add_edge(graph, edges[index].consumer,
                                          edges[index].dependency, relation)) {
            xr_dependency_graph_finalize(graph);
            return false;
        }
    }
    return xr_dependency_graph_validate(graph);
}

static bool result_is_stably_ordered(const XrInvalidationResult *result) {
    for (size_t i = 1; i < result->record_count; i++) {
        if (xr_stable_id_compare(result->records[i - 1u].module_id,
                                 result->records[i].module_id) >= 0) {
            return false;
        }
    }
    return true;
}

static bool invalidation_results_equal(const XrInvalidationResult *left,
                                       const XrInvalidationResult *right) {
    if (left->record_count != right->record_count ||
        left->evidence_count != right->evidence_count ||
        !xr_stable_id_equal(left->root_id, right->root_id) ||
        !xr_fingerprint_equal(left->root_old_fingerprint,
                              right->root_old_fingerprint) ||
        !xr_fingerprint_equal(left->root_new_fingerprint,
                              right->root_new_fingerprint)) {
        return false;
    }
    for (size_t i = 0; i < left->record_count; i++) {
        const XrInvalidationRecord *a = &left->records[i];
        const XrInvalidationRecord *b = &right->records[i];
        if (!xr_stable_id_equal(a->module_id, b->module_id) ||
            a->invalidated_facets != b->invalidated_facets ||
            a->observed_facets != b->observed_facets || a->direct_reason != b->direct_reason ||
            a->evidence_start != b->evidence_start ||
            a->evidence_count != b->evidence_count) {
            return false;
        }
    }
    for (size_t i = 0; i < left->evidence_count; i++) {
        const XrInvalidationEvidence *a = &left->evidence[i];
        const XrInvalidationEvidence *b = &right->evidence[i];
        if (!xr_stable_id_equal(a->module_id, b->module_id) ||
            !xr_stable_id_equal(a->parent_module_id, b->parent_module_id) ||
            a->observed_facet != b->observed_facet ||
            a->invalidated_facets != b->invalidated_facets) {
            return false;
        }
    }
    return true;
}

TEST(module_resolution_fingerprint_is_consumer_exact_and_order_independent) {
    XrDependencyGraph forward;
    XrDependencyGraph reverse;
    TestGraphIds forward_ids;
    TestGraphIds reverse_ids;
    ASSERT_TRUE(build_facet_graph(&forward, &forward_ids, false));
    ASSERT_TRUE(build_facet_graph(&reverse, &reverse_ids, true));

    XrFingerprint forward_body;
    XrFingerprint reverse_body;
    XrFingerprint signature;
    ASSERT_TRUE(xr_dependency_graph_module_resolution_fingerprint(
        &forward, forward_ids.body, &forward_body));
    ASSERT_TRUE(xr_dependency_graph_module_resolution_fingerprint(
        &reverse, reverse_ids.body, &reverse_body));
    ASSERT_TRUE(xr_dependency_graph_module_resolution_fingerprint(
        &forward, forward_ids.signature, &signature));
    ASSERT_TRUE(xr_fingerprint_equal(forward_body, reverse_body));
    ASSERT_FALSE(xr_fingerprint_equal(forward_body, signature));

    XrStableId missing = forward_ids.body;
    missing.bytes[0] ^= UINT8_C(1);
    ASSERT_FALSE(xr_dependency_graph_module_resolution_fingerprint(
        &forward, missing, &signature));
    ASSERT_FALSE(xr_dependency_graph_module_resolution_fingerprint(
        NULL, forward_ids.body, &signature));
    ASSERT_FALSE(xr_dependency_graph_module_resolution_fingerprint(
        &forward, forward_ids.body, NULL));

    xr_dependency_graph_finalize(&reverse);
    xr_dependency_graph_finalize(&forward);
}

static bool test_relation_is_empty(
    const XrModuleFacetMask relation[XR_MODULE_FACET_COUNT]) {
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
        if (relation[facet] != 0)
            return false;
    return true;
}

static bool apply_test_delta(XrDependencyGraph *graph,
                             const XrDependencyGraphDelta *delta,
                             bool forward) {
    for (size_t cursor = 0; cursor < delta->row_count; cursor++) {
        size_t index = forward ? cursor : delta->row_count - cursor - 1u;
        const XrDependencyGraphDeltaRow *row = &delta->rows[index];
        const XrModuleFacetMask *from =
            forward ? row->old_relation : row->new_relation;
        const XrModuleFacetMask *to =
            forward ? row->new_relation : row->old_relation;
        bool from_empty = test_relation_is_empty(from);
        bool to_empty = test_relation_is_empty(to);
        bool ok;
        if (from_empty) {
            ok = xr_dependency_graph_add_edge(
                graph, row->consumer, row->dependency, to);
        } else if (to_empty) {
            ok = xr_dependency_graph_remove_edge(
                graph, row->consumer, row->dependency);
        } else {
            ok = xr_dependency_graph_add_edge(
                graph, row->consumer, row->dependency, to);
        }
        if (!ok)
            return false;
    }
    return xr_dependency_graph_validate(graph);
}

static bool prospective_resolution_fingerprint(
    XrDependencyGraph *graph, XrStableId consumer,
    const XrDependencyGraphDelta *delta, XrFingerprint *out) {
    if (!apply_test_delta(graph, delta, true))
        return false;
    bool fingerprinted = xr_dependency_graph_module_resolution_fingerprint(
        graph, consumer, out);
    bool restored = apply_test_delta(graph, delta, false);
    return fingerprinted && restored;
}

static const XrInvalidationEvidence *find_evidence(
    const XrInvalidationResult *result, XrStableId module_id,
    XrStableId parent_id, XrModuleFacetMask observed_facet) {
    for (size_t i = 0; i < result->evidence_count; i++) {
        const XrInvalidationEvidence *evidence = &result->evidence[i];
        if (xr_stable_id_equal(evidence->module_id, module_id) &&
            xr_stable_id_equal(evidence->parent_module_id, parent_id) &&
            evidence->observed_facet == observed_facet) {
            return evidence;
        }
    }
    return NULL;
}

static bool apply_single_facet_change(XrDependencyGraph *graph, XrStableId root_id,
                                      XrModuleSummaryFacet facet, uint8_t seed,
                                      XrInvalidationResult *result) {
    const XrModuleSummary *stored = xr_dependency_graph_find_node(graph, root_id);
    XrModuleSummary replacement;
    if (!stored || !xr_module_summary_copy(&replacement, stored))
        return false;
    if (!xr_module_summary_set_fingerprint(&replacement, facet, test_fingerprint(seed))) {
        xr_module_summary_finalize(&replacement);
        return false;
    }
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = root_id,
        .replacement_summary = &replacement,
    };
    bool applied = xr_cache_invalidate_apply(graph, &event, result);
    xr_module_summary_finalize(&replacement);
    return applied;
}

TEST(module_summary_owns_key_and_distinguishes_facets) {
    char key[] = "pkg/summary";
    XrModuleSummary original;
    ASSERT_TRUE(init_test_summary(&original, key, 4));
    key[0] = 'X';
    ASSERT_STR_EQ(original.canonical_key, "pkg/summary");
    ASSERT_TRUE(xr_module_summary_validate(&original));

    XrModuleSummary changed;
    ASSERT_TRUE(xr_module_summary_copy(&changed, &original));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(
        &changed, XR_MODULE_FACET_BODY_EVIDENCE, test_fingerprint(200)));
    ASSERT_EQ_UINT(xr_module_summary_changed_facets(&original, &changed),
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(
        &changed, XR_MODULE_FACET_PUBLIC_SIGNATURE, test_fingerprint(201)));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(&changed, XR_MODULE_FACET_LAYOUT,
                                                  test_fingerprint(202)));
    ASSERT_EQ_UINT(xr_module_summary_changed_facets(&original, &changed),
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT));

    xr_module_summary_finalize(&changed);
    xr_module_summary_finalize(&original);
}

TEST(body_only_change_is_precise_and_reason_chain_is_traversable) {
    XrDependencyGraph graph;
    TestGraphIds ids;
    ASSERT_TRUE(build_facet_graph(&graph, &ids, false));
    XrFingerprint old_fingerprint;
    ASSERT_TRUE(xr_module_summary_fingerprint(
        xr_dependency_graph_find_node(&graph, ids.root), &old_fingerprint));

    XrInvalidationResult result;
    ASSERT_TRUE(apply_single_facet_change(&graph, ids.root, XR_MODULE_FACET_BODY_EVIDENCE, 210,
                                          &result));
    ASSERT_EQ_UINT(result.record_count, 3);
    ASSERT_TRUE(result_is_stably_ordered(&result));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.root));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.body));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.leaf));
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.signature));
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.layout));
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.untouched));

    const XrInvalidationRecord *leaf =
        xr_invalidation_result_find(&result, ids.leaf);
    const XrInvalidationRecord *body =
        xr_invalidation_result_find(&result, ids.body);
    ASSERT_NOT_NULL(leaf);
    ASSERT_NOT_NULL(body);
    ASSERT_EQ_UINT(leaf->evidence_count, 1);
    ASSERT_EQ_UINT(body->evidence_count, 1);
    ASSERT_NOT_NULL(find_evidence(
        &result, ids.leaf, ids.body,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE)));
    ASSERT_NOT_NULL(find_evidence(
        &result, ids.body, ids.root,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE)));
    const XrInvalidationRecord *root =
        xr_invalidation_result_find(&result, ids.root);
    ASSERT_NOT_NULL(root);
    ASSERT_EQ_UINT(root->evidence_count, 0);
    ASSERT_EQ_INT(root->direct_reason, XR_INVALIDATION_SUMMARY_CHANGED);
    ASSERT_EQ_INT(body->direct_reason, XR_INVALIDATION_DEPENDENCY);
    ASSERT_TRUE(xr_fingerprint_equal(result.root_old_fingerprint,
                                     old_fingerprint));
    XrFingerprint new_fingerprint;
    ASSERT_TRUE(xr_module_summary_fingerprint(
        xr_dependency_graph_find_node(&graph, ids.root), &new_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(result.root_new_fingerprint,
                                     new_fingerprint));
    ASSERT_FALSE(xr_fingerprint_equal(result.root_old_fingerprint,
                                      result.root_new_fingerprint));

    XrInvalidationExplanation explanation;
    ASSERT_TRUE(xr_invalidation_explain(
        &result, ids.leaf,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE), &explanation));
    ASSERT_TRUE(xr_stable_id_equal(explanation.root_id, ids.root));
    ASSERT_TRUE(xr_stable_id_equal(explanation.subject_id, ids.leaf));
    ASSERT_TRUE(xr_fingerprint_equal(explanation.root_old_fingerprint,
                                     result.root_old_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(explanation.root_new_fingerprint,
                                     result.root_new_fingerprint));
    ASSERT_EQ_INT(explanation.root_reason,
                  XR_INVALIDATION_SUMMARY_CHANGED);
    ASSERT_EQ_UINT(explanation.step_count, 2);
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].module_id,
                                   ids.leaf));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].parent_module_id,
                                   ids.body));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[1].module_id,
                                   ids.body));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[1].parent_module_id,
                                   ids.root));
    ASSERT_EQ_UINT(explanation.steps[0].invalidated_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE));
    ASSERT_EQ_UINT(explanation.steps[0].observed_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE));
    xr_invalidation_explanation_finalize(&explanation);

    xr_invalidation_result_finalize(&result);
    xr_dependency_graph_finalize(&graph);
}

TEST(summary_change_derives_every_changed_facet) {
    XrDependencyGraph graph;
    TestGraphIds ids;
    ASSERT_TRUE(build_facet_graph(&graph, &ids, false));
    const XrModuleSummary *stored = xr_dependency_graph_find_node(&graph, ids.root);
    ASSERT_NOT_NULL(stored);
    XrModuleSummary replacement;
    ASSERT_TRUE(xr_module_summary_copy(&replacement, stored));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(&replacement, XR_MODULE_FACET_BODY_EVIDENCE,
                                                  test_fingerprint(240)));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(&replacement, XR_MODULE_FACET_PUBLIC_SIGNATURE,
                                                  test_fingerprint(241)));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = ids.root,
        .replacement_summary = &replacement,
    };
    XrInvalidationResult result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &event, &result));
    const XrInvalidationRecord *root =
        xr_invalidation_result_find(&result, ids.root);
    ASSERT_NOT_NULL(root);
    ASSERT_EQ_UINT(root->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.body));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.signature));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&result, ids.leaf));
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.layout));
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.untouched));
    xr_invalidation_result_finalize(&result);
    xr_module_summary_finalize(&replacement);
    xr_dependency_graph_finalize(&graph);
}

TEST(public_and_layout_changes_do_not_reuse_body_closure) {
    XrDependencyGraph graph;
    TestGraphIds ids;
    ASSERT_TRUE(build_facet_graph(&graph, &ids, false));

    XrInvalidationResult signature_result;
    ASSERT_TRUE(apply_single_facet_change(&graph, ids.root, XR_MODULE_FACET_PUBLIC_SIGNATURE,
                                          220, &signature_result));
    ASSERT_EQ_UINT(signature_result.record_count, 2);
    ASSERT_NOT_NULL(xr_invalidation_result_find(&signature_result, ids.root));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&signature_result, ids.signature));
    ASSERT_NULL(xr_invalidation_result_find(&signature_result, ids.body));
    ASSERT_NULL(xr_invalidation_result_find(&signature_result, ids.leaf));
    ASSERT_NULL(xr_invalidation_result_find(&signature_result, ids.layout));
    xr_invalidation_result_finalize(&signature_result);

    XrInvalidationResult layout_result;
    ASSERT_TRUE(apply_single_facet_change(&graph, ids.root, XR_MODULE_FACET_LAYOUT, 230,
                                          &layout_result));
    ASSERT_EQ_UINT(layout_result.record_count, 2);
    ASSERT_NOT_NULL(xr_invalidation_result_find(&layout_result, ids.root));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&layout_result, ids.layout));
    ASSERT_NULL(xr_invalidation_result_find(&layout_result, ids.signature));
    ASSERT_NULL(xr_invalidation_result_find(&layout_result, ids.body));
    ASSERT_NULL(xr_invalidation_result_find(&layout_result, ids.leaf));
    xr_invalidation_result_finalize(&layout_result);

    xr_dependency_graph_finalize(&graph);
}

TEST(relation_rows_propagate_exact_consumer_facets) {
    XrDependencyGraph graph;
    xr_dependency_graph_init(&graph);
    XrStableId root_id;
    XrStableId consumer_id;
    XrStableId leaf_id;
    ASSERT_TRUE(add_test_module(&graph, "pkg/exact-root", 1, &root_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/exact-consumer", 20, &consumer_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/exact-leaf", 40, &leaf_id));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, consumer_id, root_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
            [XR_MODULE_FACET_PUBLIC_SIGNATURE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY),
        }));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, leaf_id, consumer_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
            [XR_MODULE_FACET_CAPABILITY] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        }));

    const XrModuleSummary *stored = xr_dependency_graph_find_node(&graph, root_id);
    XrModuleSummary replacement;
    ASSERT_NOT_NULL(stored);
    ASSERT_TRUE(xr_module_summary_copy(&replacement, stored));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(&replacement, XR_MODULE_FACET_BODY_EVIDENCE,
                                                  test_fingerprint(200)));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(&replacement, XR_MODULE_FACET_PUBLIC_SIGNATURE,
                                                  test_fingerprint(201)));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = root_id,
        .replacement_summary = &replacement,
    };
    XrInvalidationResult result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &event, &result));

    const XrInvalidationRecord *consumer = xr_invalidation_result_find(&result, consumer_id);
    const XrInvalidationRecord *leaf = xr_invalidation_result_find(&result, leaf_id);
    ASSERT_NOT_NULL(consumer);
    ASSERT_NOT_NULL(leaf);
    ASSERT_EQ_UINT(consumer->observed_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE));
    ASSERT_EQ_UINT(consumer->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY));
    ASSERT_EQ_UINT(leaf->observed_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY));
    ASSERT_EQ_UINT(leaf->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT));

    xr_invalidation_result_finalize(&result);
    xr_module_summary_finalize(&replacement);
    xr_dependency_graph_finalize(&graph);
}

TEST(invalid_relation_rows_are_rejected_without_mutating_graph) {
    XrDependencyGraph graph;
    xr_dependency_graph_init(&graph);
    XrStableId dependency_id;
    XrStableId consumer_id;
    ASSERT_TRUE(add_test_module(&graph, "pkg/relation-dependency", 1, &dependency_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/relation-consumer", 20, &consumer_id));
    XrModuleFacetMask valid[XR_MODULE_FACET_COUNT] = {0};
    valid[XR_MODULE_FACET_BODY_EVIDENCE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE);
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, consumer_id, dependency_id, valid));

    XrModuleFacetMask empty[XR_MODULE_FACET_COUNT] = {0};
    ASSERT_FALSE(xr_dependency_graph_add_edge(&graph, consumer_id, dependency_id, empty));
    ASSERT_TRUE(xr_dependency_graph_validate(&graph));
    const XrDependencyEdge *edge = xr_dependency_graph_edge_at(&graph, 0);
    ASSERT_NOT_NULL(edge);
    ASSERT_MEM_EQ(edge->relation, valid, sizeof(valid));

    XrModuleFacetMask invalid[XR_MODULE_FACET_COUNT] = {0};
    invalid[XR_MODULE_FACET_LAYOUT] = XR_MODULE_FACET_ALL | (XR_MODULE_FACET_ALL + 1u);
    ASSERT_FALSE(xr_dependency_graph_add_edge(&graph, consumer_id, dependency_id, invalid));
    ASSERT_TRUE(xr_dependency_graph_validate(&graph));
    ASSERT_MEM_EQ(edge->relation, valid, sizeof(valid));
    xr_dependency_graph_finalize(&graph);
}

TEST(edge_insertion_order_does_not_change_records) {
    XrDependencyGraph forward;
    XrDependencyGraph reverse;
    TestGraphIds forward_ids;
    TestGraphIds reverse_ids;
    ASSERT_TRUE(build_facet_graph(&forward, &forward_ids, false));
    ASSERT_TRUE(build_facet_graph(&reverse, &reverse_ids, true));
    XrFingerprint forward_graph_fingerprint;
    XrFingerprint reverse_graph_fingerprint;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(&forward,
                                                &forward_graph_fingerprint));
    ASSERT_TRUE(xr_dependency_graph_fingerprint(&reverse,
                                                &reverse_graph_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(forward_graph_fingerprint,
                                     reverse_graph_fingerprint));

    XrInvalidationResult forward_result;
    XrInvalidationResult reverse_result;
    ASSERT_TRUE(apply_single_facet_change(&forward, forward_ids.root,
                                          XR_MODULE_FACET_BODY_EVIDENCE, 240,
                                          &forward_result));
    ASSERT_TRUE(apply_single_facet_change(&reverse, reverse_ids.root,
                                          XR_MODULE_FACET_BODY_EVIDENCE, 240,
                                          &reverse_result));
    ASSERT_TRUE(result_is_stably_ordered(&forward_result));
    ASSERT_TRUE(result_is_stably_ordered(&reverse_result));
    ASSERT_TRUE(invalidation_results_equal(&forward_result, &reverse_result));

    xr_invalidation_result_finalize(&reverse_result);
    xr_invalidation_result_finalize(&forward_result);
    xr_dependency_graph_finalize(&reverse);
    xr_dependency_graph_finalize(&forward);
}

TEST(multi_parent_multi_facet_evidence_is_complete) {
    XrDependencyGraph graph;
    xr_dependency_graph_init(&graph);
    XrStableId root_id;
    XrStableId body_parent_id;
    XrStableId signature_parent_id;
    XrStableId sink_id;
    ASSERT_TRUE(add_test_module(&graph, "pkg/evidence-root", 1, &root_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/evidence-body", 20,
                                &body_parent_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/evidence-signature", 40,
                                &signature_parent_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/evidence-sink", 60, &sink_id));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, body_parent_id, root_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] =
                XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
        }));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, signature_parent_id, root_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_PUBLIC_SIGNATURE] =
                XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE),
        }));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, sink_id, body_parent_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] =
                XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        }));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, sink_id, signature_parent_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_PUBLIC_SIGNATURE] =
                XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT),
        }));

    const XrModuleSummary *stored =
        xr_dependency_graph_find_node(&graph, root_id);
    XrModuleSummary replacement;
    ASSERT_NOT_NULL(stored);
    ASSERT_TRUE(xr_module_summary_copy(&replacement, stored));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(
        &replacement, XR_MODULE_FACET_BODY_EVIDENCE, test_fingerprint(220)));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(
        &replacement, XR_MODULE_FACET_PUBLIC_SIGNATURE, test_fingerprint(221)));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = root_id,
        .replacement_summary = &replacement,
    };
    XrInvalidationResult result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &event, &result));

    const XrInvalidationRecord *sink =
        xr_invalidation_result_find(&result, sink_id);
    ASSERT_NOT_NULL(sink);
    ASSERT_EQ_UINT(sink->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT));
    ASSERT_EQ_UINT(sink->observed_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
                       XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE));
    ASSERT_EQ_UINT(sink->evidence_count, 2);
    const XrInvalidationEvidence *body_evidence = find_evidence(
        &result, sink_id, body_parent_id,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE));
    const XrInvalidationEvidence *signature_evidence = find_evidence(
        &result, sink_id, signature_parent_id,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE));
    ASSERT_NOT_NULL(body_evidence);
    ASSERT_NOT_NULL(signature_evidence);
    ASSERT_EQ_UINT(body_evidence->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT));
    ASSERT_EQ_UINT(signature_evidence->invalidated_facets,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT));

    XrInvalidationExplanation layout_explanation;
    ASSERT_TRUE(xr_invalidation_explain(
        &result, sink_id, XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        &layout_explanation));
    ASSERT_EQ_UINT(layout_explanation.step_count, 2);
    ASSERT_TRUE(xr_stable_id_equal(layout_explanation.steps[0].module_id,
                                   sink_id));
    ASSERT_TRUE(xr_stable_id_equal(
        layout_explanation.steps[0].parent_module_id, body_parent_id));
    ASSERT_EQ_UINT(layout_explanation.steps[0].invalidated_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT));
    ASSERT_EQ_UINT(layout_explanation.steps[1].invalidated_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE));
    xr_invalidation_explanation_finalize(&layout_explanation);

    XrInvalidationExplanation effect_explanation;
    ASSERT_TRUE(xr_invalidation_explain(
        &result, sink_id, XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT),
        &effect_explanation));
    ASSERT_EQ_UINT(effect_explanation.step_count, 2);
    ASSERT_TRUE(xr_stable_id_equal(
        effect_explanation.steps[0].parent_module_id,
        signature_parent_id));
    ASSERT_EQ_UINT(effect_explanation.steps[0].invalidated_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT));
    ASSERT_EQ_UINT(effect_explanation.steps[1].invalidated_facet,
                   XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE));
    xr_invalidation_explanation_finalize(&effect_explanation);

    xr_invalidation_result_finalize(&result);
    xr_module_summary_finalize(&replacement);
    xr_dependency_graph_finalize(&graph);
}

TEST(reason_explanation_terminates_on_cycles_and_rejects_forged_rows) {
    XrDependencyGraph graph;
    xr_dependency_graph_init(&graph);
    XrStableId root_id;
    XrStableId left_id;
    XrStableId right_id;
    XrStableId leaf_id;
    ASSERT_TRUE(add_test_module(&graph, "pkg/cycle-root", 1, &root_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/cycle-left", 20, &left_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/cycle-right", 40, &right_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/cycle-leaf", 60, &leaf_id));
    XrModuleFacetMask body_relation[XR_MODULE_FACET_COUNT] = {0};
    body_relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE);
    XrModuleFacetMask leaf_relation[XR_MODULE_FACET_COUNT] = {0};
    leaf_relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, left_id, root_id,
                                             body_relation));
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, right_id, left_id,
                                             body_relation));
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, left_id, right_id,
                                             body_relation));
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, leaf_id, right_id,
                                             leaf_relation));

    XrInvalidationResult result;
    ASSERT_TRUE(apply_single_facet_change(
        &graph, root_id, XR_MODULE_FACET_BODY_EVIDENCE, 210, &result));
    XrInvalidationExplanation explanation;
    ASSERT_TRUE(xr_invalidation_explain(
        &result, leaf_id, XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        &explanation));
    ASSERT_EQ_UINT(explanation.step_count, 3);
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].module_id, leaf_id));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].parent_module_id,
                                   right_id));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[1].module_id, right_id));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[1].parent_module_id,
                                   left_id));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[2].module_id, left_id));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[2].parent_module_id,
                                   root_id));
    xr_invalidation_explanation_finalize(&explanation);

    ASSERT_FALSE(xr_invalidation_explain(&result, leaf_id, 0, &explanation));
    ASSERT_FALSE(xr_invalidation_explain(
        &result, leaf_id,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT) |
            XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT),
        &explanation));
    ASSERT_FALSE(xr_invalidation_explain(
        &result, leaf_id, XR_MODULE_FACET_BIT(XR_MODULE_FACET_EFFECT),
        &explanation));

    ASSERT_TRUE(result.evidence_count != 0);
    XrModuleFacetMask saved_observed = result.evidence[0].observed_facet;
    result.evidence[0].observed_facet |=
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_FALSE(xr_invalidation_explain(
        &result, leaf_id, XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        &explanation));
    result.evidence[0].observed_facet = saved_observed;

    ASSERT_TRUE(xr_invalidation_explain(
        &result, root_id,
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE), &explanation));
    ASSERT_EQ_UINT(explanation.step_count, 0);
    ASSERT_NULL(explanation.steps);
    xr_invalidation_explanation_finalize(&explanation);

    xr_invalidation_result_finalize(&result);
    xr_dependency_graph_finalize(&graph);
}

TEST(module_resolution_delta_requires_exact_authority_and_is_atomic) {
    XrDependencyGraph graph;
    TestGraphIds ids;
    ASSERT_TRUE(build_facet_graph(&graph, &ids, false));
    XrModuleFacetMask downstream_relation[XR_MODULE_FACET_COUNT] = {0};
    downstream_relation[XR_MODULE_FACET_CAPABILITY] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, ids.leaf, ids.untouched, downstream_relation));
    XrFingerprint original_fingerprint;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(&graph, &original_fingerprint));

    XrDependencyGraphDeltaRow row = {
        .consumer = ids.untouched,
        .dependency = ids.body,
    };
    row.old_relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE);
    row.new_relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY);
    XrDependencyGraphDelta delta = {.rows = &row, .row_count = 1};
    XrModuleResolutionChange change = {
        .delta = &delta,
        .changed_facets =
            XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE) |
            XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY),
    };
    ASSERT_TRUE(xr_dependency_graph_module_resolution_fingerprint(
        &graph, ids.untouched, &change.old_fingerprint));
    change.new_fingerprint = change.old_fingerprint;
    change.new_fingerprint.bytes[0] ^= UINT8_C(1);
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_MODULE_RESOLUTION_CHANGED,
        .root_id = ids.untouched,
        .module_resolution = &change,
    };
    XrInvalidationResult rejected;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    XrFingerprint after_rejection;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(&graph, &after_rejection));
    ASSERT_TRUE(xr_fingerprint_equal(original_fingerprint, after_rejection));

    memset(row.old_relation, 0, sizeof(row.old_relation));
    change.changed_facets =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY);
    ASSERT_TRUE(prospective_resolution_fingerprint(
        &graph, ids.untouched, &delta, &change.new_fingerprint));
    delta.row_count = XR_INVALIDATION_MAX_DELTA_ROWS + 1u;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    delta.row_count = 1;

    change.old_fingerprint.bytes[0] ^= UINT8_C(1);
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    change.old_fingerprint.bytes[0] ^= UINT8_C(1);
    change.new_fingerprint.bytes[0] ^= UINT8_C(1);
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    change.new_fingerprint.bytes[0] ^= UINT8_C(1);
    change.changed_facets |= XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    change.changed_facets =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_CAPABILITY);
    event.root_id = ids.body;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    event.root_id = ids.untouched;
    row.consumer = ids.body;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    row.consumer = ids.untouched;
    event.module_resolution = NULL;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &rejected));
    event.module_resolution = &change;

    ASSERT_TRUE(xr_dependency_graph_fingerprint(&graph, &after_rejection));
    ASSERT_TRUE(xr_fingerprint_equal(original_fingerprint, after_rejection));

    XrDependencyGraph before;
    TestGraphIds before_ids;
    ASSERT_TRUE(build_facet_graph(&before, &before_ids, true));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &before, before_ids.leaf, before_ids.untouched,
        downstream_relation));
    XrInvalidationResult result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &event, &result));
    ASSERT_TRUE(xr_cache_invalidation_verify(
        &before, &event, &graph, &result));
    const XrDependencyEdge *published = xr_dependency_graph_find_edge(
        &graph, ids.untouched, ids.body);
    ASSERT_NOT_NULL(published);
    ASSERT_MEM_EQ(published->relation, row.new_relation,
                  sizeof(row.new_relation));
    ASSERT_TRUE(xr_fingerprint_equal(result.root_old_fingerprint,
                                     change.old_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(result.root_new_fingerprint,
                                     change.new_fingerprint));
    const XrInvalidationRecord *root =
        xr_invalidation_result_find(&result, ids.untouched);
    const XrInvalidationRecord *downstream =
        xr_invalidation_result_find(&result, ids.leaf);
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(downstream);
    ASSERT_TRUE(root->invalidated_facets == change.changed_facets);
    ASSERT_TRUE(downstream->invalidated_facets ==
                XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT));
    ASSERT_EQ_INT(root->direct_reason,
                  XR_INVALIDATION_MODULE_RESOLUTION_CHANGED);
    ASSERT_NULL(xr_invalidation_result_find(&result, ids.signature));

    XrInvalidationExplanation explanation;
    ASSERT_TRUE(xr_invalidation_explain(
        &result, ids.leaf, XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT),
        &explanation));
    ASSERT_EQ_INT(explanation.root_reason,
                  XR_INVALIDATION_MODULE_RESOLUTION_CHANGED);
    ASSERT_EQ_UINT(explanation.step_count, 1);
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].module_id,
                                   ids.leaf));
    ASSERT_TRUE(xr_stable_id_equal(explanation.steps[0].parent_module_id,
                                   ids.untouched));
    ASSERT_TRUE(xr_fingerprint_equal(explanation.root_old_fingerprint,
                                     change.old_fingerprint));
    ASSERT_TRUE(xr_fingerprint_equal(explanation.root_new_fingerprint,
                                     change.new_fingerprint));
    xr_invalidation_explanation_finalize(&explanation);

    XrDependencyGraph corrupt_budget = graph;
    corrupt_budget.edge_count = XR_DEPENDENCY_GRAPH_MAX_EDGES + 1u;
    ASSERT_FALSE(xr_dependency_graph_validate(&corrupt_budget));
    xr_invalidation_result_finalize(&result);
    xr_dependency_graph_finalize(&before);
    xr_dependency_graph_finalize(&graph);
}

TEST(independent_verifier_rejects_result_and_graph_mutations) {
    XrDependencyGraph before;
    XrDependencyGraph after;
    TestGraphIds before_ids;
    TestGraphIds after_ids;
    ASSERT_TRUE(build_facet_graph(&before, &before_ids, false));
    ASSERT_TRUE(build_facet_graph(&after, &after_ids, true));

    const XrModuleSummary *stored =
        xr_dependency_graph_find_node(&before, before_ids.root);
    XrModuleSummary replacement;
    ASSERT_NOT_NULL(stored);
    ASSERT_TRUE(xr_module_summary_copy(&replacement, stored));
    ASSERT_TRUE(xr_module_summary_set_fingerprint(
        &replacement, XR_MODULE_FACET_BODY_EVIDENCE, test_fingerprint(250)));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = before_ids.root,
        .replacement_summary = &replacement,
    };
    XrInvalidationResult result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&after, &event, &result));
    ASSERT_TRUE(xr_cache_invalidation_verify(&before, &event, &after, &result));

    result.root_old_fingerprint.bytes[0] ^= UINT8_C(1);
    ASSERT_FALSE(xr_cache_invalidation_verify(&before, &event, &after, &result));
    result.root_old_fingerprint.bytes[0] ^= UINT8_C(1);
    ASSERT_TRUE(xr_cache_invalidation_verify(&before, &event, &after, &result));

    ASSERT_TRUE(result.evidence_count != 0);
    result.evidence[0].invalidated_facets ^=
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_FALSE(xr_cache_invalidation_verify(&before, &event, &after, &result));
    result.evidence[0].invalidated_facets ^=
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_TRUE(xr_cache_invalidation_verify(&before, &event, &after, &result));

    ASSERT_TRUE(result.record_count != 0);
    result.records[0].observed_facets ^=
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_FALSE(xr_cache_invalidation_verify(&before, &event, &after, &result));
    result.records[0].observed_facets ^=
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT);
    ASSERT_TRUE(xr_cache_invalidation_verify(&before, &event, &after, &result));

    ASSERT_TRUE(xr_dependency_graph_remove_edge(&after, after_ids.untouched,
                                                after_ids.root));
    ASSERT_FALSE(xr_cache_invalidation_verify(&before, &event, &after, &result));

    xr_invalidation_result_finalize(&result);
    xr_module_summary_finalize(&replacement);
    xr_dependency_graph_finalize(&after);
    xr_dependency_graph_finalize(&before);
}

TEST(delete_rename_add_and_module_resolution_leave_no_ghost_nodes) {
    XrDependencyGraph graph;
    xr_dependency_graph_init(&graph);
    XrStableId root_id;
    XrStableId consumer_id;
    XrStableId unrelated_id;
    ASSERT_TRUE(add_test_module(&graph, "pkg/delete-root", 1, &root_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/consumer", 20, &consumer_id));
    ASSERT_TRUE(add_test_module(&graph, "pkg/unrelated", 40, &unrelated_id));
    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, consumer_id, root_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
            [XR_MODULE_FACET_PUBLIC_SIGNATURE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
            [XR_MODULE_FACET_LAYOUT] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
            [XR_MODULE_FACET_CAPABILITY] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
        }));

    XrInvalidationEvent deletion = {
        .reason = XR_INVALIDATION_MODULE_DELETED,
        .root_id = root_id,
    };
    XrInvalidationResult deletion_result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &deletion, &deletion_result));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&deletion_result, root_id));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&deletion_result, consumer_id));
    ASSERT_NULL(xr_invalidation_result_find(&deletion_result, unrelated_id));
    ASSERT_NULL(xr_dependency_graph_find_node(&graph, root_id));
    ASSERT_EQ_UINT(graph.edge_count, 0);
    xr_invalidation_result_finalize(&deletion_result);

    XrModuleSummary added;
    ASSERT_TRUE(init_test_summary(&added, "pkg/added", 60));
    XrStableId added_id = added.module_id;
    XrInvalidationEvent addition = {
        .reason = XR_INVALIDATION_MODULE_ADDED,
        .root_id = added.module_id,
        .replacement_summary = &added,
    };
    XrInvalidationResult addition_result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &addition, &addition_result));
    ASSERT_EQ_UINT(addition_result.record_count, 1);
    ASSERT_NOT_NULL(xr_dependency_graph_find_node(&graph, added.module_id));
    xr_invalidation_result_finalize(&addition_result);
    xr_module_summary_finalize(&added);

    ASSERT_TRUE(xr_dependency_graph_add_edge(
        &graph, unrelated_id, consumer_id,
        (XrModuleFacetMask[XR_MODULE_FACET_COUNT]) {
            [XR_MODULE_FACET_BODY_EVIDENCE] = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
        }));
    XrModuleSummary renamed;
    ASSERT_TRUE(init_test_summary(&renamed, "pkg/renamed-consumer", 80));
    XrInvalidationEvent rename = {
        .reason = XR_INVALIDATION_MODULE_RENAMED,
        .root_id = consumer_id,
        .replacement_summary = &renamed,
    };
    XrInvalidationResult rename_result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &rename, &rename_result));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&rename_result, consumer_id));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&rename_result, unrelated_id));
    ASSERT_NULL(xr_dependency_graph_find_node(&graph, consumer_id));
    ASSERT_NOT_NULL(xr_dependency_graph_find_node(&graph, renamed.module_id));
    for (size_t i = 0; i < graph.edge_count; i++) {
        ASSERT_FALSE(xr_stable_id_equal(graph.edges[i].consumer, consumer_id));
        ASSERT_FALSE(xr_stable_id_equal(graph.edges[i].dependency, consumer_id));
    }
    xr_invalidation_result_finalize(&rename_result);

    XrModuleFacetMask body_relation[XR_MODULE_FACET_COUNT] = {0};
    body_relation[XR_MODULE_FACET_BODY_EVIDENCE] =
        XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE);
    XrDependencyGraphDeltaRow delta_row = {
        .consumer = renamed.module_id,
        .dependency = added_id,
    };
    memcpy(delta_row.new_relation, body_relation, sizeof(body_relation));
    XrDependencyGraphDelta delta = {
        .rows = &delta_row,
        .row_count = 1,
    };
    XrModuleResolutionChange resolution_change = {
        .delta = &delta,
        .changed_facets =
            XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
    };
    ASSERT_TRUE(xr_dependency_graph_module_resolution_fingerprint(
        &graph, renamed.module_id, &resolution_change.old_fingerprint));
    ASSERT_TRUE(prospective_resolution_fingerprint(
        &graph, renamed.module_id, &delta,
        &resolution_change.new_fingerprint));
    XrInvalidationEvent resolution_event = {
        .reason = XR_INVALIDATION_MODULE_RESOLUTION_CHANGED,
        .root_id = renamed.module_id,
        .module_resolution = &resolution_change,
    };
    XrInvalidationResult graph_result;
    ASSERT_TRUE(xr_cache_invalidate_apply(
        &graph, &resolution_event, &graph_result));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&graph_result, renamed.module_id));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&graph_result, unrelated_id));
    ASSERT_TRUE(result_is_stably_ordered(&graph_result));
    ASSERT_NOT_NULL(xr_dependency_graph_find_edge(
        &graph, renamed.module_id, added_id));
    ASSERT_FALSE(xr_fingerprint_equal(graph_result.root_old_fingerprint,
                                      graph_result.root_new_fingerprint));
    xr_invalidation_result_finalize(&graph_result);

    xr_module_summary_finalize(&renamed);
    xr_dependency_graph_finalize(&graph);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Module summary and invalidation graph");
RUN_TEST(module_resolution_fingerprint_is_consumer_exact_and_order_independent);
RUN_TEST(module_summary_owns_key_and_distinguishes_facets);
RUN_TEST(body_only_change_is_precise_and_reason_chain_is_traversable);
RUN_TEST(summary_change_derives_every_changed_facet);
RUN_TEST(public_and_layout_changes_do_not_reuse_body_closure);
RUN_TEST(relation_rows_propagate_exact_consumer_facets);
RUN_TEST(invalid_relation_rows_are_rejected_without_mutating_graph);
RUN_TEST(edge_insertion_order_does_not_change_records);
RUN_TEST(multi_parent_multi_facet_evidence_is_complete);
RUN_TEST(module_resolution_delta_requires_exact_authority_and_is_atomic);
RUN_TEST(independent_verifier_rejects_result_and_graph_mutations);
RUN_TEST(delete_rename_add_and_module_resolution_leave_no_ghost_nodes);
RUN_TEST(reason_explanation_terminates_on_cycles_and_rejects_forged_rows);
TEST_MAIN_END()
