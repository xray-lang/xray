/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_summary_build.c - Module summary derivation contract tests
 */

#include "incremental/xr_cache_invalidate.h"
#include "incremental/xr_module_summary_build.h"
#include "test_framework.h"

#include <string.h>

static XrFingerprint fingerprint(const char *text) {
    XrFingerprint result;
    xr_semantic_fingerprint((const uint8_t *) text, strlen(text), &result);
    return result;
}

static XrModuleSummaryFacts base_facts(void) {
    XrModuleSummaryFacts facts;
    memset(&facts, 0, sizeof(facts));
    facts.semantics = fingerprint("semantics");
    facts.dependencies = fingerprint("dependencies");
    facts.declarations = fingerprint("declarations");
    facts.target = fingerprint("target");
    facts.toolchain = fingerprint("toolchain");
    facts.configuration = fingerprint("configuration");
    facts.semantic_schema = 7u;
    return facts;
}

static bool facet_equals(const XrModuleSummary *summary, XrModuleSummaryFacet facet,
                         XrFingerprint expected) {
    XrFingerprint actual;
    return xr_module_summary_get_fingerprint(summary, facet, &actual) &&
           xr_fingerprint_equal(actual, expected);
}

TEST(identical_facts_produce_identical_summary_and_key) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary first;
    XrModuleSummary second;
    XrCacheKey first_key;
    XrCacheKey second_key;

    ASSERT_TRUE(xr_module_summary_build(&first, &first_key, "app/main", &facts));
    ASSERT_TRUE(xr_module_summary_build(&second, &second_key, "app/main", &facts));

    XrFingerprint first_digest;
    XrFingerprint second_digest;
    ASSERT_TRUE(xr_module_summary_fingerprint(&first, &first_digest));
    ASSERT_TRUE(xr_module_summary_fingerprint(&second, &second_digest));
    ASSERT_TRUE(xr_fingerprint_equal(first_digest, second_digest));
    ASSERT_TRUE(xr_cache_key_equal(first_key, second_key));
    ASSERT_EQ_INT((int) xr_module_summary_changed_facets(&first, &second), 0);

    xr_module_summary_finalize(&first);
    xr_module_summary_finalize(&second);
}

/* An absent facet cannot register as changed, so a summary that left one out
 * would silently stop invalidation from propagating along an edge observing it. */
TEST(every_facet_is_present_and_sourced_from_a_named_authority) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary summary;
    XrCacheKey key;
    ASSERT_TRUE(xr_module_summary_build(&summary, &key, "app/main", &facts));
    ASSERT_TRUE(summary.present_facets == XR_MODULE_FACET_ALL);

    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_BODY_EVIDENCE, facts.semantics));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_PUBLIC_SIGNATURE, facts.semantics));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_OWNERSHIP, facts.semantics));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_DEBUG_MAPPING, facts.semantics));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_TARGET, facts.target));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_PROVIDER, facts.target));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_RUNTIME_ABI, facts.target));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_COMPILER, facts.toolchain));
    ASSERT_TRUE(facet_equals(&summary, XR_MODULE_FACET_CONFIG, facts.configuration));
    xr_module_summary_finalize(&summary);
}

TEST(semantic_change_invalidates_every_module_local_facet) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary before;
    XrModuleSummary after;
    XrCacheKey before_key;
    XrCacheKey after_key;
    ASSERT_TRUE(xr_module_summary_build(&before, &before_key, "app/main", &facts));

    facts.semantics = fingerprint("semantics-2");
    ASSERT_TRUE(xr_module_summary_build(&after, &after_key, "app/main", &facts));
    ASSERT_FALSE(xr_cache_key_equal(before_key, after_key));

    XrModuleFacetMask changed = xr_module_summary_changed_facets(&before, &after);
    ASSERT_TRUE((changed & XR_MODULE_FACET_BIT(XR_MODULE_FACET_BODY_EVIDENCE)) != 0);
    ASSERT_TRUE((changed & XR_MODULE_FACET_BIT(XR_MODULE_FACET_PUBLIC_SIGNATURE)) != 0);
    ASSERT_TRUE((changed & XR_MODULE_FACET_BIT(XR_MODULE_FACET_LAYOUT)) != 0);
    ASSERT_TRUE((changed & XR_MODULE_FACET_BIT(XR_MODULE_FACET_TARGET)) == 0);
    ASSERT_TRUE((changed & XR_MODULE_FACET_BIT(XR_MODULE_FACET_COMPILER)) == 0);
    xr_module_summary_finalize(&before);
    xr_module_summary_finalize(&after);
}

TEST(target_change_moves_only_profile_owned_facets) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary before;
    XrModuleSummary after;
    XrCacheKey before_key;
    XrCacheKey after_key;
    ASSERT_TRUE(xr_module_summary_build(&before, &before_key, "app/main", &facts));

    facts.target = fingerprint("target-2");
    ASSERT_TRUE(xr_module_summary_build(&after, &after_key, "app/main", &facts));
    ASSERT_FALSE(xr_cache_key_equal(before_key, after_key));

    XrModuleFacetMask changed = xr_module_summary_changed_facets(&before, &after);
    XrModuleFacetMask expected = XR_MODULE_FACET_BIT(XR_MODULE_FACET_TARGET) |
                                 XR_MODULE_FACET_BIT(XR_MODULE_FACET_PROVIDER) |
                                 XR_MODULE_FACET_BIT(XR_MODULE_FACET_RUNTIME_ABI);
    ASSERT_TRUE(changed == expected);
    xr_module_summary_finalize(&before);
    xr_module_summary_finalize(&after);
}

/* A verified plan fingerprint already binds its ordered dependency rows, so
 * these components never move alone in a real build. The cache identity still
 * separates them, because an artifact must not be reachable through a key that
 * does not name its exact dependency and declaration set. */
TEST(dependency_and_declaration_identity_separate_the_cache_key) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary summary;
    XrCacheKey base_key;
    XrCacheKey dependency_key;
    XrCacheKey declaration_key;
    XrCacheKey schema_key;

    ASSERT_TRUE(xr_module_summary_build(&summary, &base_key, "app/main", &facts));
    xr_module_summary_finalize(&summary);

    facts.dependencies = fingerprint("dependencies-2");
    ASSERT_TRUE(xr_module_summary_build(&summary, &dependency_key, "app/main", &facts));
    xr_module_summary_finalize(&summary);
    ASSERT_FALSE(xr_cache_key_equal(base_key, dependency_key));

    facts = base_facts();
    facts.declarations = fingerprint("declarations-2");
    ASSERT_TRUE(xr_module_summary_build(&summary, &declaration_key, "app/main", &facts));
    xr_module_summary_finalize(&summary);
    ASSERT_FALSE(xr_cache_key_equal(base_key, declaration_key));

    facts = base_facts();
    facts.semantic_schema = 8u;
    ASSERT_TRUE(xr_module_summary_build(&summary, &schema_key, "app/main", &facts));
    ASSERT_FALSE(xr_cache_key_equal(base_key, schema_key));
    ASSERT_TRUE(summary.present_facets == XR_MODULE_FACET_ALL);
    xr_module_summary_finalize(&summary);
}

TEST(distinct_modules_never_share_a_cache_identity) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary first;
    XrModuleSummary second;
    XrCacheKey first_key;
    XrCacheKey second_key;
    ASSERT_TRUE(xr_module_summary_build(&first, &first_key, "app/main", &facts));
    ASSERT_TRUE(xr_module_summary_build(&second, &second_key, "app/helper", &facts));

    ASSERT_FALSE(xr_stable_id_equal(first.module_id, second.module_id));
    /* Two modules with identical semantics share one artifact identity: the
     * artifact is content-addressed, while the summary keeps module identity. */
    ASSERT_TRUE(xr_cache_key_equal(first_key, second_key));
    xr_module_summary_finalize(&first);
    xr_module_summary_finalize(&second);
}

TEST(invalid_derivation_inputs_are_rejected) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleSummary summary;
    XrCacheKey key;
    ASSERT_FALSE(xr_module_summary_build(&summary, &key, "", &facts));
    ASSERT_FALSE(xr_module_summary_build(&summary, &key, NULL, &facts));
    ASSERT_FALSE(xr_module_summary_build(&summary, &key, "app/main", NULL));
    ASSERT_FALSE(xr_module_summary_build(NULL, &key, "app/main", &facts));
    ASSERT_FALSE(xr_module_summary_build(&summary, NULL, "app/main", &facts));
}

/* The wiring a real build performs: derive one summary per module, connect
 * them with the conservative observation relation, then prove a body edit in a
 * leaf reaches every transitive consumer. */
TEST(published_graph_propagates_a_leaf_change_to_every_consumer) {
    XrModuleSummaryFacts facts = base_facts();
    XrModuleFacetMask relation[XR_MODULE_FACET_COUNT];
    XrDependencyGraph graph;
    XrModuleSummary leaf;
    XrModuleSummary middle;
    XrModuleSummary root;
    XrCacheKey unused_key;

    xr_module_summary_full_relation(relation);
    for (unsigned facet = 0; facet < XR_MODULE_FACET_COUNT; facet++)
        ASSERT_TRUE(relation[facet] == XR_MODULE_FACET_ALL);

    xr_dependency_graph_init(&graph);
    facts.semantics = fingerprint("leaf");
    ASSERT_TRUE(xr_module_summary_build(&leaf, &unused_key, "app/leaf", &facts));
    facts.semantics = fingerprint("middle");
    ASSERT_TRUE(xr_module_summary_build(&middle, &unused_key, "app/middle", &facts));
    facts.semantics = fingerprint("root");
    ASSERT_TRUE(xr_module_summary_build(&root, &unused_key, "app/root", &facts));

    ASSERT_TRUE(xr_dependency_graph_add_node(&graph, &leaf));
    ASSERT_TRUE(xr_dependency_graph_add_node(&graph, &middle));
    ASSERT_TRUE(xr_dependency_graph_add_node(&graph, &root));
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, middle.module_id, leaf.module_id, relation));
    ASSERT_TRUE(xr_dependency_graph_add_edge(&graph, root.module_id, middle.module_id, relation));
    ASSERT_TRUE(xr_dependency_graph_validate(&graph));

    XrFingerprint before;
    XrFingerprint after;
    ASSERT_TRUE(xr_dependency_graph_fingerprint(&graph, &before));

    XrModuleSummary edited;
    facts.semantics = fingerprint("leaf-edited");
    ASSERT_TRUE(xr_module_summary_build(&edited, &unused_key, "app/leaf", &facts));
    XrInvalidationEvent event = {
        .reason = XR_INVALIDATION_SUMMARY_CHANGED,
        .root_id = leaf.module_id,
        .replacement_summary = &edited,
    };
    XrInvalidationResult result;
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(xr_cache_invalidate_apply(&graph, &event, &result));

    const XrInvalidationRecord *middle_record =
        xr_invalidation_result_find(&result, middle.module_id);
    const XrInvalidationRecord *root_record = xr_invalidation_result_find(&result, root.module_id);
    ASSERT_TRUE(middle_record != NULL);
    ASSERT_TRUE(root_record != NULL);
    ASSERT_TRUE(middle_record->invalidated_facets == XR_MODULE_FACET_ALL);
    ASSERT_TRUE(root_record->invalidated_facets == XR_MODULE_FACET_ALL);

    ASSERT_TRUE(xr_dependency_graph_fingerprint(&graph, &after));
    ASSERT_FALSE(xr_fingerprint_equal(before, after));

    xr_invalidation_result_finalize(&result);
    xr_module_summary_finalize(&edited);
    xr_module_summary_finalize(&leaf);
    xr_module_summary_finalize(&middle);
    xr_module_summary_finalize(&root);
    xr_dependency_graph_finalize(&graph);
}

TEST_MAIN_BEGIN()
    RUN_TEST(identical_facts_produce_identical_summary_and_key);
    RUN_TEST(every_facet_is_present_and_sourced_from_a_named_authority);
    RUN_TEST(semantic_change_invalidates_every_module_local_facet);
    RUN_TEST(target_change_moves_only_profile_owned_facets);
    RUN_TEST(dependency_and_declaration_identity_separate_the_cache_key);
    RUN_TEST(distinct_modules_never_share_a_cache_identity);
    RUN_TEST(invalid_derivation_inputs_are_rejected);
    RUN_TEST(published_graph_propagates_a_leaf_change_to_every_consumer);
TEST_MAIN_END()
