/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cache_key.c - Incremental cache key contract tests
 */

#include "incremental/xr_cache_key.h"
#include "test_framework.h"

#include <string.h>

static XrCacheFingerprint fingerprint(const char *text) {
    XrCacheFingerprint result;
    xr_cache_fingerprint_bytes((const uint8_t *) text, strlen(text), &result);
    return result;
}

static XrSemanticCacheKeyInput semantic_input(void) {
    XrSemanticCacheKeyInput input = {
        .normalized_source = fingerprint("source"),
        .compiler = fingerprint("compiler"),
        .semantic_schema = fingerprint("semantic-schema"),
        .contract = fingerprint("contract"),
        .language_configuration = fingerprint("language-config"),
        .semantic_dependencies = fingerprint("dependencies"),
        .declaration_identities = fingerprint("declarations"),
    };
    return input;
}

TEST(semantic_key_is_deterministic_and_domain_separated) {
    XrSemanticCacheKeyInput input = semantic_input();
    XrCacheKey first;
    XrCacheKey second;
    xr_cache_key_semantic(&input, &first);
    xr_cache_key_semantic(&input, &second);
    ASSERT_TRUE(xr_cache_key_equal(first, second));

    XrTargetCacheKeyInput target = {
        .semantic_plan = input.normalized_source,
        .target_profile = input.compiler,
        .provider_capabilities = input.semantic_schema,
        .runtime_abi = input.contract,
        .planner_schema = input.language_configuration,
        .optimization_budget = input.semantic_dependencies,
    };
    XrCacheKey target_key;
    xr_cache_key_target(&target, &target_key);
    ASSERT_FALSE(xr_cache_key_equal(first, target_key));
}

TEST(semantic_key_changes_only_for_explicit_input) {
    XrSemanticCacheKeyInput base = semantic_input();
    XrSemanticCacheKeyInput changed = base;
    XrCacheKey base_key;
    XrCacheKey changed_key;
    xr_cache_key_semantic(&base, &base_key);

    changed.language_configuration = fingerprint("language-config-2");
    xr_cache_key_semantic(&changed, &changed_key);
    ASSERT_FALSE(xr_cache_key_equal(base_key, changed_key));

    changed = base;
    changed.semantic_dependencies = fingerprint("dependencies-2");
    xr_cache_key_semantic(&changed, &changed_key);
    ASSERT_FALSE(xr_cache_key_equal(base_key, changed_key));
}

TEST(target_key_tracks_target_contract_without_schema_copy) {
    XrTargetCacheKeyInput input = {
        .semantic_plan = fingerprint("semantic-plan"),
        .target_profile = fingerprint("target-profile"),
        .provider_capabilities = fingerprint("provider-capabilities"),
        .runtime_abi = fingerprint("runtime-abi"),
        .planner_schema = fingerprint("planner-schema"),
        .optimization_budget = fingerprint("optimization-budget"),
    };
    XrCacheKey base;
    XrCacheKey changed;
    xr_cache_key_target(&input, &base);
    input.provider_capabilities = fingerprint("provider-capabilities-2");
    xr_cache_key_target(&input, &changed);
    ASSERT_FALSE(xr_cache_key_equal(base, changed));
}

TEST(hex_round_trip_rejects_noncanonical_text) {
    XrSemanticCacheKeyInput input = semantic_input();
    XrCacheKey key;
    XrCacheKey parsed;
    char hex[XR_CACHE_KEY_HEX_SIZE];
    xr_cache_key_semantic(&input, &key);
    xr_cache_key_hex(key, hex);
    ASSERT_TRUE(xr_cache_key_from_hex(hex, &parsed));
    ASSERT_TRUE(xr_cache_key_equal(key, parsed));

    hex[0] = 'A';
    ASSERT_FALSE(xr_cache_key_from_hex(hex, &parsed));
    ASSERT_FALSE(xr_cache_key_from_hex("00", &parsed));
}

TEST_MAIN_BEGIN()
    RUN_TEST(semantic_key_is_deterministic_and_domain_separated);
    RUN_TEST(semantic_key_changes_only_for_explicit_input);
    RUN_TEST(target_key_tracks_target_contract_without_schema_copy);
    RUN_TEST(hex_round_trip_rejects_noncanonical_text);
TEST_MAIN_END()
