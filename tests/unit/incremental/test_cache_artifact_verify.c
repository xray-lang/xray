/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cache_artifact_verify.c - Exact cache-hit plan verification
 */

#include "../../../src/incremental/xr_cache_artifact_verify.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_module.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/format/xr_xtp_schema.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/target/xr_target_builder.h"
#include "../../../src/runtime/value/xtype.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

typedef struct CacheArtifactFixture {
    XrSemanticPlan *semantic;
    XrTargetProfile *profile;
    XrTargetPlan *plan;
    uint8_t *bytes;
    size_t size;
    XrCacheXtpArtifactVerifyContext requirements;
    XrCacheKey key;
} CacheArtifactFixture;

static XrType cache_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static XrSemanticPlan *build_semantic(const char *name, int64_t value) {
    XiFunc *function = xi_func_new(name, &cache_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *constant = xi_const_int(function, entry, value, &cache_int);
    REQUIRE(constant != NULL);
    xi_block_set_return(entry, constant);
    function->stage = XI_STAGE_OPTIMIZED;
    function->module = xi_module_new("fixture/cache_artifact_verify.xr", name, function);
    REQUIRE(function->module != NULL);
    REQUIRE(xi_module_set_identity(
        function->module, "memory-module-v1:id=29:cache-artifact-verify-fixture"));
    XrSemanticPlan *semantic = NULL;
    char error[512] = {0};
    bool built = xr_semantic_plan_build(function, &semantic, error, sizeof(error));
    if (!built)
        fprintf(stderr, "semantic fixture build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return semantic;
}

static XrCacheKey xsm_key(const char *identity) {
    XrCacheFingerprint fingerprint;
    XrCacheKey key;
    xr_cache_fingerprint_bytes((const uint8_t *) identity, strlen(identity),
                               &fingerprint);
    memcpy(key.bytes, fingerprint.bytes, sizeof(key.bytes));
    return key;
}

static void test_exact_xsm_cache_hit_and_authority_mutations(void) {
    XrSemanticPlan *semantic =
        build_semantic("cache_xsm_probe", INT64_C(42));
    XrSemanticPlan *other =
        build_semantic("cache_xsm_other", INT64_C(43));
    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(semantic, &bytes, &size, error, sizeof(error)));

    XrCacheKey key = xsm_key("cache-xsm-probe-key");
    XrCacheXsmArtifactVerifyContext requirements = {
        .expected_key = key,
        .semantic_plan = semantic,
    };
    REQUIRE(xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, &requirements));

    XrCacheKey wrong_key = key;
    wrong_key.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, wrong_key, bytes, size, &requirements));
    requirements.expected_key = wrong_key;
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, &requirements));
    requirements.expected_key = key;
    requirements.semantic_plan = other;
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, &requirements));
    requirements.semantic_plan = semantic;

    const XrSemanticPlan *wrong_dependencies[] = {other};
    requirements.semantic_dependencies = wrong_dependencies;
    requirements.semantic_dependency_count = 1;
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, &requirements));
    requirements.semantic_dependencies = NULL;
    requirements.semantic_dependency_count = 0;

    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, NULL));
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XTP, key, bytes, size, &requirements));
    bytes[size - 1u] ^= UINT8_C(1);
    REQUIRE(!xr_cache_verify_xsm_artifact(
        XR_CACHE_ARTIFACT_XSM, key, bytes, size, &requirements));

    xr_free(bytes);
    xr_semantic_plan_free(other);
    xr_semantic_plan_free(semantic);
}

static CacheArtifactFixture make_fixture(void) {
    CacheArtifactFixture fixture = {0};
    fixture.semantic = build_semantic("cache_xtp_probe", INT64_C(42));
    fixture.profile = xr_test_target_profile_build(
        false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(fixture.profile != NULL);
    char error[512] = {0};
    REQUIRE(xr_target_plan_build(fixture.semantic, fixture.profile, &fixture.plan,
                                 error, sizeof(error)));
    REQUIRE(xr_xtp_encode_plan(fixture.plan, &fixture.bytes, &fixture.size, error,
                               sizeof(error)));
    fixture.requirements.semantic_plan = fixture.semantic;
    fixture.requirements.target_profile = fixture.profile;
    static const uint8_t budget[] = "baseline-optimization-budget-v1";
    xr_cache_fingerprint_bytes(budget, sizeof(budget) - 1u,
                               &fixture.requirements.optimization_budget);
    REQUIRE(xr_cache_xtp_key(&fixture.requirements, &fixture.key));
    return fixture;
}

static void dispose_fixture(CacheArtifactFixture *fixture) {
    xr_xtp_encoded_free(fixture->bytes);
    xr_target_plan_free(fixture->plan);
    xr_target_profile_free(fixture->profile);
    xr_semantic_plan_free(fixture->semantic);
    memset(fixture, 0, sizeof(*fixture));
}

static void test_exact_xtp_cache_hit(void) {
    CacheArtifactFixture fixture = make_fixture();
    XrCacheKey repeated = {{0}};
    REQUIRE(xr_cache_xtp_key(&fixture.requirements, &repeated));
    REQUIRE(xr_cache_key_equal(fixture.key, repeated));
    REQUIRE(xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, fixture.bytes, fixture.size,
        &fixture.requirements));
    XrCacheXtpArtifactLoadContext load = {
        .requirements = fixture.requirements,
    };
    REQUIRE(xr_cache_materialize_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, fixture.bytes, fixture.size,
        &load));
    REQUIRE(load.accepted_plan != NULL);
    REQUIRE(xr_target_plan_semantic_plan(load.accepted_plan) == fixture.semantic);
    REQUIRE(xr_cache_key_equal(fixture.key, repeated));
    REQUIRE(!xr_cache_materialize_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, fixture.bytes, fixture.size,
        &load));
    xr_target_plan_free(load.accepted_plan);
    dispose_fixture(&fixture);
}

static void test_key_and_artifact_mutations_fail_closed(void) {
    CacheArtifactFixture fixture = make_fixture();
    XrCacheKey wrong_key = fixture.key;
    wrong_key.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, wrong_key, fixture.bytes, fixture.size,
        &fixture.requirements));
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XSM, fixture.key, fixture.bytes, fixture.size,
        &fixture.requirements));

    fixture.bytes[fixture.size - 1u] ^= UINT8_C(1);
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, fixture.bytes, fixture.size,
        &fixture.requirements));
    fixture.bytes[fixture.size - 1u] ^= UINT8_C(1);

    XrCacheXtpArtifactVerifyContext changed_budget = fixture.requirements;
    changed_budget.optimization_budget.bytes[0] ^= UINT8_C(1);
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, fixture.bytes, fixture.size,
        &changed_budget));
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, fixture.key, NULL, 0, &fixture.requirements));
    REQUIRE(!xr_cache_xtp_key(NULL, &wrong_key));
    REQUIRE(!xr_cache_xtp_key(&fixture.requirements, NULL));
    dispose_fixture(&fixture);
}

static void test_wrong_semantic_and_profile_authorities_fail_closed(void) {
    CacheArtifactFixture fixture = make_fixture();
    XrSemanticPlan *other_semantic =
        build_semantic("cache_xtp_other", INT64_C(43));
    XrCacheXtpArtifactVerifyContext other_requirements = fixture.requirements;
    other_requirements.semantic_plan = other_semantic;
    XrCacheKey other_key = {{0}};
    REQUIRE(xr_cache_xtp_key(&other_requirements, &other_key));
    REQUIRE(!xr_cache_key_equal(fixture.key, other_key));
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, other_key, fixture.bytes, fixture.size,
        &other_requirements));

    XrTargetProfile *other_profile = xr_test_target_profile_build(
        true, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    REQUIRE(other_profile != NULL);
    other_requirements = fixture.requirements;
    other_requirements.target_profile = other_profile;
    REQUIRE(xr_cache_xtp_key(&other_requirements, &other_key));
    REQUIRE(!xr_cache_key_equal(fixture.key, other_key));
    REQUIRE(!xr_cache_verify_xtp_artifact(
        XR_CACHE_ARTIFACT_XTP, other_key, fixture.bytes, fixture.size,
        &other_requirements));

    xr_target_profile_free(other_profile);
    xr_semantic_plan_free(other_semantic);
    dispose_fixture(&fixture);
}

int main(void) {
    test_exact_xsm_cache_hit_and_authority_mutations();
    test_exact_xtp_cache_hit();
    test_key_and_artifact_mutations_fail_closed();
    test_wrong_semantic_and_profile_authorities_fail_closed();
    puts("test_cache_artifact_verify: ok");
    return 0;
}
