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
    if (left->record_count != right->record_count)
        return false;
    for (size_t i = 0; i < left->record_count; i++) {
        const XrInvalidationRecord *a = &left->records[i];
        const XrInvalidationRecord *b = &right->records[i];
        if (!xr_stable_id_equal(a->module_id, b->module_id) ||
            a->invalidated_facets != b->invalidated_facets ||
            a->observed_facets != b->observed_facets || a->direct_reason != b->direct_reason ||
            a->has_parent != b->has_parent ||
            (a->has_parent && !xr_stable_id_equal(a->parent_module_id, b->parent_module_id))) {
            return false;
        }
    }
    return true;
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
        .old_fingerprint = test_fingerprint((uint8_t) (seed - 1u)),
        .new_fingerprint = test_fingerprint(seed),
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

    const XrInvalidationRecord *leaf = xr_invalidation_result_find(&result, ids.leaf);
    ASSERT_TRUE(leaf->has_parent);
    ASSERT_TRUE(xr_stable_id_equal(leaf->parent_module_id, ids.body));
    const XrInvalidationRecord *body =
        xr_invalidation_result_find(&result, leaf->parent_module_id);
    ASSERT_NOT_NULL(body);
    ASSERT_TRUE(body->has_parent);
    ASSERT_TRUE(xr_stable_id_equal(body->parent_module_id, ids.root));
    const XrInvalidationRecord *root =
        xr_invalidation_result_find(&result, body->parent_module_id);
    ASSERT_NOT_NULL(root);
    ASSERT_FALSE(root->has_parent);
    ASSERT_EQ_INT(root->direct_reason, XR_INVALIDATION_SUMMARY_CHANGED);
    ASSERT_EQ_INT(body->direct_reason, XR_INVALIDATION_DEPENDENCY);
    ASSERT_TRUE(xr_fingerprint_equal(result.root_old_fingerprint, test_fingerprint(209)));
    ASSERT_TRUE(xr_fingerprint_equal(result.root_new_fingerprint, test_fingerprint(210)));

    xr_invalidation_result_finalize(&result);
    xr_dependency_graph_finalize(&graph);
}

TEST(summary_change_rejects_caller_mask_that_omits_a_changed_facet) {
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
        .changed_facets = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
        .replacement_summary = &replacement,
    };
    XrInvalidationResult result;
    ASSERT_FALSE(xr_cache_invalidate_apply(&graph, &event, &result));
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

TEST(delete_rename_add_and_graph_change_leave_no_ghost_nodes) {
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
        .old_fingerprint = test_fingerprint(1),
        .new_fingerprint = test_fingerprint(2),
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
    XrInvalidationEvent addition = {
        .reason = XR_INVALIDATION_MODULE_ADDED,
        .root_id = added.module_id,
        .old_fingerprint = test_fingerprint(3),
        .new_fingerprint = test_fingerprint(4),
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
        .changed_facets = XR_MODULE_FACET_ALL,
        .old_fingerprint = test_fingerprint(5),
        .new_fingerprint = test_fingerprint(6),
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

    XrInvalidationEvent graph_change = {
        .reason = XR_INVALIDATION_GRAPH_CHANGED,
        .root_id = renamed.module_id,
        .changed_facets = XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE),
        .old_fingerprint = test_fingerprint(7),
        .new_fingerprint = test_fingerprint(8),
    };
    XrInvalidationResult graph_result;
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &graph_change, &graph_result));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&graph_result, renamed.module_id));
    ASSERT_NOT_NULL(xr_invalidation_result_find(&graph_result, unrelated_id));
    ASSERT_TRUE(result_is_stably_ordered(&graph_result));
    xr_invalidation_result_finalize(&graph_result);

    xr_module_summary_finalize(&renamed);
    xr_dependency_graph_finalize(&graph);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Module summary and invalidation graph");
RUN_TEST(module_summary_owns_key_and_distinguishes_facets);
RUN_TEST(body_only_change_is_precise_and_reason_chain_is_traversable);
RUN_TEST(summary_change_rejects_caller_mask_that_omits_a_changed_facet);
RUN_TEST(public_and_layout_changes_do_not_reuse_body_closure);
RUN_TEST(relation_rows_propagate_exact_consumer_facets);
RUN_TEST(invalid_relation_rows_are_rejected_without_mutating_graph);
RUN_TEST(edge_insertion_order_does_not_change_records);
RUN_TEST(delete_rename_add_and_graph_change_leave_no_ghost_nodes);
TEST_MAIN_END()
