/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xglobal_cache_payload.c - Unit tests for evidence cache payload parsing
 */

#include "../test_framework.h"
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/base/xmalloc.h"
#include <string.h>

static void add_sample_body_summary(XgGlobalEvidence *ev) {
    XgDeclSummary decl = {0};
    XgBodySummary body = {0};
    XgCallsiteSummary call = {0};

    decl.decl_id = 1;
    decl.module_id = 7;
    decl.kind = XG_DECL_FUNC;
    decl.name_id = xg_name_id("entry");
    decl.signature_key = 101;
    decl.source_span_id = 3;
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(ev, &decl));

    body.func_id = 11;
    body.module_id = 7;
    body.owner_decl_id = decl.decl_id;
    body.name_id = decl.name_id;
    body.signature_key = decl.signature_key;
    body.source_span_id = 3;
    body.kind = XG_BODY_FUNCTION;
    body.body_hash = UINT64_C(0x123456789abcdef0);
    body.effect_bits = XG_BODY_MAY_CALL;
    body.callsite_start = 1;
    body.callsite_count = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_body(ev, &body));

    call.callsite_id = 1;
    call.owner_func_id = body.func_id;
    call.source_span_id = 4;
    call.body_ordinal = 1;
    call.kind = XG_CALL_DIRECT_FUNC;
    call.static_target_func_id = body.func_id;
    call.method_name_id = xg_name_id("entry");
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(ev, &call));
}

TEST(cache_payload_parse_exposes_validated_body) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCachePayloadInfo info;
    XgEvidenceCacheKey expected;
    char *payload;

    ev.key.module_id = 7;
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_body_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    ASSERT(xg_evidence_cache_payload_parse(payload, &info));
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT(xg_evidence_cache_key_matches(&info.key, &expected));
    ASSERT_EQ_UINT(info.phase, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_EQ_UINT(info.key_hash, xg_evidence_cache_key_hash(&expected));
    ASSERT_EQ_UINT(info.body_len, info.payload_bytes);
    ASSERT_NOT_NULL(strstr(info.body, "payload-count bodies=1 callsites=1"));
    ASSERT_NOT_NULL(strstr(info.body, "body id=11"));
    ASSERT(xg_evidence_cache_payload_matches(payload, &expected));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_parse_rejects_body_drift) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCachePayloadInfo info;
    char *payload;
    char *needle;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);

    needle = strstr(payload, "body id=11");
    ASSERT_NOT_NULL(needle);
    needle[8] = '2';
    ASSERT(!xg_evidence_cache_payload_parse(payload, &info));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_matches_rejects_wrong_phase_key) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCacheKey wrong_phase;
    char *payload;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    wrong_phase = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(!xg_evidence_cache_payload_matches(payload, &wrong_phase));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Global Evidence Cache Payload");
RUN_TEST(cache_payload_parse_exposes_validated_body);
RUN_TEST(cache_payload_parse_rejects_body_drift);
RUN_TEST(cache_payload_matches_rejects_wrong_phase_key);

TEST_MAIN_END()
